#include "server/kvpsync/libkvpsync.h"

#include "test/test_utils.h"

#define START(syncid, resyncid) \
(kvp_update_t){ \
    .type = KVP_UPDATE_START, \
    .sync_id = syncid, \
    .resync_id = resyncid, \
}

#define EMPTY(syncid, updateid, okexpiry) \
(kvp_update_t){ \
    .type = KVP_UPDATE_START, \
    .ok_expiry = okexpiry, \
    .sync_id = syncid, \
    .update_id = updateid, \
}

#define FLUSH(syncid, updateid, okexpiry) \
(kvp_update_t){ \
    .type = KVP_UPDATE_FLUSH, \
    .ok_expiry = okexpiry, \
    .sync_id = syncid, \
    .update_id = updateid, \
}

static kvp_update_t INSERT(
    const char *k,
    const char *v,
    uint32_t sync_id,
    uint32_t update_id,
    xtime_t ok_expiry
){
    kvp_update_t out = {
        .type = KVP_UPDATE_INSERT,
        .ok_expiry = ok_expiry,
        .sync_id = sync_id,
        .update_id = update_id,
    };
    out.klen = (uint8_t)strlen(strcpy(out.key, k));
    out.vlen = (uint8_t)strlen(strcpy(out.val, v));
    return out;
}

static kvp_update_t DELETE(
    const char *k,
    uint32_t delete_id,
    uint32_t sync_id,
    uint32_t update_id,
    xtime_t ok_expiry
){
    kvp_update_t out = {
        .type = KVP_UPDATE_INSERT,
        .ok_expiry = ok_expiry,
        .sync_id = sync_id,
        .update_id = update_id,
        .delete_id = delete_id,
    };
    out.klen = (uint8_t)strlen(strcpy(out.key, k));
    return out;
}

#define MUST_IGNORE(_update) do { \
    bool ok; \
    PROP_GO(&e, kvpsync_recv_handle_update(&r, now, _update, &ok), cu); \
    EXPECT_B_GO(&e, "should_ack", ok, false, cu); \
} while(0)

#define MUST_HANDLE(_update) do { \
    bool ok; \
    PROP_GO(&e, kvpsync_recv_handle_update(&r, now, _update, &ok), cu); \
    EXPECT_B_GO(&e, "should_ack", ok, true, cu); \
} while(0)


static dstr_t display_value(const dstr_t *got){
    DSTR_STATIC(null, "NULL");
    DSTR_STATIC(unsure, "UNSURE");
    if(got == NULL) return null;
    if(got == UNSURE) return unsure;
    return *got;
}

#define EXPECT_REPLY(k, exp) do { \
    const dstr_t _k = DSTR_LIT(k); \
    const dstr_t _exp = DSTR_LIT(exp); \
    const dstr_t _got = display_value( \
        kvpsync_recv_get_value(&r, now, _k) \
    ); \
    if(dstr_eq(_got, _exp)) break; \
    ORIG_GO(&e, \
        E_VALUE, \
        "expected %x -> %x but got %x\n", \
        cu, \
        FD(&_k), \
        FD(&_exp), \
        FD(&_got) \
    ); \
} while(0)

