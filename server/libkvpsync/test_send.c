#include "server/libkvpsync/libkvpsync.h"

#include "test/test_utils.h"

static void akcb(kvpsync_send_t *send, void *arg){
    (void)send;
    if(!arg) return;
    bool *done = arg;
    *done = true;
}

#define ADD_KEY(k, v, ptr) \
    kvpsync_send_add_key(&s, now, DSTR_LIT(k), DSTR_LIT(v), akcb, ptr)

#define DEL_KEY(k) \
    kvpsync_send_delete_key(&s, DSTR_LIT(k))

#define ACK(s, u) (kvp_ack_t){ .sync_id = s, .update_id = u }

#define ACK_FOR(update) ACK(update->sync_id, update->update_id)

// render deadline in ms instead of ns
#define EXPECT_DEADLINE_GO(e, name, got, exp, label) do { \
    EXPECT_U_GO(e, name, (got)/1000000, (exp)/1000000, label); \
} while(0)

#define EXPECT_NO_PKT(e, msg, _deadline) do { \
    EXPECT_NULL_GO(e, msg "::pkt", result.pkt, cu); \
    EXPECT_DEADLINE_GO(e, msg "::deadline", result.deadline, _deadline, cu); \
} while(0)

#define EXPECT_START(e, msg, _resync_id, _deadline) do { \
    EXPECT_NOT_NULL_GO(e, msg "::pkt", result.pkt, cu); \
    EXPECT_U_GO(e, msg "::type", result.pkt->type, KVP_UPDATE_START, cu); \
    EXPECT_U_GO(e, msg "::update_id", result.pkt->update_id, 1, cu); \
    EXPECT_U_GO(e, msg "::resync_id", result.pkt->resync_id, _resync_id, cu); \
    EXPECT_U_GO(e, msg "::ok_expiry", result.pkt->ok_expiry, 0, cu); \
    EXPECT_DEADLINE_GO(e, msg "::deadline", result.deadline, _deadline, cu); \
} while(0)

#define EXPECT_FLUSH(e, msg, _update_id, _ok_expiry, _deadline) do { \
    const kvp_update_t *pkt = result.pkt; \
    EXPECT_NOT_NULL_GO(e, msg "::pkt", result.pkt, cu); \
    EXPECT_U_GO(e, msg "::type", result.pkt->type, KVP_UPDATE_FLUSH, cu); \
    EXPECT_U_GO(e, msg "::update_id", result.pkt->update_id, _update_id, cu); \
    EXPECT_U_GO(e, msg "::ok_expiry", pkt->ok_expiry, _ok_expiry, cu); \
    EXPECT_DEADLINE_GO(e, msg "::deadline", result.deadline, _deadline, cu); \
} while(0)

#define EXPECT_STRN_GO(e, name, gotbuf, gotlen, exp, label) do { \
    if(strncmp(gotbuf, exp, gotlen) != 0){ \
        ORIG_GO(e, \
            E_VALUE, \
            "expected '%x' == '%x' but got '%x'", \
            label, \
            FS(name), FS(exp), FSN(gotbuf, gotlen) \
        ); \
    } \
} while (0)

#define EXPECT_INSERT(e, msg, k, v, _update_id, _ok_expiry,  _deadline) do { \
    const kvp_update_t *pkt = result.pkt; \
    EXPECT_NOT_NULL_GO(e, msg "::pkt", pkt, cu); \
    EXPECT_U_GO(e, msg "::type", pkt->type, KVP_UPDATE_INSERT, cu); \
    EXPECT_U_GO(e, msg "::update_id", pkt->update_id, _update_id, cu); \
    EXPECT_U_GO(e, msg "::ok_expiry", pkt->ok_expiry, _ok_expiry, cu); \
    EXPECT_STRN_GO(e, msg "::key", pkt->key, pkt->klen, k, cu); \
    EXPECT_STRN_GO(e, msg "::val", pkt->val, pkt->vlen, v, cu); \
    EXPECT_DEADLINE_GO(e, msg "::deadline", result.deadline, _deadline, cu); \
} while(0)

