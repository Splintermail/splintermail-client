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
struct maildir_i;
typedef struct maildir_i maildir_i;
struct dirmgr_i;
typedef struct dirmgr_i dirmgr_i;
struct imaildir_t;
typedef struct imaildir_t imaildir_t;
struct maildir_log_i;
typedef struct maildir_log_i maildir_log_i;

/* the upwards session must provide this interface to the maildir; it is how
   the maildir will communicate the imap client behavior */
struct maildir_conn_up_i {
    // the maildir wants to pass an imap command over the wire
    void (*cmd)(maildir_conn_up_i*, imap_cmd_t*);
    // this event indicates the maildir finished an initial sync
    void (*synced)(maildir_conn_up_i*);
    // this event is a response to the maildir_i->unselect() call
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
    // The maildir BROADCASTs its failures when it dies
    /* note the asymmetry: the maildir resource can tell the actor owning the
       conn_up that the actor must die with this call, but there is no call for
       the actor to tell the shared resource to die */
    void (*failure)(maildir_conn_dn_i*, derr_t);
    // this is always the last call to the interface
    void (*release)(maildir_conn_dn_i*);
};

struct maildir_i {
    // imap responses must come from an upwards connection
    derr_t (*resp)(maildir_i*, imap_resp_t*);

    // return true if the maildir is currently synchronized with the mailserver
    bool (*synced)(maildir_i*);

    // return true if the maildir is the conn is in a SELECTED state
    bool (*selected)(maildir_i*);

    // if the connection is in a SELECTED state, CLOSE it.
    derr_t (*unselect)(maildir_i*);

    ////// for downwards connections:

    // imap commands must come from a downwards connection
    // (the first one must be the SELECT)
    derr_t (*cmd)(maildir_i*, imap_cmd_t*);

    // unregistering must be done through the imaildir_t or dirmgr_t
};

// dirmgr_t-provided interface, required by imaildir_t
struct dirmgr_i {
    // all_unregistered is called any time the number of acessors goes to zero.
    void (*all_unregistered)(dirmgr_i*);
};

// IMAP maildir
struct imaildir_t {
    dirmgr_i *dirmgr;
    // path to this maildir on the filesystem
    string_builder_t path;
    // the mailbox name
    const dstr_t *name;
    unsigned int uid_validity;
    // mailbox flags
    ie_mflags_t mflags;
    // crypto for this box
    const keypair_t *keypair;

    // lock ordering is downwards

    struct {
        uv_mutex_t lock;
        link_t ups;  // up_t->link;
        link_t dns;  // dn_t->link;
        /* The value of naccessors may not match the length of the accessor
           lists, particularly during the failing shutdown sequence. */
        size_t naccessors;
        /* failed refers to the whole imaildir_t, but the only race condition
           is related to rejecting incoming accessors, so it falls under the
           same mutex as naccessors */
        bool failed;
    } access;

    struct {
        uv_rwlock_t lock;
        jsw_atree_t tree;  // msg_base_t->node
    } msgs;

    struct {
        uv_rwlock_t lock;
        jsw_atree_t tree;  // msg_mod_t->node
    } mods;

    struct {
        uv_rwlock_t lock;
        jsw_atree_t tree;  // msg_expunge_t->node;
    } expunged;

    struct {
        uv_mutex_t lock;
        maildir_log_i *log;
    } log;

    struct {
        uv_mutex_t lock;
        // the latest serial of things we put in /tmp
        size_t count;
    } tmp;
};

// open a maildir at path, path and name must be linked to long-lived objects
derr_t imaildir_init(imaildir_t *m, string_builder_t path, const dstr_t *name,
        dirmgr_i *mgr, const keypair_t *keypair);
// free must only be called if the maildir has no accessors
void imaildir_free(imaildir_t *m);

// useful if an open maildir needs to be deleted
void imaildir_forceclose(imaildir_t *m);

// void imaildir_ref_up(imaildir_t *m);
// void imaildir_ref_down(imaildir_t *m);

// regsiter a new connection with the imaildir, and return a maildir_i
derr_t imaildir_register_up(imaildir_t *m, maildir_conn_up_i *conn_up,
        maildir_i **maildir_out);

// regsiter a new connection with the imaildir, and return a maildir_i
derr_t imaildir_register_dn(imaildir_t *m, maildir_conn_dn_i *conn_dn,
        maildir_i **maildir_out);

/* there is a race condition between imalidir_register and imaildir_unregister;
   generally, if you got a maildir_i* through dirmgr_open, you should close it
   via dirmgr_close */
// (the argument is actually just for type-safety, it's not used)
void imaildir_unregister_up(maildir_i *m, maildir_conn_up_i *conn);
void imaildir_unregister_dn(maildir_i *m, maildir_conn_dn_i *conn);

bool imaildir_synced(imaildir_t *m);

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
