#include "server/kvpsync/libkvpsync.h"

#include <stdlib.h>
#include <string.h>

// we expect RTT to be around 20ms between our server and the DNS server
#define MIN_INFLIGHT 1  // 529*8bits * 1pkts * 50pkts/s = 212kbps
// rfc5348: use additive increase, multiplicative decrease
#define INCREASE_PKTS 1
#define DECREASE_BY_FACTOR(n) ((n*4) / 5)
#define DECREASE_BACKOFF (1*SECOND)

// send_data_t is used for all of cache, unsent, and unacked/sent
typedef struct {
    kvp_update_t update;
    link_t link;  // kvpsync_send_t->unsent,sent
    hash_elem_t uelem;  // kvpsync_send_t->unacked
    hash_elem_t celem;  // kvpsync_send_t->cache
    dstr_t key;  // ultimately wraps update.key, only for INSERTs
    size_t nrefs;
    // congestion tracking
    bool inflight;
    int inflight_at_send;
    uint32_t congest_validity;
    xtime_t sent_time;
    bool resent;
    kvpsync_add_key_cb cb;  // may be NULL
    void *cb_data;
    link_t oldest;  // kvpsync_send_t->oldest;
    xtime_t deadline;
} send_data_t;
DEF_CONTAINER_OF(send_data_t, link, link_t);
DEF_CONTAINER_OF(send_data_t, uelem, hash_elem_t);
DEF_CONTAINER_OF(send_data_t, celem, hash_elem_t);
DEF_CONTAINER_OF(send_data_t, oldest, link_t);

// aborts on OOM
static send_data_t *send_data_xnew(void){
    send_data_t *data = malloc(sizeof(*data));
    if(!data) LOG_FATAL("out of memory\n");
    *data = (send_data_t){ .nrefs = 1 };
    return data;
}

// downrefs or frees
static void data_free(send_data_t *data){
    if(--data->nrefs) return;
    link_remove(&data->oldest);
    // TODO: it seems wrong that there are cases where cb is not guaranteed
    free(data);
}

static send_data_t *peek(link_t *list){
    if(link_list_isempty(list)) return NULL;
    return CONTAINER_OF(list->next, send_data_t, link);
}

static send_data_t *peek_oldest(link_t *list){
    if(link_list_isempty(list)) return NULL;
    return CONTAINER_OF(list->next, send_data_t, oldest);
}

// remove from sent or unsent or wherever
static void data_remove(kvpsync_send_t *s, send_data_t *data){
    link_remove(&data->link);
    if((s->inflight -= data->inflight) < 0) LOG_FATAL("s->inflight underflow");
    data->inflight = false;
}

derr_t kvpsync_send_init(kvpsync_send_t *s, xtime_t now){
    derr_t e = E_OK;

    // pick a random non-zero sync_id
    uint32_t sync_id = 0;
    while(!sync_id){
        dstr_t d = {
            .data = (char*)&sync_id,
            .len = 0,
            .size = sizeof(sync_id),
            .fixed_size = true,
        };
        PROP(&e, urandom_bytes(&d, d.size) );
    }

    *s = (kvpsync_send_t){
        .sync_id = sync_id,
        // start in not-ok state, but assume receiver just got an ok extension
        .ok_expiry = now + MIN_RESPONSE,
        .inflight_limit = MIN_INFLIGHT,
    };
    PROP(&e, hashmap_init(&s->unacked) );
    PROP_GO(&e, hashmap_init(&s->cache), fail_unacked);

    return e;

fail_unacked:
    hashmap_free(&s->unacked);
    return e;
}

static void empty_list(kvpsync_send_t *s, link_t *list){
    send_data_t *data;
    while((data = peek(list))){
        data_remove(s, data);
        data_free(data);
    }
}

static void drop_data(kvpsync_send_t *s){
    // empty unacked
    hashmap_trav_t trav;
    hash_elem_t *elem = hashmap_pop_iter(&trav, &s->unacked);
    for(; elem; elem = hashmap_pop_next(&trav));
    // drop unsent
    empty_list(s, &s->unsent);
    // drop sent
    empty_list(s, &s->sent);
    // don't empty oldest! entries there persist until the data is acked
}

