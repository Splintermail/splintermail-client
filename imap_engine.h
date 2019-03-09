#ifndef IMAP_ENGINE_H
#define IMAP_ENGINE_H

#include "ix.h"
#include "tls_engine.h"
#include "queue.h"
#include "common.h"

/* The IMAP engine is the end of the libuv-tls-imap pipeline, but it interacts
   with other IMAP sessions.  Primarily, these interactions would be things
   like a IMAP server session asking an IMAP client session if the local copy
   of a folder is up-to-date or not.


    PIPELINE API       _____________   CROSS-SESSION "API"   _____________
                      |             |                       |             |
   (1) --------> READ |             |                       |             |
       READ_DONE <--- |             | --------> REQUEST (4) |   ANOTHER   |
                      | IMAP ENGINE | REQ_DONE <--- (3)     | IMAP ENGINE |
       WRITE <------- |             |                       |             |
   (2) --> WRITE_DONE |             |                       |             |
                      |_____________|                       |_____________|

   Additional notes:
     - none yet.

*/

derr_t imape_init(void);
void imape_free(void);

void imape_read(ixs_t *ixs);
void imape_write_done(ixs_t *ixs);

// internal functions, but we will list them here for now
void handle_read(void); // all reads pass through this hook

void cmd_capability(void); // hard coded response
void cmd_noop(void); // requires sync
void cmd_logout(void); // triggers several checks, may require sync
// void cmd_starttls(void); // not implemented for now
// void cmd_authenticate(void); // not implemented for now
void cmd_login(void); // either starts a user context or associates with one
void cmd_select(void); // might launch a new imap client
void cmd_examine(void); // might launch a new imap client
void cmd_create(void); // definitely requires a sync
void cmd_delete(void); // sync
void cmd_rename(void); // sync
void cmd_subscribe(void); // wtf is this good for??
void cmd_unsubscribe(void); // wtf is this good for??
void cmd_list(void); // might require a sync?
void cmd_lsub(void); // might not require a sync?
void cmd_status(void); // sync
void cmd_append(void); // definite sync
void cmd_check(void); // sync I guess, but its a noop locally
void cmd_close(void); // might close an imap client, or some expunging (meaning a sync)
void cmd_expunge(void); // sync
void cmd_search(void); // sync for new messages, but ultimately handled locally
void cmd_fetch(void); // sync if message not found recognized, otherwise handled locally
void cmd_store(void); // sync
void cmd_copy(void); //
void cmd_uid(void); // UID copies of some of the above functions

void resp_status(void); /* an OK, NO, BAD, PREAUTH, or BYE, with optional status code.
                  Might also trigger a command-completed action */
void resp_capability(void); // make sure the capabilities we want are there
void resp_list(void); // Call a LIST hook, I guess?
// void resp_lsub(void); // not going to implement this
// void resp_search(void); // not going to implement this
void resp_flags(void); // make sure the flags we want are there
void resp_exists(void); // at any time, basically
void resp_recent(void); // this will be ignored?  I think I only care about exists
void resp_expunge(void);
void resp_fetch(void); // at any time, basically

/*
IMAP Engine state:
*/

typedef struct {
    // where to place user folders
    string_builder_t basepath;
    // where to connect to
    dstr_t url;
    unsigned int port;
} imape_t;

/*
IMAP connected user registry
This per-application structure keeps track of:
  - list of users connected to the application
  - track one keybox-watcher imap connection per user
  - track open folders, into which imap sessions have views
*/

// typedef struct {
//
// } imap_user_t;

// add to the imap session context

    // SELECTED

    /* things that need to be stored on the filesystem:
         - MESSAGES, UIDS, FLAGS
         - UIDVALIDITY
    */

    /* need to keep track of the messages that we have and the messages that
       the server has.  Messages not yet downloaded will be given placeholders.
       That makes the asynchronous handling of messages easy.

       Where to put injected messages?  I suppose the only way to handle them
       in an IMAP-cannonical way would be to give them their own folder, since
       in all other cases every email client needs to see the same thing in
       any other folder.
    */


#endif // IMAP_ENGINE_H
