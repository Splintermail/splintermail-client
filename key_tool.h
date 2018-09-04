#ifndef KEY_TOOL_H
#define KEY_TOOL_H

#include "common.h"
#include "api_client.h"
#include "json.h"
#include "crypto.h"
#include "fixed_lengths.h"

/* The transition from user-managed keys to auto-managed keys was absolutely
   necessary from a user interface point of view.  However the resulting
   auto-managed key system is inherently complicated.  That's why it has been
   pulled out into its own module, even though scope-wise it would fit neatly
   into DITM.  Additionally, a separate key_handling module allows for
   independent testing of DITM and the key handling.

   REQUIRED FEATURES:
    1) Every device has its own key
    2) Keys generated on device, to protect user
    3) Use password given by email client to self-register (transparent UX)
    4) Device alerts user of every new device
    5) Device ignores things not encrypted to it after only 1 download
   DESIRED FEATURES:
    6) If removed, a device can reregister its old key to prevent loss of email
        -> this allows for a safe delete-all-devices-and-change-password flow
        ---> this depends on the server-side unreadable-msg-cleanup procedure

   DEFINITIONS (relative to the device on which DITM is running):
    "peer":
        any other device registered to the account, identified by public key
    "old peer":
        a peer which we are used to seeing as a recipient of encryption, and
        which is still registered to the server.
    "peer list"
        This is simply a list of old peers that persists between sessions.
        When this list is updated, it is only updated by contact with the
        server.  That is, changes are detected without contacting the server
        but after detecting a change, the peer list will be updated to match
        the server's list_devices endpoint, as that is the exact list of which
        peers should be expected in future sessions.  However, using the
        server's list of device does not affect the new device alerts given to
        the user.
    "new peer":
        a peer identified for the first time during this DITM session.  There
        are two mechanisms by which a new peer might be detected: by seeing a
        new recipient to encryption, or by seeing a new device after syncing
        the peer list to the server.  At the end of the session, the user will
        be alerted to each new peer, regardless of how it was identified.
    "expired peer":
        a peer which has been deleted from the user's account since the last
        DITM session.  It's possible for a new peer to also be an expired peer,
        but it doesn't affect the new device alerts, or the update mechanism of
        the peer list.
    "ignore list":
        A list of message UID's for messages which are encrypted but not to us.
        The list must be persistent between sessions.  If a UID is found to be
        missing on the server it should be removed from the list.

   PEER LIST STATES:
    - new:
        Indicates that no peer list was found on file.  We will load the peer
        list from the server, but for this session only there is no value in
        alerting the user to new keys.
    - old:
        Indicate that the peer list was on file but that we haven't called the
        list_devices API endpoint yet.  If we identify a new peer or an
        expired peer, at the end of downloading messages we will go through the
        VERIFY FLOW, where we will set the peer list to the server's device
        list, meaning that in future sessions we won't accidentally send the
        user duplicate "new device" alerts

   DITM SESSION FLOW (implemented mostly in loginhook() of ditm.c):
    - DITM recieves connection with username and password
    - Login to remote server successful (filters out password errors)
    - enter LOAD OR GEN KEY FLOW
    - enter LOAD PEER LIST FLOW
    - load ignore list from file
    - enter DOWNLOAD EMAIL FLOW for each email on remote server
    - write the updated ignore list (dropping any UIDs we didn't see)
    - enter VERIFY FLOW if necessary
    - inject alert messages for new peers and stray peers (ft 4)
    - lastly, interact with email client

   LOAD OR GEN KEY FLOW (implemented by key_tool_load()):
    - If have a key on file, set key state to unverified and exit
    - If not, create a new key (ft 1, 2)
    - push it to the server over the add_device API endpoint (ft 3)
    - key state set to verfied
    - store the resulting API token

   LOAD PEER LIST FLOW (implemented by key_tool_load()):
    - if able to load peer list on file
        - peer list state = old
    - else
        - peer list state = new

   DOWNLOAD EMAIL FLOW (implemented by ditm_download()):
    - download a message (that is not on the ignore list, ft 5)
    - if not encrypted:
        - set flag for VERIFY FLOW to verify peer list
        - pass message to user with "NOT ENCRYPTED" in the subject line
    - if encrypted, enter EMAIL DECRYPTION FLOW

   EMAIL DECRYPTION FLOW (implemented by key_tool_decrypt()):
    - If email is corrupted (unparsable) just pass it to the user (sorry, user)
    - Check recipients on the email
    - If there's a new device, a new peer and set flag for VERIFY FLOW
    - If there is a missing device (including ours) set flag for VERIFY FLOW
    - If not encrypted to our key add UID to ignore list

   VERIFY FLOW (implemented by key_tool_verify()):
      (verify validity of our copy of the peer list)
    - use password to check list_devices API endpoint
    - if our key is not listed:
        - reregister with the add_device API endpoint
        - call list_devices again (remove this?)
    - if peer list state == new
        - set our peer list to match server peer list
    - else if peer list state == old
        - any peers on server list not on peer list are added to new peers
        - set our peer list to match server peer list

   Note: If we know our key is new (and there's nothing we can decrypt), we
   still need to download everything to get unencrypted mail, which looks the
   same from the UID.  Maybe some day that will be fixable, with a custom flag
   in the UID or something to that effect.
*/