void kvpsync_send_free(kvpsync_send_t *s){
    // drop data from unacked/unsent/sent
    drop_data(s);
    hashmap_free(&s->unacked);

    // drop data from cache
    hashmap_trav_t trav;
    for(
        hash_elem_t *elem = hashmap_pop_iter(&trav, &s->cache);
        elem;
        elem = hashmap_pop_next(&trav)
    ){
        send_data_t *data = CONTAINER_OF(elem, send_data_t, celem);
        data_free(data);
    }

    hashmap_free(&s->cache);
}

// put something in unsent for the first time
static void data_queue(kvpsync_send_t *s, send_data_t *data){
    data->update.sync_id = s->sync_id;
    uint32_t update_id = ++s->update_id;
    data->update.update_id = update_id;
    link_list_append(&s->unsent, &data->link);
    hash_elem_t *old = hashmap_setu(&s->unacked, update_id, &data->uelem);
    // old may be NULL or it match be what we inserted, but not anything else
    if(old && old != &data->uelem){
        LOG_FATAL("duplicate update_id in data_queue()\n");
    }
}

static xtime_t calculate_ok_expiry(kvpsync_send_t *s, xtime_t now){
    if(!s->recv_ok) return 0;

    xtime_t ok_expiry = s->ok_expiry;
    if(s->last_recv > s->last_extend_ok){
        /* we have proof of responsiveness since our last extension, so we
           can grant another MIN_RESPONSE */
        ok_expiry = now + MIN_RESPONSE;
    }
    send_data_t *oldest = peek_oldest(&s->oldest);
    if(oldest){
        /* but ok_expiry must never exceed the deadline of the oldest
           unacked insert, in order to offer readiness guarantees across
           multiple replicas */
        ok_expiry = MIN(oldest->deadline, ok_expiry);
    }

    if(ok_expiry > s->ok_expiry){
        // update the ok_expiry time
        s->ok_expiry = ok_expiry;
        // record this as our last extension time
        s->last_extend_ok = now;
    }

    return ok_expiry;
}

// write to a buffer, with appropriate flags set
static void data_send(
    kvpsync_send_t *s,
    send_data_t *data,
    xtime_t now,
    const kvp_update_t **out
){
    // remove from wherever it is
    data_remove(s, data);

    // put in sent
    link_list_append(&s->sent, &data->link);

    data->sent_time = now;
    data->inflight = true;
    data->inflight_at_send = ++s->inflight;
    data->congest_validity = s->congest_validity;
    data->update.ok_expiry = calculate_ok_expiry(s, now);

    *out = &data->update;
}

static void queue_start(kvpsync_send_t *s){
    send_data_t *data = send_data_xnew();
    data->update.type = KVP_UPDATE_START;
    data->update.resync_id = s->resync_id;
    data_queue(s, data);
}

static void queue_all_inserts(kvpsync_send_t *s){
    // walk through our cache and send everything
    hashmap_trav_t trav;
    for(
        hash_elem_t *elem = hashmap_iter(&trav, &s->cache);
        elem;
        elem = hashmap_next(&trav)
    ){
        send_data_t *data = CONTAINER_OF(elem, send_data_t, celem);
        if(data->nrefs++ != 1){ LOG_FATAL("too many nrefs!\n"); }
        data_queue(s, data);
    }
}

static void queue_flush(kvpsync_send_t *s){
    send_data_t *data = send_data_xnew();
    data->update.type = KVP_UPDATE_FLUSH;
    data_queue(s, data);
}

static void queue_empty(kvpsync_send_t *s){
    send_data_t *data = send_data_xnew();
    data->update.type = KVP_UPDATE_EMPTY;
    data_queue(s, data);
}