#define EXPECT_DELETE( \
    e, msg, k, _delete_id, _update_id, _ok_expiry,  _deadline \
) do { \
    const kvp_update_t *pkt = result.pkt; \
    EXPECT_NOT_NULL_GO(e, msg "::pkt", pkt, cu); \
    EXPECT_U_GO(e, msg "::type", pkt->type, KVP_UPDATE_DELETE, cu); \
    EXPECT_U_GO(e, msg "::update_id", pkt->update_id, _update_id, cu); \
    EXPECT_U_GO(e, msg "::ok_expiry", pkt->ok_expiry, _ok_expiry, cu); \
    EXPECT_STRN_GO(e, msg "::key", pkt->key, pkt->klen, k, cu); \
    EXPECT_U_GO(e, msg "::delete_id", pkt->delete_id, _delete_id, cu); \
    EXPECT_DEADLINE_GO(e, msg "::deadline", result.deadline, _deadline, cu); \
} while(0)

static derr_t test_send_sync_points(void){
    derr_t e = E_OK;

    kvpsync_send_t s = {0};
    // safe to free zeroized struct
    kvpsync_send_free(&s);

    xtime_t now = 1;

    PROP_GO(&e, kvpsync_send_init(&s, now, NULL, NULL), cu);

    // on init, ok_expiry starts 15 seconds out
    EXPECT_U_GO(&e, "ok_expiry-on-init", s.ok_expiry, now + MIN_RESPONSE, cu);

    // not testing congestion here
    s.inflight_limit = 1000;

    bool a_cb = false;
    bool b_cb = false;
    bool c_cb_1 = false;
    bool c_cb_2 = false;
    bool c_cb_3 = false;
    bool d_cb = false;
    bool e_cb = false;

    ADD_KEY("A", "aaa", &a_cb);
    ADD_KEY("B", "bbb", &b_cb);
    ADD_KEY("C", "ccc", &c_cb_1);

    kvpsync_run_t result;

    // expect the start packet first
    result = kvpsync_send_run(&s, now);
    xtime_t start_deadline = now + 1*SECOND;
    EXPECT_START(&e, "start", 0, start_deadline);
    kvp_ack_t start_ack = ACK_FOR(result.pkt);

    // send refuses to respond until this packet is acked
    now += 100*MILLISECOND;
    result = kvpsync_send_run(&s, now);
    EXPECT_NO_PKT(&e, "rerun-after-start", start_deadline);

    now += 100*MILLISECOND;
    ADD_KEY("D", "ddd", &d_cb);
    result = kvpsync_send_run(&s, now);
    EXPECT_NO_PKT(&e, "add-after-start", start_deadline);

    now += 100*MILLISECOND;
    ADD_KEY("C", "---", &c_cb_2);
    EXPECT_NO_PKT(&e, "modify-after-start", start_deadline);

    // send a wrong ack
    now += 100*MILLISECOND;
    kvpsync_send_handle_ack(&s, ACK(10, 10), now);
    result = kvpsync_send_run(&s, now);
    EXPECT_NO_PKT(&e, "wrong-ack-after-start", start_deadline);

    // send the actual ack, and extract all the inserts
    // (note, inserts are emitted in deterministic-but-unimportant order)
    now += 100*MILLISECOND;
    xtime_t ack2_deadline = now + 1*SECOND;
    kvpsync_send_handle_ack(&s, start_ack, now);
    kvpsync_send_handle_ack(&s, start_ack, now); // duplicate
    result = kvpsync_send_run(&s, now);
    EXPECT_INSERT(&e, "ack-after-start:1", "A", "aaa", 2, 0, 0);
    kvp_ack_t ack2 = ACK_FOR(result.pkt);
    //
    now += 100*MILLISECOND;
    result = kvpsync_send_run(&s, now);
    EXPECT_INSERT(&e, "ack-after-start:2", "D", "ddd", 3, 0, 0);
    kvp_ack_t ack3 = ACK_FOR(result.pkt);
    //
    now += 100*MILLISECOND;
    result = kvpsync_send_run(&s, now);
    EXPECT_INSERT(&e, "ack-after-start:3", "B", "bbb", 4, 0, 0);
    kvp_ack_t ack4 = ACK_FOR(result.pkt);
    //
    now += 100*MILLISECOND;
    xtime_t ack5_deadline = now + 1*SECOND;
    result = kvpsync_send_run(&s, now);
    EXPECT_INSERT(&e, "ack-after-start:4", "C", "---", 5, 0, ack2_deadline);
    kvp_ack_t ack5 = ACK_FOR(result.pkt);

    // no more packets
    now += 100*MILLISECOND;
    result = kvpsync_send_run(&s, now);
    EXPECT_NO_PKT(&e, "after-start-inserts", ack2_deadline);

    // updates can be emitted now
    now += 100*MILLISECOND;
    ADD_KEY("C", "c", &c_cb_3);
    result = kvpsync_send_run(&s, now);
    EXPECT_DELETE(&e, "modify-after-insert:1", "C", 5, 6, 0, 0);
    kvp_ack_t ack6 = ACK_FOR(result.pkt);
    //
    result = kvpsync_send_run(&s, now);
    EXPECT_INSERT(&e, "modify-after-insert:2", "C", "c", 7, 0, ack2_deadline);
    kvp_ack_t ack7 = ACK_FOR(result.pkt);
    //
    result = kvpsync_send_run(&s, now);
    EXPECT_NO_PKT(&e, "modify-after-insert:3", ack2_deadline);

    // receive most of the acks
    now += 100*MILLISECOND;
    EXPECT_B_GO(&e, "a_cb", a_cb, false, cu);
    EXPECT_B_GO(&e, "b_cb", b_cb, false, cu);
    EXPECT_B_GO(&e, "c_cb_1", c_cb_1, false, cu);
    EXPECT_B_GO(&e, "c_cb_2", c_cb_2, false, cu);
    EXPECT_B_GO(&e, "c_cb_3", c_cb_3, false, cu);
    EXPECT_B_GO(&e, "d_cb", d_cb, false, cu);
    kvpsync_send_handle_ack(&s, ack2, now);
    EXPECT_B_GO(&e, "a_cb", a_cb, true, cu);
    kvpsync_send_handle_ack(&s, ack2, now);
    kvpsync_send_handle_ack(&s, ack3, now);
    EXPECT_B_GO(&e, "d_cb", d_cb, true, cu);
    kvpsync_send_handle_ack(&s, ack3, now);
    kvpsync_send_handle_ack(&s, ack4, now);
    EXPECT_B_GO(&e, "b_cb", b_cb, true, cu);
    kvpsync_send_handle_ack(&s, ack4, now);
    kvpsync_send_handle_ack(&s, ack6, now);
    kvpsync_send_handle_ack(&s, ack7, now);
    EXPECT_B_GO(&e, "c_cb_3", c_cb_3, true, cu);
    kvpsync_send_handle_ack(&s, ack6, now);
    kvpsync_send_handle_ack(&s, ack7, now);
    // c_cb_1 and c_cb_2 are left behind
    EXPECT_B_GO(&e, "c_cb_1", c_cb_1, false, cu);
    EXPECT_B_GO(&e, "c_cb_2", c_cb_2, false, cu);
    result = kvpsync_send_run(&s, now);
    EXPECT_NO_PKT(&e, "miss-one-ack", ack5_deadline);

    // the final ack exposes the flush, and finally gives an ok_expiry
    now += 100*MILLISECOND;
    xtime_t ok_expiry = now + MIN_RESPONSE;
    xtime_t flush_deadline = now + 1*SECOND;
    kvpsync_send_handle_ack(&s, ack5, now);
    EXPECT_B_GO(&e, "c_cb_2", c_cb_2, true, cu);
    kvpsync_send_handle_ack(&s, ack5, now);
    // c_cb_1 is forever left behind
    EXPECT_B_GO(&e, "c_cb_1", c_cb_1, false, cu);
    result = kvpsync_send_run(&s, now);
    EXPECT_FLUSH(&e, "flush", 8, ok_expiry, flush_deadline);
    kvp_ack_t ack8 = ACK_FOR(result.pkt);

    // deletes are emitted right away
    now += 100*MILLISECOND;
    xtime_t del_b_deadline = now + 1*SECOND;
    DEL_KEY("B");
    result = kvpsync_send_run(&s, now);
    EXPECT_DELETE(&e, "del_b:1", "B", 4, 9, ok_expiry, flush_deadline);
    kvp_ack_t ack9 = ACK_FOR(result.pkt);
    //
    result = kvpsync_send_run(&s, now);
    EXPECT_NO_PKT(&e, "del_b:2", flush_deadline);

    // inserts are emitted right away too
    now += 100*MILLISECOND;
    kvpsync_send_handle_ack(&s, ack8, now);
    now += 100*MILLISECOND;  // ok_expiry calculated at send time, not ack time
    kvpsync_send_handle_ack(&s, ack5, now);
    ok_expiry = now + MIN_RESPONSE;
    ADD_KEY("E", "eee", &e_cb);
    result = kvpsync_send_run(&s, now);
    kvp_ack_t ack10 = ACK_FOR(result.pkt);
    EXPECT_INSERT(&e, "ins_e:1", "E", "eee", 10, ok_expiry, del_b_deadline);

    // trigger a resync
    now += 100*MILLISECOND;
    start_deadline = now + 1*SECOND;
    s.sync_id = UINT32_MAX;  // test sync_id overflow detection
    kvpsync_send_handle_ack(&s, ACK(77, 0), now);
    EXPECT_U_GO(&e, "sync_id", s.sync_id, 1, cu);  // UINT32_MAX -> 1
    // on resync, ok_expiry goes to zero
    EXPECT_U_GO(&e, "ok_expiry-on-resync", s.ok_expiry, 0, cu);
    result = kvpsync_send_run(&s, now);
    EXPECT_START(&e, "resync:1", 77, start_deadline);
    kvp_ack_t ack_resync77 = ACK_FOR(result.pkt);
    kvpsync_send_handle_ack(&s, ACK(777, 0), now);
    EXPECT_U_GO(&e, "sync_id", s.sync_id, 2, cu);  // 1 -> 2
    result = kvpsync_send_run(&s, now);
    EXPECT_START(&e, "resync:2", 777, start_deadline);
    kvp_ack_t ack_resync777 = ACK_FOR(result.pkt);
    result = kvpsync_send_run(&s, now);
    EXPECT_NO_PKT(&e, "resync:3", start_deadline);

    // old acks affect nothing
    kvpsync_send_handle_ack(&s, ack9, now);
    kvpsync_send_handle_ack(&s, ack10, now);
    kvpsync_send_handle_ack(&s, ack_resync77, now);
    result = kvpsync_send_run(&s, now);
    EXPECT_NO_PKT(&e, "resync:4", start_deadline);
    EXPECT_B_GO(&e, "e_cb", e_cb, false, cu);

    kvpsync_send_handle_ack(&s, ack_resync777, now);
    result = kvpsync_send_run(&s, now);
    EXPECT_INSERT(&e, "ack-after-start:1", "A", "aaa", 2, 0, 0);
    ack2 = ACK_FOR(result.pkt);
    result = kvpsync_send_run(&s, now);
    EXPECT_INSERT(&e, "ack-after-start:2", "D", "ddd", 3, 0, 0);
    ack3 = ACK_FOR(result.pkt);

    // duplicate resync affects nothing
    kvpsync_send_handle_ack(&s, ACK(777, 0), now);

    result = kvpsync_send_run(&s, now);
    EXPECT_INSERT(&e, "ack-after-start:3", "E", "eee", 4, 0, 0);
    ack4 = ACK_FOR(result.pkt);
    result = kvpsync_send_run(&s, now);
    EXPECT_INSERT(&e, "ack-after-start:4", "C", "c", 5, 0, now + 1*SECOND);
    ack5 = ACK_FOR(result.pkt);

    kvpsync_send_handle_ack(&s, ack2, now);
    kvpsync_send_handle_ack(&s, ack3, now);
    EXPECT_B_GO(&e, "e_cb", e_cb, false, cu);
    kvpsync_send_handle_ack(&s, ack4, now);
    // e_cb is still honored after resync
    EXPECT_B_GO(&e, "e_cb", e_cb, true, cu);
    kvpsync_send_handle_ack(&s, ack5, now);

cu:
    kvpsync_send_free(&s);
    // safe to free twice
    kvpsync_send_free(&s);

    return e;
}