/* SECURITY LIMITATION:

   I guess somebody could add a key to your account temporarily, and intercept
   all of your mail and delete it immediately so that your devices never
   suspected anything, and then delete their key.  You need out-of-band
   messaging (like a confirmation email address) to defend against that,
   because in-band messaging is subject to deletion by peer devices.

   Well... I guess you could also keep parallel mail stores but that would be
   counterproductive to the normal delete-once-for-all-devices workflow.

   Or... you could keep a history of key actions (encrypted of course) as
   undeletable special emails, and DITM could filter those out and use them
   to verify the keys it is seeing.  I think I will implement this with IMAP,
   since some sort of similar mechanism will be required for IMAP-DITM to
   encrypt sent mail anyways

   However, if you use normal IMAP features to share those keys, then somebody
   could connect to the encrypted IMAP hosted on the server and delete all of
   the special emails, so there would have to be some sort of permissions
   handling on the server side
*/

typedef enum {
    KT_PL_NEW,
    KT_PL_OLD,
} key_tool_peer_list_state_t;

typedef struct {
    // this will be something like "/home/user/.ditm/user@splintermail"
    char dir_buffer[4096];
    dstr_t dir;
    // the decryption key
    keypair_t key;
    bool did_key_gen;
    // dynamic backing memory for json
    dstr_t json_block;
    // dynamic, used for various json parsing tasks
    LIST(json_t) json;
    // list of peers that we are expecting to see as encryption recipients
    LIST(dstr_t) peer_list;
    key_tool_peer_list_state_t peer_list_state;
    // list of peers that the user needs to be alerted about
    LIST(dstr_t) new_peer_list;
    bool found_expired_peer;
    // the decryption object
    decrypter_t dc;
} key_tool_t;

derr_t key_tool_new(key_tool_t* kt, const dstr_t* dir, int def_key_bits);
/* throws: E_FS
           E_INTERNAL (implies SSL errors)
           E_NOMEM */

void key_tool_free(key_tool_t* kt);

derr_t key_tool_update(key_tool_t* kt, const char* host, unsigned int port,
                       const dstr_t* user, const dstr_t* pass);
/* throws: E_NOMEM (on fopen)
           E_FS
           E_OS (reading/writing an opened file)
           E_INTERNAL
           E_RESPONSE (bad response from the API server)
           E_PARAM (host, username, or password too long)
           E_SSL (bad server certificate)
           E_CONN (failed or broken connection with server) */

derr_t key_tool_decrypt(key_tool_t* kt, int infd, int outfd, size_t* outlen);
/* throws: E_NOMEM
           E_OS (read/write from already-opened file)
           E_INTERNAL
           E_PARAM (from decrypter, bad message)
           E_NOT4ME (from decrypter)
           E_FS (unable to write token) */

// functions which are exposed for testing and should not otherwise be called
derr_t key_tool_peer_list_load(key_tool_t* kt, const char* filename);
/* throws: E_NOMEM
           E_FS (failed to open/read file)
           E_PARAM (bad json found in file)
           */

derr_t key_tool_peer_list_write(key_tool_t* kt, const char* filename);
/* throws: E_FS    (from fopen)
           E_NOMEM (from fopen)
           E_OS (fwrite)
           E_INTERNAL */

derr_t key_tool_check_recips(key_tool_t* kt, LIST(dstr_t)* recips);
/* throws: E_NOMEM */

#endif // KEY_TOOL_H
