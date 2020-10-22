/*
The imap maildir is a major part of the logic in the system.  In a pure imap
client, the application logic only has to LIST folders, then sequentially open
an imaildir_t for every folder, assigning the upwards imap session to the
folder, then wait for the synchronized signal.  The imaildir_t does all of the
downloading.

In a DITM setting, the application logic has an upwards and a downwards
session.  When the downwards session asks to select a mailbox, the application
logic opens an imaildir_t and passes handles for both sessions.  Then the
imaildir_t uses the upwards session to sync itself (and stream live updates
from the mailserver), and it is capable of responding to all the imap commands
in the SELECTED state from its own store.

The imaildir_t implements all of the per-message details of all of the imap
server.

imaildir_t has two state machines, a downloader and a server.

The downloader is responsible for getting the contents of the mailbox
synchronized with the mail server and keeping it synchronized.  There may be
many connections upwards, but only one is ever active.  That state machine
looks like this:


    state: wait for conn_up
      |
    event: maildir receives a conn_up
      | ______________________________________________
      |/                                              |
      |                                               |
    state: issue SELECT, and complete initial sync    |
      |
      |\___                                           ^
      |    |                                          |
      |  event: sync interrupted by close request     |
      |     \_____________________________________    |
      |                                           |   |
    signal: indicate we are done synchronizing    |   |
      |      (signal sent to all conn_up's)       v   |
      |                                           |   |
    state: issue IDLE commands and get responses  |
      |                                           v   ^
    event: IDLE interrupted by a close request    |   |
      | __________________________________________|   |
      |/                                              |
      |                                               |
    state: issue CLOSE and get response               |
      |                                               |
    signal: indicate connection is not in use         |
      |      (signal sent to just one conn_up)
      |                                               ^
    decision: do we have more conn_up's?              |
      |\___                                           |
      |    |                                          |
      |   yes: return to initial sync state           |
      |    |__________________________________________|
      |
    no: start over with wating for a conn_up


The server state machine is responsible for implementing the IMAP server.
There may be many conn_dn's, and there will be one server state machine for
every conn_dn.  That state machine looks like this:

    event: maildir recieves a conn_dn (state machine begins)
      |
    decision: are we serving in DITM mode?
      |\
   no | \ yes
      |  \
      | state: wait to initial sync
      |   |
      | event: sync complete
      |  /
      | /
      |/
    signal: maildir is open (respond to SELECT command)
      | __________________________________
      |/                                  |
    state: wait for command from below    ^
      |    or maildir event from above    |
      |\___                               |
      |    |                              |
      |  event: command or event          ^
      |    |    (handle it appropriately) |
      |    |______________________________|
      |
    event: close command (state machine ends)

*/

/* E_IMAILDIR means that the imaildir is in an invalid state.  This will be
   thrown by an imaildir function when imaildir_fail() was called before
   exiting the function.  As a result, the accessor is likely to be in a closed
   state before that imaildir function returns, and it may be desirable in
   that accessor's close function to detect this error and keep this stack
   trace, which will be more informative than the original stack trace from
   when imaildir_fail() triggered this accessor's close function. */
extern derr_type_t E_IMAILDIR;

struct maildir_log_i;
typedef struct maildir_log_i maildir_log_i;

struct imaildir_cb_i;
typedef struct imaildir_cb_i imaildir_cb_i;

// forward declaration of dirmgr_hold_t
struct dirmgr_hold_t;
typedef struct dirmgr_hold_t dirmgr_hold_t;

struct imaildir_cb_i {
    // verify that we are allowed to download right now
    bool (*allow_download)(imaildir_cb_i*, imaildir_t*);
    derr_t (*dirmgr_hold_new)(
        imaildir_cb_i*, const dstr_t *name, dirmgr_hold_t **out
    );
};