#define ACK_ALL() do { \
    for(size_t i = 0; i < n; i++){ \
        kvpsync_send_handle_ack(&s, acks[i], now); \
    } \
    n = 0; \
} while(0)

static derr_t test_send_congestion(void){
    derr_t e = E_OK;

    kvpsync_send_t s = {0};
    xtime_t now = 1;
    PROP_GO(&e, kvpsync_send_init(&s, now, NULL, NULL), cu);
    kvpsync_run_t result;

    kvp_ack_t acks[8];
    size_t n = 0;

    result = kvpsync_send_run(&s, now);
    EXPECT_START(&e, "start:1", 0, now + 1*SECOND);
    kvpsync_send_handle_ack(&s, ACK_FOR(result.pkt), now);
    EXPECT_I_GO(&e, "limit", s.inflight_limit, 2, cu);

    // fill pipe
    ADD_KEY("A", "aaa", NULL);
    result = kvpsync_send_run(&s, now);
    acks[n++] = ACK_FOR(result.pkt);

    ADD_KEY("B", "bbb", NULL);
    result = kvpsync_send_run(&s, now);
    acks[n++] = ACK_FOR(result.pkt);

    ADD_KEY("C", "ccc", NULL);
    result = kvpsync_send_run(&s, now);
    EXPECT_NULL_GO(&e, "pkt", result.pkt, cu);

    // respond to all
    ACK_ALL();
    // expect limit increase
    EXPECT_I_GO(&e, "limit", s.inflight_limit, 3, cu);

    // fill pipe again
    result = kvpsync_send_run(&s, now);
    acks[n++] = ACK_FOR(result.pkt);
    ADD_KEY("D", "ddd", NULL);
    result = kvpsync_send_run(&s, now);
    acks[n++] = ACK_FOR(result.pkt);
    ADD_KEY("E", "eee", NULL);
    result = kvpsync_send_run(&s, now);
    acks[n++] = ACK_FOR(result.pkt);
    ADD_KEY("F", "fff", NULL);
    result = kvpsync_send_run(&s, now);
    EXPECT_NULL_GO(&e, "pkt", result.pkt, cu);

    // respond to all
    ACK_ALL();
    // expect limit increase
    EXPECT_I_GO(&e, "limit", s.inflight_limit, 4, cu);

    s.inflight_limit = 500;

    // drop one packet but not all packets
    result = kvpsync_send_run(&s, now);
    acks[n++] = ACK_FOR(result.pkt);
    ADD_KEY("G", "ggg", NULL);
    result = kvpsync_send_run(&s, now);
    now += 100*MILLISECOND;
    kvpsync_send_handle_ack(&s, ACK_FOR(result.pkt), now);

    now += 1*SECOND;
    result = kvpsync_send_run(&s, now);
    EXPECT_NOT_NULL_GO(&e, "resent pkt", result.pkt, cu);
    // expect limit decrease
    EXPECT_I_GO(&e, "limit", s.inflight_limit, 400, cu);

    // drop all packets
    ADD_KEY("H", "hhh", NULL);
    result = kvpsync_send_run(&s, now);
    now += 1*SECOND;
    result = kvpsync_send_run(&s, now);
    EXPECT_NOT_NULL_GO(&e, "resent pkt", result.pkt, cu);
    // expect limit reset
    EXPECT_I_GO(&e, "limit", s.inflight_limit, 1, cu);
    EXPECT_I_GO(&e, "limit", s.inflight, 1, cu);

cu:
    kvpsync_send_free(&s);

    return e;
}

