/* the component of an imap_maildir responsible for synchronizing upwards */
struct up_t;
typedef struct up_t up_t;
struct up_cb_i;
typedef struct up_cb_i up_cb_i;

/*
    PARALLELISM IN UP_T:

    The up_t is actually very simple.  There are 7 different types of commands
    that get sent upwards:
      - SELECT, which is really sent before anything else in the state machine
      - bootstrap fetches (initial fetch of all metadata)
      - deletions, from imaildir's initial unpushed expunges
          - these are actually done in a STORE and an EXPUNGE command
      - content fetch, either from:
          - imaildir_t's initial unfilled uids
          - newly seen uids
      - relay commands, any of:
          - a passthru command from the client, like LIST or LSUB
          - a STORE command originating from the dn_t
          - an EXPUNGE command originating from the dn_t
      - IDLE command
      - UNSELECT command, which is sent on request at literally any time
          - though if an IDLE is present, a DONE should preceed it

    Conclusions are:
      - boostrap fetches are always run before other things
      - initial deletions seem suspect... is this the right layer?  In any
        case, making them serialized should be fine since it's sort of a weird
        error case that won't arise nearly ever (without a MUA like mutt).
      - content fetches can be fully parallelized without concern
      - relay commands can be fully parallel, that's on the caller to handle
      - IDLE obviously precludes anything else being parallel, and should only
        be launched when nothing else is in flight
      - UNSELECT state needs to be checked in between any commands which
        trigger other commands (such as the steps in the initial deletions)
*/

// the interface provided to up_t by its owner
struct up_cb_i {
    // the up_t wants to pass an imap command over the wire
    derr_t (*cmd)(up_cb_i*, imap_cmd_t*);
    // this event indiates a SELECT finished, with an ie_st_resp_t if it failed
    // (if select fails, you should go straight to dirmgr_close_up())
    // (if this imaildir is already selected, this call won't even happen)
    void (*selected)(up_cb_i*, ie_st_resp_t*);
    // this event indicates the maildir finished an initial sync
    // (if this imaildir is already synced, the callback may be instant)
    void (*synced)(up_cb_i*);
    // after up_idle_block; this callback is often instant
    void (*idle_blocked)(up_cb_i*);
    // this event is a response to the up_unselect() call
    derr_t (*unselected)(up_cb_i*);
    // interaction with the imaildir_t has trigged some new work
    void (*enqueue)(up_cb_i*);
    // the imaildir has failed
    void (*failure)(up_cb_i*, derr_t);

    /* the up_t is fully controlled by its owner; it does not have reference
       counts or lifetime callbacks */
};

// the interface the up_t provides to its owner:

// pass a response from the remote imap server to the up_t
derr_t up_resp(up_t *up, imap_resp_t *resp);
// block IDLE commands based on some external state
derr_t up_idle_block(up_t *up);
derr_t up_idle_unblock(up_t *up);
// if the connection is in a SELECTED state, UNSELECT it.
derr_t up_unselect(up_t *up);
derr_t up_do_work(up_t *up, bool *noop);

// the interface the up_t provides to the imaildir:

// up_imaildir_select contains late-initialization information
// it can be called multiple times in the lifetime of an up_t
void up_imaildir_select(
    up_t *up,
    const dstr_t *name,
    unsigned int uidvld_up,
    uint64_t himodseq_up,
    bool examine
);
void up_imaildir_relay_cmd(up_t *up, imap_cmd_t *cmd, imap_cmd_cb_t *cb);
void up_imaildir_preunregister(up_t *up);
// disallow downloading a specific UID
void up_imaildir_have_local_file(up_t *up, unsigned uid_up);
// trigger any downloading work that needs to be done after a hold ends
void up_imaildir_hold_end(up_t *up);

// up_t is all the state we have for an upwards connection
struct up_t {
    imaildir_t *m;
    // the interfaced provided to us
    up_cb_i *cb;
    bool synced;
    // a tool for tracking the highestmodseq we have actually synced
    himodseq_calc_t hmsc;
    seq_set_builder_t uids_up_to_download;
    // current tag
    size_t tag;
    link_t cbs;  // imap_cmd_cb_t->link (may be wrapped in an up_cb_t)
    link_t link;  // imaildir_t->access.ups

    /* technically, advance_state is written to be callable at any time, but
       only calling it when we asked to be enqueued makes debugging easier */
    bool enqueued;

    /* this is defined by the most recent SELECT/EXAMINE call; it may not have
       finished its round trip yet */
    bool examine;

    struct {
        bool needed;
        bool sent;
    } unselect;

    struct {
        bool ready;
        bool sent;
        bool done;
        bool examine;
        const dstr_t *name;
        unsigned int uidvld_up;
        uint64_t himodseq_up;
    } select;

    struct {
        bool needed;
        bool sent;
        bool done;
    } bootstrap;

    struct {
        bool store_sent;
        bool store_done;
        bool expunge_sent;
        bool expunge_done;
        ie_seq_set_t *uids_up;
    } deletions;

    struct {
        seq_set_builder_t uids_up;
        int in_flight;
    } fetch;

    struct {
        /* reselect logic is a bit complex.  For correctness, it must be
           enqueued with the relay commands, because the relay commands can
           include e.g. a STORE command that depends on the reselect transition
           to occur first.

           The easy-but-ugly way to guarantee this is to have separate queues
           for post-reselect cbs and cmds.  Relays recieved while we expect to
           send a reselect get appended to that delayed queue.

           Overall, this saves us from having to keep track of multiple
           in-flight reselects, which would be a PITA. */
        bool needed;
        bool examine;
        unsigned int uidvld_up;
        uint64_t himodseq_up;

        // delayed relays; if relays arrive before we
        link_t cmds;  // imap_cmd_t->link
        link_t cbs;  // imap_cmd_cb_t->link (from an imaildir_cb_t)

    } reselect;

    struct {
        // cmds and cbs are synchronized lists
        link_t cmds;  // imap_cmd_t->link
        link_t cbs;  // imap_cmd_cb_t->link (from an imaildir_cb_t)
    } relay;

    /* when our owner wants to take control over the connection for a passthru
       command, it will block our IDLE so that it doesn't send mid-command */
    struct {
        bool want;
        bool active;
    } idle_block;

    struct {
        bool sent;
        bool got_plus;
        bool done_sent;
    } idle;

    // *exts should point to somewhere else
    extensions_t *exts;
};
DEF_CONTAINER_OF(up_t, link, link_t)

derr_t up_init(up_t *up, up_cb_i *cb, extensions_t *exts);
void up_free(up_t *up);