static derr_t test_recv(void){
    derr_t e = E_OK;

    kvpsync_recv_t r = {0};
    // safe to free zeroized struct
    kvpsync_recv_free(&r);

    PROP_GO(&e, kvpsync_recv_init(&r), cu);

    xtime_t now = 1;

    // everything is ignored before the initial packet is acked
    MUST_IGNORE( INSERT("A", "aaa", 9, 3, 1*SECOND) );
    MUST_IGNORE( DELETE("B", 4, 9, 5, 1*SECOND) );
    MUST_IGNORE( FLUSH(9, 6, 1*SECOND) );
    EXPECT_B_GO(&e, "initial_sync_acked", r.initial_sync_acked, false, cu);
    EXPECT_U_GO(&e, "ok_expiry", r.ok_expiry, 0, cu);
    EXPECT_U_GO(&e, "num_elems", r.h.num_elems, 0, cu);
    EXPECT_B_GO(&e, "gc empty", link_list_isempty(&r.gc), true, cu);

    // all responses are UNSURE
    EXPECT_REPLY("A", "UNSURE");
    EXPECT_REPLY("Z", "UNSURE");

    // acknowledge resync request
    MUST_HANDLE( START(1, r.recv_id) );

    // resolution case 1: only a single insert
    MUST_HANDLE( INSERT("A", "aaa", 1, 2, 0) );

    // resolution case 2: two inserts
    MUST_HANDLE( INSERT("B", "old", 1, 3, 0) );
    MUST_HANDLE( INSERT("B", "new", 1, 5, 0) );

    // resolution case 3: old deletion, new insert
    MUST_HANDLE( DELETE("C", 7, 1, 8, 0) );
    MUST_HANDLE( INSERT("C", "new", 1, 9, 0) );

    // resolution case 4: old insert, new deletion
    MUST_HANDLE( INSERT("D", "old", 1, 11, 0) );
    MUST_HANDLE( DELETE("D", 13, 1, 14, 0) );

    // add some noise from wrong sync_ids
    MUST_HANDLE( INSERT("A", "wrong", 8, 2, 0) );
    MUST_HANDLE( INSERT("B", "wrong", 8, 3, 0) );
    MUST_HANDLE( INSERT("B", "new", 8, 5, 0) );
    MUST_HANDLE( DELETE("C", 7, 8, 8, 0) );
    MUST_HANDLE( INSERT("C", "wrong", 8, 9, 0) );
    MUST_HANDLE( INSERT("D", "wrong", 8, 11, 0) );
    MUST_HANDLE( DELETE("D", 13, 8, 14, 0) );

    // all responses are still UNSURE
    EXPECT_REPLY("A", "UNSURE");
    EXPECT_REPLY("B", "UNSURE");
    EXPECT_REPLY("C", "UNSURE");
    EXPECT_REPLY("D", "UNSURE");
    EXPECT_REPLY("Z", "UNSURE");

    // finish the sync
    xtime_t expire = now + 15*SECOND;
    MUST_HANDLE( FLUSH(1, 15, expire) );

    // duplicate packets are harmless
    MUST_HANDLE( INSERT("A", "aaa", 1, 2, 0) );
    MUST_HANDLE( INSERT("B", "old", 1, 3, 0) );
    MUST_HANDLE( INSERT("B", "new", 1, 5, 0) );
    MUST_HANDLE( DELETE("C", 7, 1, 8, 0) );
    MUST_HANDLE( INSERT("C", "new", 1, 9, 0) );
    MUST_HANDLE( INSERT("D", "old", 1, 11, 0) );
    MUST_HANDLE( DELETE("D", 13, 1, 14, 0) );

    // add more noise
    MUST_HANDLE( INSERT("A", "wrong", 8, 2, 0) );
    MUST_HANDLE( INSERT("B", "wrong", 8, 3, 0) );
    MUST_HANDLE( INSERT("B", "new", 8, 5, 0) );
    MUST_HANDLE( DELETE("C", 7, 8, 8, 0) );
    MUST_HANDLE( INSERT("C", "wrong", 8, 9, 0) );
    MUST_HANDLE( INSERT("D", "wrong", 8, 11, 0) );
    MUST_HANDLE( DELETE("D", 13, 8, 14, 0) );

    now = expire-1;

    // case 1: return the one value
    EXPECT_REPLY("A", "aaa");
    // case 2: return the newvalue
    EXPECT_REPLY("B", "new");
    // case 3: return the new insert
    EXPECT_REPLY("C", "new");
    // case 4: return NULL
    EXPECT_REPLY("D", "NULL");
    // no returning UNSURE now
    EXPECT_REPLY("Z", "NULL");

    // after expiring, negative answers become UNSURE
    now = expire + 10;
    EXPECT_REPLY("A", "aaa");
    EXPECT_REPLY("B", "new");
    EXPECT_REPLY("C", "new");
    EXPECT_REPLY("D", "UNSURE");
    EXPECT_REPLY("Z", "UNSURE");

    // a keepalive restores the answers
    expire = now + 15*SECOND;
    MUST_HANDLE( EMPTY(1, 16, expire) );
    EXPECT_REPLY("A", "aaa");
    EXPECT_REPLY("B", "new");
    EXPECT_REPLY("C", "new");
    EXPECT_REPLY("D", "NULL");
    EXPECT_REPLY("Z", "NULL");

    // simulate a server-side resync
    MUST_HANDLE( START(2, r.recv_id) );
    MUST_HANDLE( INSERT("A", "aaa2", 2, 2, 0) );
    MUST_HANDLE( INSERT("B", "old2", 2, 3, 0) );
    MUST_HANDLE( INSERT("B", "new2", 2, 5, 0) );
    MUST_HANDLE( DELETE("C", 7, 2, 8, 0) );
    MUST_HANDLE( INSERT("C", "new2", 2, 9, 0) );
    MUST_HANDLE( INSERT("D", "old2", 2, 11, 0) );
    MUST_HANDLE( DELETE("D", 13, 2, 14, 0) );

    // before flush, nothing changes
    EXPECT_REPLY("A", "aaa");
    EXPECT_REPLY("B", "new");
    EXPECT_REPLY("C", "new");
    EXPECT_REPLY("D", "NULL");
    EXPECT_REPLY("Z", "NULL");
    now = expire;
    EXPECT_REPLY("A", "aaa");
    EXPECT_REPLY("B", "new");
    EXPECT_REPLY("C", "new");
    EXPECT_REPLY("D", "UNSURE");
    EXPECT_REPLY("Z", "UNSURE");

    // after flush, the new data is ready
    expire = now + 15*SECOND;
    MUST_HANDLE( FLUSH(2, 15, expire) );
    EXPECT_REPLY("A", "aaa2");
    EXPECT_REPLY("B", "new2");
    EXPECT_REPLY("C", "new2");
    EXPECT_REPLY("D", "NULL");
    EXPECT_REPLY("Z", "NULL");

cu:
    kvpsync_recv_free(&r);

    return e;
}