static derr_t test_no_stale_cbs(void){
    derr_t e = E_OK;

    xtime_t now = 1;

    kvpsync_send_t s;
    PROP_GO(&e, kvpsync_send_init(&s, now, NULL, NULL), cu);

    // not testing congestion here
    s.inflight_limit = 1000;

    kvpsync_run_t result;

    // expect the start packet
    result = kvpsync_send_run(&s, now);
    EXPECT_START(&e, "start", 0, now + 1*SECOND);
    kvp_ack_t start_ack = ACK_FOR(result.pkt);

    // send the start ack, and expect the flush
    now += 1*MILLISECOND;
    kvpsync_send_handle_ack(&s, start_ack, now);
    result = kvpsync_send_run(&s, now);
    xtime_t flush_expiry = now + 15*SECOND;
    EXPECT_FLUSH(&e, "flush", 2, flush_expiry, now + 1*SECOND);
    kvp_ack_t flush_ack = ACK_FOR(result.pkt);

    now += 1*MILLISECOND;
    kvpsync_send_handle_ack(&s, flush_ack, now);

    bool a_cb = false;
    bool b_cb = false;
    bool B_cb = false;

    ADD_KEY("A", "aaa", &a_cb);
    ADD_KEY("B", "bbb", &b_cb);
    result = kvpsync_send_run(&s, now);
    xtime_t a_expiry = now + 15*SECOND;
    xtime_t a_deadline = now + 1*SECOND;
    EXPECT_INSERT(&e, "ack-a", "A", "aaa", 3, a_expiry, 0);
    kvp_ack_t ack1 = ACK_FOR(result.pkt);
    //
    now += 1*MILLISECOND;
    result = kvpsync_send_run(&s, now);
    EXPECT_INSERT(&e, "ack-b", "B", "bbb", 4, a_expiry, a_deadline);
    kvp_ack_t ack2 = ACK_FOR(result.pkt);

    // now delete A and B
    now += 1*MILLISECOND;
    DEL_KEY("A");
    DEL_KEY("B");
    result = kvpsync_send_run(&s, now);
    EXPECT_DELETE(&e, "delete-a", "A", 3, 5, a_expiry, 0);
    kvp_ack_t ack3 = ACK_FOR(result.pkt);
    result = kvpsync_send_run(&s, now);
    EXPECT_DELETE(&e, "delete-b", "B", 4, 6, a_expiry, a_deadline);
    kvp_ack_t ack4 = ACK_FOR(result.pkt);

    // now add a different B
    now += 1*MILLISECOND;
    ADD_KEY("B", "BBB", &B_cb);
    result = kvpsync_send_run(&s, now);
    EXPECT_INSERT(&e, "ack-B", "B", "BBB", 7, a_expiry, a_deadline);
    kvp_ack_t ack5 = ACK_FOR(result.pkt);

    now += 1*MILLISECOND;
    // ack everything, expect specific callbacks
    EXPECT_B_GO(&e, "a_cb before", a_cb, false, cu);
    EXPECT_B_GO(&e, "b_cb before", b_cb, false, cu);
    EXPECT_B_GO(&e, "B_cb before", B_cb, false, cu);
    kvpsync_send_handle_ack(&s, ack1, now);
    kvpsync_send_handle_ack(&s, ack2, now);
    kvpsync_send_handle_ack(&s, ack3, now);
    kvpsync_send_handle_ack(&s, ack4, now);
    kvpsync_send_handle_ack(&s, ack5, now);
    EXPECT_B_GO(&e, "a_cb after", a_cb, false, cu);
    EXPECT_B_GO(&e, "b_cb after", b_cb, false, cu);
    EXPECT_B_GO(&e, "B_cb after", B_cb, true, cu);

cu:
    kvpsync_send_free(&s);

    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_send_sync_points(), test_fail);
    PROP_GO(&e, test_send_congestion(), test_fail);
    PROP_GO(&e, test_no_stale_cbs(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
