/* It is technically possible for a delete to cancel an update, then for a
   duplicate of the update to appear and remain forever, so we prevent that by
   leaving deletions in the data structure for a few minutes to ensure that no
   stray network packet could be still circulating.  We use the IPv4 max TTL
   even though in practice that's a lot longer than a real packet can live. */
#define GC_DELAY 255

// kvpsync_recv_t is the receiver's side of kvp sync
typedef struct {
    hashmap_t h;  // maps dstr_t keys to dstr_t values
    uint32_t recv_id;  // randomly chosen at boot
    uint32_t sync_id;  // the send_id for a sync we've completed, or zero
    bool initial_sync_acked;
    xtime_t ok_expiry;
    link_t gc; // recv_datum_t->gc
} kvpsync_recv_t;

derr_t kvpsync_recv_init(kvpsync_recv_t *r);
void kvpsync_recv_free(kvpsync_recv_t *r);

// process an incoming packet, and decide if it should be acked or not
// (pre-initial-sync packets are the only packets not acked)
derr_t kvpsync_recv_handle_update(
    kvpsync_recv_t *r, xtime_t now, kvp_update_t update, bool *should_ack
);

extern const dstr_t *UNSURE;
/* returns NULL, UNSURE, or an answer, and is only guaranteed to be valid
   until the next call to handle_update() */
const dstr_t *kvpsync_recv_get_value(
    kvpsync_recv_t *r, xtime_t now, const dstr_t key
);