// IMAP maildir
struct imaildir_t {
    imaildir_cb_i *cb;
    // path to this maildir on the filesystem
    string_builder_t path;
    // the name of this box for SELECT purposes
    const dstr_t *name;
    unsigned int uid_validity;
    unsigned int hi_uid_dn; // starts at 0 for empty boxes
    // mailbox flags
    ie_mflags_t mflags;
    // crypto for this box
    const keypair_t *keypair;
    // has this imaildir synced yet? (subsequent sync's don't send updates)
    bool synced;

    // if SELECT returned NO, delete the box afterwards
    bool rm_on_close;
    bool closed;

    // accessors
    link_t ups;  // up_t->link;
    link_t dns;  // dn_t->link;
    /* The value of naccessors may not match the length of the accessor
       lists, particularly during the failing shutdown sequence. */
    size_t naccessors;
    // one writer for every examine=false dn_t
    size_t nwriters;

    /* if the primary up_t is disconnecting, we save commands that we would
       pass through it for the next up_t.  The UNSELECT is synchronous though,
       so that if up_t has any useful commands in flight in front of the
       UNSELECT, those will be completed before we take a new primary.

       That of course, does not help us in the case of a truly broken
       connection to the mail server.  In that case, ideally we should just
       break our connection with the mail client, and let the mail client's
       replay logic take over.

       In the case the mail client disconnects, we should just make sure to let
       the corresponding upwards connection continue to live long enough to
       disconnect gracefully, since there may be a secondary downwards
       connection whose commands are being relayed through that upwards
       connection, and closing it abruptly may introduce spurious duplicated
       commands in the replay logic (or spurious disconnects for the secondary
       downwards connection, if we passed the broken connection to it without
       any replay logic).

       All things considered, the only IMAP commands which cannot safely be
       duplicated are APPEND, COPY, and RENAME (I think).

       Fortunately, APPEND is already handled as a passthru command so this
       should not be a concern there. */
    link_t relays;  // relay_t->link
    // the id in the tag of commands originating with us
    size_t tag;

    // because we execute the file copying on behalf of the dn_t
    struct {
        // when
        void *requester;
    } copy;

    // did we open via imaildir_init_lite()?
    bool lite;
    jsw_atree_t msgs;  // msg_t->node, keyed by uid_up
    jsw_atree_t mods;  // msg_mod_t->node
    jsw_atree_t expunged;  // msg_expunge_t->node;
    maildir_log_i *log;
    // the latest serial of things we put in /tmp
    size_t tmp_count;
    link_t updates_requested;  // update_req_t->link
    // link_t updates_in_flight;  // update_base_t->link
};

// open a maildir at path, path and name must be linked to long-lived objects
derr_t imaildir_init(imaildir_t *m, imaildir_cb_i *cb, string_builder_t path,
        const dstr_t *name, const keypair_t *keypair);

/* open an imaildir without reading files on disk.  The imaildir is only
   allowed to be used for imaildir_add_local_file() */
derr_t imaildir_init_lite(imaildir_t *m, string_builder_t path);

// free must only be called if the maildir has no accessors
void imaildir_free(imaildir_t *m);

// useful if an open maildir needs to be deleted
void imaildir_forceclose(imaildir_t *m);

// regsiter a new connection with the imaildir, and return a maildir_i
void imaildir_register_up(imaildir_t *m, up_t *up);

// regsiter a new connection with the imaildir, and return a maildir_i
void imaildir_register_dn(imaildir_t *m, dn_t *dn);

/* if you got a maildir_*_i through dirmgr_open, you should close it via
   dirmgr_close */
// returns number of accessors after unregister operation
size_t imaildir_unregister_up(up_t *up);
size_t imaildir_unregister_dn(dn_t *dn);

/* Synchronous filesystem-backed storage for an imap maildir.

   Message content is stored in maildir format, and is managed directly by the
   imaildir_t.  These functions are for things not easily stored in maildir
   format.
*/
struct maildir_log_i {
    unsigned int (*get_uidvld_up)(maildir_log_i*);
    unsigned int (*get_uidvld_dn)(maildir_log_i*);
    derr_t (*set_uidvlds)(
        maildir_log_i*, unsigned int uidvld_up, unsigned int uidvld_dn
    );

