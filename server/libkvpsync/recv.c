#include "server/libkvpsync/libkvpsync.h"

#include <stdlib.h>

struct recv_data_t;
typedef struct recv_data_t recv_data_t;

/* The receiver must accommodate UPDATE packets which arrive out-of-order.  We
   can do that by tracking deletions as objects rather than executing them as
   actions.  The action is taken when a delete_id matches an update_id from the
   same sequence. */
typedef struct {
    recv_data_t *data;
    uint32_t sync_id;
    uint32_t update_id;
    uint32_t delete_id;  // nonzero means this is a deletion
    dstr_t val;
    char _val[KVPSYNC_MAX_LEN];
    link_t link;  // recv_data_t->list
    xtime_t gc_time;
    link_t gc;  // kvpsync_recv_t->gc
} recv_datum_t;
DEF_CONTAINER_OF(recv_datum_t, link, link_t)
DEF_CONTAINER_OF(recv_datum_t, gc, link_t)

struct recv_data_t {
    dstr_t key;
    char _key[KVPSYNC_MAX_LEN];
    link_t list;  // recv_datum_t->link
    hash_elem_t elem;  // kvp_recv_t->h
};
DEF_CONTAINER_OF(recv_data_t, elem, hash_elem_t)

// recv_datum_t is freed with just free
static derr_t recv_datum_new(
    recv_datum_t **out,
    recv_data_t *data,
    uint32_t sync_id,
    uint32_t update_id,
    uint32_t delete_id,
    const dstr_t val
){
    derr_t e = E_OK;
    *out = NULL;

    if(val.len > KVPSYNC_MAX_LEN){
        ORIG(&e, E_INTERNAL, "val too long!");
    }

    recv_datum_t *datum = DMALLOC_STRUCT_PTR(&e, datum);
    CHECK(&e);

    datum->data = data;
    datum->sync_id = sync_id;
    datum->update_id = update_id;
    datum->delete_id = delete_id;
    link_init(&datum->link);
    DSTR_WRAP_ARRAY(datum->val, datum->_val);
    PROP_GO(&e, dstr_append(&datum->val, &val), fail);

    *out = datum;

    return e;

fail:
    free(datum);
    return e;
}

// recv_data_t is freed with just free
static derr_t recv_data_new(recv_data_t **out, const dstr_t key){
    derr_t e = E_OK;
    *out = NULL;

    if(key.len > KVPSYNC_MAX_LEN){
        ORIG(&e, E_INTERNAL, "key too long!");
    }

    recv_data_t *data = DMALLOC_STRUCT_PTR(&e, data);
    CHECK(&e);

    DSTR_WRAP_ARRAY(data->key, data->_key);
    PROP_GO(&e, dstr_append(&data->key, &key), fail);

    *out = data;

    return e;

fail:
    free(data);
    return e;
}

// returns true if data still exists
static bool data_remove_datum(recv_data_t *data, recv_datum_t *datum){
    link_remove(&datum->gc);
    link_remove(&datum->link);
    free(datum);
    if(!link_list_isempty(&data->list)) return true;
    // this data is empty
    hash_elem_remove(&data->elem);
    free(data);
    return false;
}

derr_t kvpsync_recv_init(kvpsync_recv_t *r){
    derr_t e = E_OK;

    // pick a random non-zero sync_id
    uint32_t recv_id = 0;
    while(!recv_id){
        dstr_t d = {
            .data = (char*)&recv_id,
            .len = 0,
            .size = sizeof(recv_id),
            .fixed_size = true,
        };
        PROP(&e, urandom_bytes(&d, d.size) );
    }

    *r = (kvpsync_recv_t){ .recv_id = recv_id };

    PROP(&e, hashmap_init(&r->h) );

    return e;
}