// keep cache and inflight_limit but not unsent/unacked/sent
static void kvpsync_send_resync(kvpsync_send_t *s, uint32_t resync_id){
    s->resync_id = resync_id;
    // pick a new sync_id (avoiding zero)
    s->sync_id += 1U + (s->sync_id == UINT32_MAX);
    s->update_id = 0;

    // start in not-ok state
    s->recv_ok = false;

    s->last_extend_ok = 0;

    drop_data(s);

    s->inflight = 0;
    s->ok_expiry = 0;  // resync means the receiver knows its not ok
    // reset flags
    s->blocked = false;
    s->synced = false;
    s->start_done = false;
    s->start_sent = false;
    s->sync_done = false;
    s->sync_sent = false;
}

static void advance_state(kvpsync_send_t *s, xtime_t now){
    // initial sync
    if(!s->synced){
        // start packet
        if(!s->start_done){
            if(!s->start_sent){
                queue_start(s);
                s->blocked = true;
                s->start_sent = true;
            }
            if(s->blocked) return;
            s->start_done = true;
        }
        // sync content
        if(!s->sync_done){
            if(!s->sync_sent){
                queue_all_inserts(s);
                s->blocked = true;
                s->sync_sent = true;
            }
            if(s->blocked) return;
            s->sync_done = true;
        }
        // receiver is now in OK state
        s->recv_ok = true;
        (void)calculate_ok_expiry(s, now);
        // send flush packet and proceed as normal
        queue_flush(s);
        s->synced = true;
    }

    // now we just react to stimuli?
}

// congestion control
static void successful_packet(kvpsync_send_t *s, send_data_t *data){
    if(data->congest_validity != s->congest_validity) return;
    if(data->inflight_at_send < s->inflight_limit) return;
    s->inflight_limit += INCREASE_PKTS;
    s->congest_validity++;
}

// process an incoming packet
void kvpsync_send_handle_ack(kvpsync_send_t *s, kvp_ack_t ack, xtime_t now){
    s->last_recv = now;

    // detect resync packets
    if(ack.update_id == 0){
        uint32_t resync_id = ack.sync_id;
        // ignore duplicates (we have normal resend logic)
        if(resync_id == s->resync_id) return;
        kvpsync_send_resync(s, resync_id);
        return;
    }

    // ignore stale acks from an earlier synchronization
    if(ack.sync_id != s->sync_id) return;

    // otherwise process the ack
    hash_elem_t *elem = hashmap_delu(&s->unacked, ack.update_id);
    // ignore unexpected acks
    if(!elem) return;

    send_data_t *data = CONTAINER_OF(elem, send_data_t, uelem);
    data_remove(s, data);
    if(link_list_isempty(&s->sent) && link_list_isempty(&s->unsent)){
        s->blocked = false;
    }

    // congestion control
    successful_packet(s, data);

    // is there an action to take after finishing this packet?
    if(data->cb){
        link_remove(&data->oldest);
        data->cb(data->cb_data);
        // avoid retriggering callback after resync
        data->cb = NULL;
    }

    data_free(data);
}

static xtime_t fault_time(send_data_t *data){
    return data->sent_time + 1 * SECOND;
}

static bool inflight_is_full(kvpsync_send_t *s){
    return s->inflight >= s->inflight_limit;
}

// congestion control
static void fault_detected(kvpsync_send_t *s, send_data_t *data, xtime_t now){
    if(data->congest_validity != s->congest_validity) return;
    if(now < s->decrease_backoff) return;
    s->inflight_limit = DECREASE_BY_FACTOR(s->inflight_limit);
    s->inflight_limit = MAX(s->inflight_limit, MIN_INFLIGHT);
    s->decrease_backoff = now + DECREASE_BACKOFF;
    s->congest_validity++;
}