static derr_t test_gc(void){
    derr_t e = E_OK;

    kvpsync_recv_t r = {0};
    PROP_GO(&e, kvpsync_recv_init(&r), cu);

    xtime_t now = 1;
    xtime_t expire = 10000*SECOND;

    MUST_HANDLE( START(1, r.recv_id) );
    MUST_HANDLE( FLUSH(1, 2, expire) );

    // case 1: deletion matches insertion, and persists for a while
    MUST_HANDLE( INSERT("A", "aaa", 1, 3, expire) );
    EXPECT_REPLY("A", "aaa");

    EXPECT_B_GO(&e, "gc empty", link_list_isempty(&r.gc), true, cu);

    MUST_HANDLE( DELETE("A", 3, 1, 4, expire) );
    EXPECT_REPLY("A", "NULL");

    EXPECT_B_GO(&e, "gc not empty", !link_list_isempty(&r.gc), true, cu);

    // duplicates are ignored
    MUST_HANDLE( INSERT("A", "aaa", 1, 3, expire) );
    EXPECT_REPLY("A", "NULL");
    MUST_HANDLE( DELETE("A", 3, 1, 4, expire) );
    EXPECT_REPLY("A", "NULL");

    // late duplicates do not affect gc timer
    now += GC_DELAY - 1;
    MUST_HANDLE( INSERT("A", "aaa", 1, 3, expire) );
    MUST_HANDLE( DELETE("A", 3, 1, 4, expire) );
    EXPECT_REPLY("A", "NULL");
    EXPECT_B_GO(&e, "gc not empty", !link_list_isempty(&r.gc), true, cu);

    now++;
    EXPECT_REPLY("A", "NULL");
    EXPECT_B_GO(&e, "gc empty", link_list_isempty(&r.gc), true, cu);

    // case 2: insertion matches deletion, and deletion persists
    MUST_HANDLE( DELETE("B", 5, 1, 6, expire) );
    EXPECT_REPLY("B", "NULL");

    EXPECT_B_GO(&e, "gc empty", link_list_isempty(&r.gc), true, cu);
    MUST_HANDLE( INSERT("B", "bbb", 1, 5, expire) );
    EXPECT_B_GO(&e, "gc not empty", !link_list_isempty(&r.gc), true, cu);

    MUST_HANDLE( INSERT("B", "bbb", 1, 5, expire) );
    EXPECT_REPLY("B", "NULL");
    MUST_HANDLE( DELETE("B", 5, 1, 6, expire) );
    EXPECT_REPLY("B", "NULL");

    // late duplicates do not affect gc timer
    now += GC_DELAY - 1;
    MUST_HANDLE( INSERT("B", "bbb", 1, 5, expire) );
    MUST_HANDLE( DELETE("B", 5, 1, 6, expire) );
    EXPECT_REPLY("A", "NULL");
    EXPECT_B_GO(&e, "gc not empty", !link_list_isempty(&r.gc), true, cu);

    now++;
    EXPECT_REPLY("B", "NULL");
    EXPECT_B_GO(&e, "gc empty", link_list_isempty(&r.gc), true, cu);

cu:
    kvpsync_recv_free(&r);

    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_recv(), test_fail);
    PROP_GO(&e, test_gc(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}