static void for_each_datum(
    kvpsync_recv_t *r, bool (*should_free)(kvpsync_recv_t*, recv_datum_t*)
){
    // free every data and datum
    hashmap_trav_t trav;
    hash_elem_t *elem = hashmap_iter(&trav, &r->h);
    for(; elem; elem = hashmap_next(&trav)){
        recv_data_t *data = CONTAINER_OF(elem, recv_data_t, elem);
        recv_datum_t *datum, *temp;
        LINK_FOR_EACH_SAFE(datum, temp, &data->list, recv_datum_t, link){
            if(!should_free(r, datum)) continue;
            bool data_ok = data_remove_datum(data, datum);
            if(!data_ok) break;
        }
    }
}


static bool _always_should_free(kvpsync_recv_t *r, recv_datum_t *datum){
    (void)r; (void)datum;
    return true;
}

void kvpsync_recv_free(kvpsync_recv_t *r){
    // free every data and datum
    for_each_datum(r, _always_should_free);
    // free hashmap
    hashmap_free(&r->h);
}

static void do_gc(kvpsync_recv_t *r, xtime_t now){
    recv_datum_t *datum, *temp;
    LINK_FOR_EACH_SAFE(datum, temp, &r->gc, recv_datum_t, gc){
        if(now < datum->gc_time) return;
        recv_data_t *data = datum->data;
        data_remove_datum(data, datum);
    }
}

static bool _flush_should_free(kvpsync_recv_t *r, recv_datum_t *datum){
    return datum->sync_id != r->sync_id;
}

static void flush_stale_data(kvpsync_recv_t *r){
    for_each_datum(r, _flush_should_free);
}

static derr_t handle_recv_or_delete(
    kvpsync_recv_t *r,
    xtime_t now,
    uint32_t sync_id,
    uint32_t update_id,
    const dstr_t key,
    uint32_t delete_id,
    const dstr_t val
){
    derr_t e = E_OK;

    recv_data_t *new_data = NULL;
    recv_datum_t *new_datum = NULL;

    hash_elem_t *elem = hashmap_gets(&r->h, &key);
    if(!elem){
        // first item for this key, put a datum in a data in the hashmap
        PROP(&e, recv_data_new(&new_data, key) );
        PROP_GO(&e,
            recv_datum_new(
                &new_datum, new_data, sync_id, update_id, delete_id, val
            ),
        fail);
        PROP_GO(&e,
            hashmap_sets_unique(&r->h, &new_data->key, &new_data->elem),
        fail);
        link_list_append(&new_data->list, &new_datum->link);
        return e;
    }

    // secondary item for this key, check for duplicates or cancellations
    recv_data_t *data = CONTAINER_OF(elem, recv_data_t, elem);
    recv_datum_t *other, *temp;
    xtime_t gc_time = 0;
    LINK_FOR_EACH_SAFE(other, temp, &data->list, recv_datum_t, link){
        if(other->sync_id != sync_id) continue;
        if(other->update_id == update_id){
            // duplicate packet
            return e;
        }

        if(delete_id && other->update_id == delete_id){
            // we are a deletion which matches the other insertion
            gc_time = now + GC_DELAY;
            // remove other immediately
            link_remove(&other->link);
            free(other);
            // continue with inserting ourselves, but with a timer

        }else if(!delete_id && other->delete_id == update_id){
            // we are an insertion which matches the other deletion
            if(other->gc_time){
                // other is already on a timer
                return e;
            }
            // put other on a timer
            other->gc_time = now + GC_DELAY;
            link_list_append(&r->gc, &other->gc);
            return e;
        }
    }

    // add a datum to this key's data
    PROP(&e,
        recv_datum_new(&new_datum, data, sync_id, update_id, delete_id, val)
    );
    link_list_append(&data->list, &new_datum->link);
    if(gc_time){
        new_datum->gc_time = gc_time;
        link_list_append(&r->gc, &new_datum->gc);
    }

    return e;

fail:
    if(new_data) free(new_data);
    if(new_datum) free(new_datum);
    return e;
}