kvpsync_run_t kvpsync_send_run(kvpsync_send_t *s, xtime_t now){
    kvpsync_run_t out = {0};

    advance_state(s, now);

    if(s->recv_ok && now >= s->ok_expiry){
        // peer is no longer in OK state
        s->recv_ok = false;
        // TODO: tell somebody?
    }

    // check for unresponsive peer
    send_data_t *data = peek(&s->sent);
    if(data && now >= fault_time(data) && s->last_recv <= data->sent_time){
        // reset limit and resend all packets
        link_t *ptr = s->unsent.next;
        while((data = peek(&s->sent))){
            data_remove(s, data);
            link_list_append(ptr, &data->link);
        }
        if(s->inflight != 0){
            LOG_FATAL("inflight should be zero\n");
        }
        s->inflight_limit = MIN_INFLIGHT;
    }

    // deal with packets past their fault time
    while((data = peek(&s->sent)) && now >= fault_time(data)){
        // peer is still responding but this packet got skipped
        /* (note: we checked s->last_recv <= s->sent time against the oldest
           sent packet we have previously, so we skip that here */
        // just resend it
        data_remove(s, data);
        link_list_append(&s->unsent, &data->link);
        fault_detected(s, data, now);
    }

    // if we have unsent, and inflight capacity, send a packet
    if(!inflight_is_full(s) && (data = peek(&s->unsent))){
        data_send(s, data, now, &out.pkt);
        if(!inflight_is_full(s) && !link_list_isempty(&s->unsent)){
            // there's another packet to send immediately
            out.deadline = 0;
        }else{
            // next fault time to consider is the first sent packet
            out.deadline = fault_time(peek(&s->sent));
        }
        goto adjust_deadline;
    }

    // if we have sent packets, the next deadline would be our fault time
    if((data = peek(&s->sent))){
        out.deadline = fault_time(peek(&s->sent));
        goto adjust_deadline;
    }

    // no sent or unsent packets, should we send a keepalive packet?
    if(now + 3 * SECOND < s->ok_expiry){
        out.deadline = s->ok_expiry - 3 * SECOND;
        goto adjust_deadline;
    }

    // send a keepalive
    s->recv_ok = true;
    queue_empty(s);
    data = peek(&s->unsent);
    data_send(s, data, now, &out.pkt);
    out.deadline = fault_time(data);

adjust_deadline:
    // detect when the ok_expiry dominates our deadline
    if(s->recv_ok && out.deadline > s->ok_expiry){
        out.deadline = s->ok_expiry;
    }
    return out;
}

//

static void queue_delete_and_free(kvpsync_send_t *s, send_data_t *old){
    // only send the deletion if the insertion was sent with this sync_id
    if(s->sync_sent && old->update.sync_id == s->sync_id){
        send_data_t *data = send_data_xnew();
        data->update.type = KVP_UPDATE_DELETE;
        data->update.klen = old->update.klen;
        memcpy(data->update.key, old->update.key, old->update.klen);
        data->update.delete_id = old->update.update_id;
        data_queue(s, data);
    }
    data_free(old);
}

void kvpsync_send_add_key(
    kvpsync_send_t *s,
    xtime_t now,
    const dstr_t key,
    const dstr_t val,
    kvpsync_add_key_cb cb,
    void *cb_data
){
    if(key.len > KVPSYNC_MAX_LEN) LOG_FATAL("key too long!");
    if(val.len > KVPSYNC_MAX_LEN) LOG_FATAL("val too long!");
    send_data_t *data = send_data_xnew();
    data->update.type = KVP_UPDATE_INSERT;
    data->update.klen = (uint8_t)key.len;
    memcpy(data->update.key, key.data, key.len);
    data->update.vlen = (uint8_t)val.len;
    memcpy(data->update.val, val.data, val.len);
    data->cb = cb;
    data->cb_data = cb_data;
    data->deadline = now + MIN_RESPONSE;
    link_list_append(&s->oldest, &data->oldest);
    DSTR_WRAP(data->key, data->update.key, key.len, false);

    // insert this data
    hash_elem_t *elem = hashmap_sets(&s->cache, &data->key, &data->celem);

    // detect pre-existing data
    if(elem != NULL){
        send_data_t *old = CONTAINER_OF(elem, send_data_t, celem);
        queue_delete_and_free(s, old);
    }

    // now send our new update
    if(s->sync_sent){
        data->nrefs++;
        data_queue(s, data);
    }
}

// delete a key-value pair
void kvpsync_send_delete_key(kvpsync_send_t *s, const dstr_t key){
    hash_elem_t *elem = hashmap_dels(&s->cache, &key);
    if(elem == NULL) return;
    send_data_t *data = CONTAINER_OF(elem, send_data_t, celem);
    queue_delete_and_free(s, data);
}
