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
      - detection fetches (detect messages with just their metadata)
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
      - initial "boostrap" detection fetch is always run before other things
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
    void (*schedule)(up_cb_i*);
};

// the interface the up_t provides to its owner:

// pass a response from the remote imap server to the up_t
derr_t up_st_resp(up_t *up, const ie_st_resp_t *st_resp, link_t *out);
derr_t up_fetch_resp(up_t *up, const ie_fetch_resp_t *fetch, link_t *out);
derr_t up_vanished_resp(up_t *up, const ie_vanished_resp_t *vanished);
derr_t up_exists_resp(up_t *up, unsigned int exists, link_t *out);
derr_t up_plus_resp(up_t *up, link_t *out);

// block IDLE commands based on some external state
derr_t up_idle_block(up_t *up, link_t *out, bool *ok);
derr_t up_idle_unblock(up_t *up, link_t *out);

// if the connection is in a SELECTED state, UNSELECT it.
derr_t up_unselect(up_t *up, link_t *out, bool *ok);

derr_t up_advance_state(up_t *up, link_t *out);

// the interface the up_t provides to the imaildir:

// up_imaildir_select contains late-initialization information
// it can be called multiple times in the lifetime of an up_t
// imaildir_t must not emit a second call before the first is completed
void up_imaildir_select(
    up_t *up,
    const dstr_t *name,
    unsigned int uidvld_up,
    uint64_t himodseq_up,
    bool examine
);
// imaildir_t must not emit a relay while a select is in flight
void up_imaildir_relay_cmd(up_t *up, imap_cmd_t *cmd, imap_cmd_cb_t *cb);
// disallow downloading a specific UID
void up_imaildir_have_local_file(up_t *up, unsigned int uid, bool resync);
// trigger any downloading work that needs to be done after a hold ends
void up_imaildir_hold_end(up_t *up);
// solemnly swear to never touch the imaildir again
void up_imaildir_failed(up_t *up);

// up_t is all the state we have for an upwards connection
struct up_t {
    imaildir_t *m;
    // the interfaced provided to us
    up_cb_i *cb;
    // have we finished our initial sync?
    bool synced;
    // track the highest modseq value we see from any source
    uint64_t himodseq_up_seen;
    /* After any command is complete, we can commit the highest modseq we've
       seen to persistent storage, so long as we do not have new messages to
       find (before the first bootstrap or after an EXISTS response).

       There are also a few cases where we will do this in the middle of an
       IDLE command.

       Note that this is only safe because we never emit plain FETCH, STORE, or
       COPY commands, as those cause EXPUNGEs to be witheld, which in turn
       causes the himodseq to become unnecessarily complex to interpret. */
    uint64_t himodseq_up_committed;
    seq_set_builder_t uids_up_to_download;
    // current tag
    size_t tag;
    link_t cbs;  // imap_cmd_cb_t->link (may be wrapped in an up_cb_t)
    link_t link;  // imaildir_t->ups

    // we will neither send nor receive anything further
    bool done;

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

    /* a "bootstrap" triggers the behavior of a SELECT (QRESYNC ...) when our
       UIDVALIDITY was correct and our HIGHESTMODSEQ was zero.  It sends a
       detection fetch with CHANGEDSINCE 1.

       The bootstrap should be invoked anytime that we had the wrong
       UIDVALIDITY, either because it is an initial synchronization and we
       didn't know the UIDVALIDITY at all, or because it changed on us.

       After the bootstrap is complete, our state should correctly represent
       the HIGHESTMODSEQ value reported after the SELECT. */
    struct {
        bool needed;
        bool sent;
    } bootstrap;

    struct {
        bool store_sent;
        bool store_done;
        bool expunge_sent;
        bool expunge_done;
        ie_seq_set_t *uids_up;
    } deletions;

    struct {
        // nonzero chgsince means we saw an EXISTS
        uint64_t chgsince;
        /* The repeat flag is set if we see an EXISTS response while chgsince
           is set.  It will cause us to do a second detect fetch immediately
           after the first finishes, without allowing any room for committing
           the himodseq to persistent storage.

           This is because an EXISTS response in the middle of our detect
           fetch response means that our fetch was incomplete.  This is easily
           observable.

           Technically, the detect fetch can be pipelined and it is probably
           safe to only set the repeat flag if the EXISTS response happens
           while in the fetch response.  An EXISTS response in the middle of
           an earlier response which arrives after we send the detect fetch
           command but before the fetch response starts probably does not need
           to trigger a repeat, but that's very hard to verify, so we do the
           simple and bulletproof thing, and repeat in that case anyway. */
        bool inflight;
        bool repeat;
    } detect;

    struct {
        seq_set_builder_t uids_up;
        int in_flight;
    } fetch;

    struct {
        /* imaildir_t must guarantee we only have one reselect in progress at
           a time and that no relays are added after a reselect is requested */
        bool needed;
        bool enqueued;
        bool examine;
        unsigned int uidvld_up;
        uint64_t himodseq_up;
        bool done;
    } reselect;


    struct {
        /* after a hold, the up_t will need to resync with a NOOP before it can
           execute any other relay commands.  Otherwise, if somebody else does
           the APPEND and immediately sets a flag on it, this up_t may not have
           seen the EXISTS response yet */
        struct {
            bool needed;
            bool inflight;
        } resync;

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

derr_t up_init(up_t *up, imaildir_t *m, up_cb_i *cb, extensions_t *exts);
void up_free(up_t *up);