// process an incoming packet and configure the ack
// (pre-initial-syncs are acked with reset packets)
derr_t kvpsync_recv_handle_update(
    kvpsync_recv_t *r, xtime_t now, kvp_update_t update, kvp_ack_t *ack
){
    derr_t e = E_OK;

    *ack = (kvp_ack_t){
        .sync_id = update.sync_id,
        .update_id = update.update_id,
    };

    if(!r->initial_sync_acked){
        if(update.type != KVP_UPDATE_START || update.resync_id != r->recv_id){
            // send a resync request instead of a regular ack
            *ack = (kvp_ack_t){
                .sync_id = r->recv_id,
                .update_id = 0,
            };
            return e;
        }
        // got the start packet, ack it as normal
        r->initial_sync_acked = true;
        return e;
    }

    do_gc(r, now);

    /* process update.ok_expiry: we know the sender will not give us a valid
       ok_expiry until after we finish a sync, so we can safely ignore
       ok_expiry from packets that don't match our sync_id */
    if(update.sync_id == r->sync_id){
        r->ok_expiry = MAX(update.ok_expiry, r->ok_expiry);
    }

    dstr_t key;
    dstr_t val;
    switch(update.type){
        case KVP_UPDATE_EMPTY:
            // noop, just keepalives
            break;

        case KVP_UPDATE_START:
            // noop, sender-side resyncs happen passively to us
            break;

        case KVP_UPDATE_FLUSH:
            // ignore any packets before our resync is acknowldged
            if(!r->initial_sync_acked) break;
            r->sync_id = update.sync_id;
            r->ok_expiry = MAX(r->ok_expiry, update.ok_expiry);
            flush_stale_data(r);
            break;

        case KVP_UPDATE_INSERT:
        case KVP_UPDATE_DELETE:
            // ignore any packets before our resync is acknowldged
            if(!r->initial_sync_acked) break;
            DSTR_WRAP(key, update.key, update.klen, false);
            DSTR_WRAP(val, update.val, update.vlen, false);
            PROP(&e,
                handle_recv_or_delete(
                    r,
                    now,
                    update.sync_id,
                    update.update_id,
                    key,
                    update.delete_id,
                    val
                )
            );
            break;

        default:
            LOG_FATAL("invalid update.type (%x)\n", FU(update.type));
    }

    return e;
}

const dstr_t *UNSURE = &(dstr_t){0};

/* returns NULL, UNSURE, or an answer, and is only guaranteed to be valid
   until the next call to handle_update() */
const dstr_t *kvpsync_recv_get_value(
    kvpsync_recv_t *r, xtime_t now, const dstr_t key
){
    do_gc(r, now);

    // if we have no sync_id, then we can serve nothing at all
    if(!r->sync_id){
        return UNSURE;
    }

    // lookup the data we have for this key
    hash_elem_t *elem = hashmap_gets(&r->h, &key);
    if(!elem){
        // no information
        return now < r->ok_expiry ? NULL : UNSURE;
    }

    recv_data_t *data = CONTAINER_OF(elem, recv_data_t, elem);

    // use the datum with the highest update_id
    dstr_t *ans = NULL;
    uint32_t ans_update_id = 0;
    recv_datum_t *datum;
    LINK_FOR_EACH(datum, &data->list, recv_datum_t, link){
        // ignore any datum from the wrong sync_id
        if(datum->sync_id != r->sync_id) continue;
        // ignore any datum older than our current answer
        if(datum->update_id < ans_update_id) continue;
        ans = datum->delete_id ? NULL : &datum->val;
        ans_update_id = datum->update_id;
    }

    if(!ans_update_id){
        // no info matches sync_id at all
        return now < r->ok_expiry ? NULL : UNSURE;
    }

    if(!ans){
        // our latest info was deletion info
        return now < r->ok_expiry ? NULL : UNSURE;
    }

    // even if we are not OK, we serve positive results confidently
    return ans;
}
