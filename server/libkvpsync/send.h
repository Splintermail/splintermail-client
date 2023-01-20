struct kvpsync_send_t;
typedef struct kvpsync_send_t kvpsync_send_t;

typedef void (*recv_state_cb)(kvpsync_send_t*, bool, void*);

struct kvpsync_send_t {
    recv_state_cb state_cb;
    void *cb_data;

    uint32_t sync_id;
    uint32_t resync_id;
    uint32_t update_id; // latest value

    /* Track our own cache to make resyncing easy, and to ensure cbs are
       persisted across resync events.  Our caller will also have their own
       cache for comparing against current versions of the database, and they
       update us with a diff-based API.  This wastes a bit of memory but it
       hides kvpsync resync logic in the sender and hides database sync logic
       in the calling code, and the separation is worth it. */
    hashmap_t cache; // send_data_t->celem

    /* packet lifetime summary:
       - when a packet is sent, it goes to unacked until its response
       - packets may be in unsent or sent, but not both
       - when a new packet is created it goes to unsent
       - when a packet is written to the wire, it goes to sent
       - when we decide to resend a packet it goes back to unsent
       - inflight counts only what is in sent
         - after deciding to resend a packet, it no longer counts as inflight
       - sent is ordered by oldest-sent first */
    link_t unsent;  // send_data_t->link
    hashmap_t unacked;  // send_data_t->uelem
    link_t sent;  // send_data_t->link

    /* When new inserts are added, they go into oldest, where they stay until
       they have been ACKed, even across resyncs.

       ok_expiry must not exceed 15 seconds later than the oldest added key,
       since the overall required behavior is that when a kvp is added to the
       system, the system is ready to serve that kvp:
         - as soon as all receivers have ACKed it (best case)
         - after 15 seconds, if at least one recevier has ACKed it, and the
           remaining receivers know they are out-of-date (fallback case)
         - never, if all receivers are unresponsive (service outage case) */
    link_t oldest;

    // congestion control
    int inflight;
    int inflight_limit;
    xtime_t decrease_backoff;
    uint32_t congest_validity;
    xtime_t last_recv;

    bool recv_ok;
    bool old_recv_ok;
    xtime_t ok_expiry;
    xtime_t last_extend_ok;

    // state flags
    bool blocked : 1;  // waiting for unsent/sent to be empty
    bool synced : 1;
    bool start_done : 1;
    bool start_sent : 1;
    bool sync_done : 1;
    bool sync_sent : 1;
};

// note that the recv_ok starts as false, but there's no state_cb on startup
derr_t kvpsync_send_init(
    kvpsync_send_t *s, xtime_t now, recv_state_cb state_cb, void *cb_data
);
void kvpsync_send_free(kvpsync_send_t *s);

// process an incoming packet
void kvpsync_send_handle_ack(kvpsync_send_t *s, kvp_ack_t ack, xtime_t now);

// return value for kvpsync_send_run
typedef struct {
    /* if pkt is returned, it is only guaranteed valid until the next call
       is made against the kvpsync_send_t */
    const kvp_update_t *pkt;
    // deadline is for the next action, and will always be set
    xtime_t deadline;
} kvpsync_run_t;

kvpsync_run_t kvpsync_send_run(kvpsync_send_t *s, xtime_t now);

typedef void (*kvpsync_add_key_cb)(kvpsync_send_t*, void*);

// add a key-value pair (or crash if OOM)
void kvpsync_send_add_key(
    kvpsync_send_t *s,
    xtime_t now,
    const dstr_t key,
    const dstr_t val,
    kvpsync_add_key_cb cb,
    void *cb_data
);

// delete a key-value pair, guarantee the associated add_key_cb is not called
void kvpsync_send_delete_key(kvpsync_send_t *s, const dstr_t key);