    // the highest modseq we have synced from above, or 1 if we've seen nothing
    unsigned long (*get_himodseq_up)(maildir_log_i*);
    derr_t (*set_himodseq_up)(maildir_log_i*, unsigned long himodseq_up);

    // store the up-to-date message
    derr_t (*update_msg)(maildir_log_i*, const msg_t *msg);

    // store the up-to-date expunge
    derr_t (*update_expunge)(maildir_log_i*, const msg_expunge_t *expunge);

    // close and free the log
    void (*close)(maildir_log_i*);
};

// this must be implemented by some backend, currently only lmdb
derr_t imaildir_log_open(const string_builder_t *dirpath,
        jsw_atree_t *msgs_out, jsw_atree_t *expunged_out,
        jsw_atree_t *mods_out, maildir_log_i **log_out);

// this must be implemented by some backend, currently only lmdb
derr_t imaildir_log_rm(const string_builder_t *dirpath);


/////////////////
/* imaildir functions exposed only for up_t.  up_t does not keep its own view
   of the imaildir, and so it will read and write directly to the imaildir_t.
   This works because only one up_t is ever active for an imaildir_t. */

/* these two are called during up_imaildir_select() for the up_t to populate
   its internal seq_set_builder_t's */
derr_t imaildir_up_get_unfilled_msgs(imaildir_t *m, seq_set_builder_t *ssb);
derr_t imaildir_up_get_unpushed_expunges(imaildir_t *m, ie_seq_set_t **out);

derr_t imaildir_up_check_uidvld_up(imaildir_t *m, unsigned int uidvld_up);

derr_t imaildir_up_set_himodseq_up(imaildir_t *m, unsigned long himodseq);

// return the msg if it exists and if it is expunged
msg_t *imaildir_up_lookup_msg(imaildir_t *m, unsigned int uid_up,
        bool *expunged);

// add a new message to the maildir
derr_t imaildir_up_new_msg(imaildir_t *m, unsigned int uid, msg_flags_t flags,
        msg_t **out);

// update flags for an existing message
derr_t imaildir_up_update_flags(imaildir_t *m, msg_t *msg, msg_flags_t flags);

// handle the static attributes from a FETCH
derr_t imaildir_up_handle_static_fetch_attr(imaildir_t *m,
        msg_t *msg, const ie_fetch_resp_t *fetch);

derr_t imaildir_up_initial_sync_complete(imaildir_t *m, up_t *up);

derr_t imaildir_up_delete_msg(imaildir_t *m, unsigned int uid_up);

// verify that we are allowed to download messages right now
bool imaildir_up_allow_download(imaildir_t *m);

/////////////////
/* imaildir functions exposed only for dn_t.  dn_t keeps its own view of the
   mailbox and therefore relies less on the imaildir_t. */

derr_t imaildir_dn_build_views(imaildir_t *m, jsw_atree_t *views,
        unsigned int *max_uid_dn, unsigned int *uidvld);

// this will always consume or free req
derr_t imaildir_dn_request_update(imaildir_t *m, update_req_t *req);

// open a message in a thread-safe way; return a file descriptor
derr_t imaildir_dn_open_msg(imaildir_t *m, unsigned int uid_up, int *fd);

// close a message in a thread-safe way; return the result of close()
derr_t imaildir_dn_close_msg(imaildir_t *m, unsigned int uid_up, int *fd,
        int *ret);

/////////////////
// support for APPEND and COPY (without redownloading message)

// add a file to an open imaildir_t (rename or remove path)
// if uidvld is invalid, this will silently delete the file.
derr_t imaildir_add_local_file(
    imaildir_t *m,
    const string_builder_t *path,
    unsigned int uidvld_up,
    unsigned int uid_up,
    size_t len,
    imap_time_t intdate,
    msg_flags_t flags
);

// the dirmgr should call this, not the owner of the hold
void imaildir_hold_end(imaildir_t *m);
