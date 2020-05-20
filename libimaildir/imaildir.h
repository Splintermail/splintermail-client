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

typedef enum {
    IMAILDIR_WAITING_FOR_CONN = 0,
    IMAILDIR_INITIAL_SYNC,
    IMAILDIR_IDLE,
    IMAILDIR_CLOSING,
} downloader_state_e;

struct maildir_conn_up_i;
typedef struct maildir_conn_up_i maildir_conn_up_i;
struct maildir_conn_dn_i;
typedef struct maildir_conn_dn_i maildir_conn_dn_i;
struct maildir_up_i;
typedef struct maildir_up_i maildir_up_i;
struct maildir_dn_i;
typedef struct maildir_dn_i maildir_dn_i;
struct imaildir_t;
typedef struct imaildir_t imaildir_t;
struct maildir_log_i;
typedef struct maildir_log_i maildir_log_i;

/* the upwards session must provide this interface to the maildir; it is how
   the maildir will communicate the imap client behavior */
struct maildir_conn_up_i {
    // the maildir wants to pass an imap command over the wire
    void (*cmd)(maildir_conn_up_i*, imap_cmd_t*);
    // this event indiates a SELECT finished, with an ie_st_resp_t if it failed
    // (if select fails, you should go straight to dirmgr_close_up())
    void (*selected)(maildir_conn_up_i*, ie_st_resp_t*);
    // this event indicates the maildir finished an initial sync
    void (*synced)(maildir_conn_up_i*);
    // this event is a response to the maildir_up_i->unselect() call
    void (*unselected)(maildir_conn_up_i*);
    // The maildir BROADCASTs its failures when it dies
    /* note the asymmetry: the maildir resource can tell the actor owning the
       conn_up that the actor must die with this call, but there is no call for
       the actor to tell the shared resource to die */
    void (*failure)(maildir_conn_up_i*, derr_t);
    // this is always the last call to the interface
    void (*release)(maildir_conn_up_i*);
};

/* the downwards session must provide this interface to the maildir; it is how
   the maildir will communicate the imap server behavior */
struct maildir_conn_dn_i {
    // the maildir wants to pass an imap response over the wire
    void (*resp)(maildir_conn_dn_i*, imap_resp_t*);
    // the maildir has some processing that needs to happen on-thread
    void (*advance)(maildir_conn_dn_i*);
    // The maildir BROADCASTs its failures when it dies
    /* note the asymmetry: the maildir resource can tell the actor owning the
       conn_up that the actor must die with this call, but there is no call for
       the actor to tell the shared resource to die */
    void (*failure)(maildir_conn_dn_i*, derr_t);
    // this is always the last call to the interface
    void (*release)(maildir_conn_dn_i*);
};

struct maildir_up_i {
    derr_t (*resp)(maildir_up_i*, imap_resp_t*);

    // return true if the maildir is currently synchronized with the mailserver
    bool (*synced)(maildir_up_i*);

    // return true if the maildir is the conn is in a SELECTED state
    bool (*selected)(maildir_up_i*);

    // if the connection is in a SELECTED state, CLOSE it.
    derr_t (*unselect)(maildir_up_i*);

    // unregistering must be done through the imaildir_t or dirmgr_t
};

struct maildir_dn_i {
    // (the first one must be the SELECT)
    derr_t (*cmd)(maildir_dn_i*, imap_cmd_t*);
    // check if there is on-thread processing needed
    bool (*more_work)(maildir_dn_i*);
    // do some on-thread work
    derr_t (*do_work)(maildir_dn_i*);

    // unregistering must be done through the imaildir_t or dirmgr_t
};

// IMAP maildir
struct imaildir_t {
    // path to this maildir on the filesystem
    string_builder_t path;
    // the mailbox name
    const dstr_t *name;
    unsigned int uid_validity;
    // mailbox flags
    ie_mflags_t mflags;
    // crypto for this box
    const keypair_t *keypair;

    // if SELECT returned NO, delete the box afterwards
    bool rm_on_close;


    // accessors
    link_t ups;  // up_t->link;
    link_t dns;  // dn_t->link;
    /* The value of naccessors may not match the length of the accessor
       lists, particularly during the failing shutdown sequence. */
    size_t naccessors;

    jsw_atree_t msgs;  // msg_base_t->node
    jsw_atree_t mods;  // msg_mod_t->node
    jsw_atree_t expunged;  // msg_expunge_t->node;
    maildir_log_i *log;
    // the latest serial of things we put in /tmp
    size_t tmp_count;
    link_t updates_requested;  // update_req_t->link
    // link_t updates_in_flight;  // update_base_t->link
};

// open a maildir at path, path and name must be linked to long-lived objects
derr_t imaildir_init(imaildir_t *m, string_builder_t path, const dstr_t *name,
        const keypair_t *keypair);
// free must only be called if the maildir has no accessors
void imaildir_free(imaildir_t *m);

// useful if an open maildir needs to be deleted
void imaildir_forceclose(imaildir_t *m);

// this will always consume or free req
derr_t imaildir_request_update(imaildir_t *m, update_req_t *req);

// void imaildir_ref_up(imaildir_t *m);
// void imaildir_ref_down(imaildir_t *m);

// regsiter a new connection with the imaildir, and return a maildir_i
derr_t imaildir_register_up(imaildir_t *m, maildir_conn_up_i *conn_up,
        maildir_up_i **maildir_up_out);

// regsiter a new connection with the imaildir, and return a maildir_i
derr_t imaildir_register_dn(imaildir_t *m, maildir_conn_dn_i *conn_dn,
        maildir_dn_i **maildir_dn_out);

/* if you got a maildir_*_i through dirmgr_open, you should close it via
   dirmgr_close */
// (the argument is actually just for type-safety, it's not used)
void imaildir_unregister_up(maildir_up_i *m);
void imaildir_unregister_dn(maildir_dn_i *m);

bool imaildir_synced(imaildir_t *m);

// open a message in a thread-safe way; return a file descriptor
derr_t imaildir_open_msg(imaildir_t *m, unsigned int uid, int *fd);

// close a message in a thread-safe way; return the result of close()
int imaildir_close_msg(imaildir_t *m, unsigned int uid, int *fd);

/* Synchronous filesystem-backed storage for an imap maildir.

   Message content is stored in maildir format, and is managed directly by the
   imaildir_t.  These functions are for things not easily stored in maildir
   format.
*/
struct maildir_log_i {
    unsigned int (*get_uidvld)(maildir_log_i*);
    derr_t (*set_uidvld)(maildir_log_i*, unsigned int uidvld);

    // the highest modseq we have synced from above, or 1 if we've seen nothing
    unsigned long (*get_himodseq_up)(maildir_log_i*);
    derr_t (*set_himodseq_up)(maildir_log_i*, unsigned long himodseq_up);

    // store the up-to-date message
    derr_t (*update_msg)(maildir_log_i*, const msg_base_t *base);

    // store the up-to-date expunge
    derr_t (*update_expunge)(maildir_log_i*, msg_expunge_t *expunge);

    // close and free the log
    void (*close)(maildir_log_i*);
};

// this must be implemented by some backend, currently only lmdb
derr_t imaildir_log_open(const string_builder_t *dirpath,
        jsw_atree_t *msgs_out, jsw_atree_t *expunged_out,
        jsw_atree_t *mods_out, maildir_log_i **log_out);

// this must be implemented by some backend, currently only lmdb
derr_t imaildir_log_rm(const string_builder_t *dirpath);

size_t imaildir_naccessors(imaildir_t *m);
