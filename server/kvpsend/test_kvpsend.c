#include "server/kvpsend/kvpsend.h"

#include "test/test_utils.h"

typedef enum {
    SCAN_TIMER_START,
    TIMEOUT_TIMER_START,
    TIMEOUT_TIMER_STOP,
    SUBSCRIBER_CLOSE,
    SUBSCRIBER_RESPOND,
    SENDER_SEND_PKT,
    SENDER_TIMER_START,
    SENDER_TIMER_STOP,
} call_e;

static const char *call2str(call_e type){
    switch(type){
        case SCAN_TIMER_START: return "SCAN_TIMER_START";
        case TIMEOUT_TIMER_START: return "TIMEOUT_TIMER_START";
        case TIMEOUT_TIMER_STOP: return "TIMEOUT_TIMER_STOP";
        case SUBSCRIBER_CLOSE: return "SUBSCRIBER_CLOSE";
        case SUBSCRIBER_RESPOND: return "SUBSCRIBER_RESPOND";
        case SENDER_SEND_PKT: return "SENDER_SEND_PKT";
        case SENDER_TIMER_START: return "SENDER_TIMER_START";
        case SENDER_TIMER_STOP: return "SENDER_TIMER_STOP";
    }
    return "unknown";
}

typedef struct {
    char *name;
    subscriber_t sub;
} test_subscriber_t;
DEF_CONTAINER_OF(test_subscriber_t, sub, subscriber_t)

typedef struct {
    call_e type;
    xtime_t deadline;
    test_subscriber_t *sub;
    char *msg;
    int sender;
    kvp_update_t *pkt;
} call_t;

typedef enum {
    SCAN_TIMER,
    TIMEOUT_TIMER,
    SENDER_TIMER,
} timer_type_e;

typedef struct {
    timer_type_e type;
    xtime_t deadline;
    hnode_t hnode;
    bool active;
} deadline_t;
DEF_CONTAINER_OF(deadline_t, hnode, hnode_t)

static const void *deadline_heap_get(const hnode_t *hnode){
    return &CONTAINER_OF(hnode, deadline_t, hnode)->deadline;
}

typedef struct {
    kvpsend_i iface;
    kvpsend_t k;
    heap_t heap;
    deadline_t scan_timer;
    deadline_t timeout_timer;
    deadline_t sender_timers[MAX_PEERS];
    derr_t error; // allocated error, might get stolen
    bool failing; // flag, gets set with first error, never unset
    call_t calls[8];
    size_t ncalls;
    size_t call_idx;
    // for database calls
    jsw_atree_t tree;  // test_challenge_t->node
    jsw_atrav_t trav;
} kvpsend_test_t;
DEF_CONTAINER_OF(kvpsend_test_t, iface, kvpsend_i)

typedef struct {
    dstr_t inst_uuid;
    dstr_t subdomain;
    dstr_t challenge;
    jsw_anode_t node;  // test_db_t->tree
} test_challenge_t;
DEF_CONTAINER_OF(test_challenge_t, node, jsw_anode_t)

// requires string literals
static test_challenge_t test_challenge(
    char *inst_uuid, char *subdomain, char *challenge
){
    return (test_challenge_t){
        .inst_uuid = dstr_from_cstr(inst_uuid),
        .subdomain = dstr_from_cstr(subdomain),
        .challenge = dstr_from_cstr(challenge),
    };
}

static const void *jsw_challenge_get(const jsw_anode_t *node){
    return &CONTAINER_OF(node, test_challenge_t, node)->subdomain;
}

static void challenge_insert(kvpsend_test_t *T, test_challenge_t *tc){
    jsw_ainsert(&T->tree, &tc->node);
}

static void challenge_delete(kvpsend_test_t *T, test_challenge_t *tc){
    jsw_anode_t *old = jsw_aerase(&T->tree, &tc->subdomain);
    if(old != &tc->node) LOG_FATAL("erased wrong node\n");
}

static test_challenge_t *challenge_first(kvpsend_test_t *T){
    jsw_anode_t *node = jsw_atfirst(&T->trav, &T->tree);
    return CONTAINER_OF(node, test_challenge_t, node);
}

static test_challenge_t *challenge_next(kvpsend_test_t *T){
    jsw_anode_t *node = jsw_atnext(&T->trav);
    return CONTAINER_OF(node, test_challenge_t, node);
}

static void tc_to_challenge_iter(test_challenge_t *tc, challenge_iter_t *it){
    if(tc){
        it->ok = true;
        it->subdomain = tc->subdomain;
        it->challenge = tc->challenge;
    }else{
        it->ok = false;
    }
}

static derr_t _challenges_first(kvpsend_i *iface, challenge_iter_t *it){
    kvpsend_test_t *T = CONTAINER_OF(iface, kvpsend_test_t, iface);

    derr_t e = E_OK;

    test_challenge_t *tc = challenge_first(T);
    while(tc && tc->challenge.len == 0){
        tc = challenge_next(T);
    }
    tc_to_challenge_iter(tc, it);

    return e;
}

static derr_t _challenges_next(kvpsend_i *iface, challenge_iter_t *it){
    kvpsend_test_t *T = CONTAINER_OF(iface, kvpsend_test_t, iface);

    derr_t e = E_OK;

    test_challenge_t *tc = challenge_next(T);
    while(tc && tc->challenge.len == 0){
        tc = challenge_next(T);
    }
    tc_to_challenge_iter(tc, it);

    return e;
}

static void _challenges_free(kvpsend_i *iface, challenge_iter_t *it){
    (void)iface;
    (void)it;
}

static derr_t _get_installation_challenge(
    kvpsend_i *iface,
    const dstr_t inst_uuid,
    dstr_t *subdomain,
    bool *subdomain_ok,
    dstr_t *challenge,
    bool *challenge_ok
){
    kvpsend_test_t *T = CONTAINER_OF(iface, kvpsend_test_t, iface);

    derr_t e = E_OK;

    *subdomain_ok = false;
    *challenge_ok = false;

    // linear scan for matching inst_uuid
    test_challenge_t *tc = challenge_first(T);
    for(; tc; tc = challenge_next(T)){
        if(dstr_eq(inst_uuid, tc->inst_uuid)){
            goto found;
        }
    }
    // not found
    return e;

found:
    PROP(&e, dstr_append(subdomain, &tc->subdomain) );
    *subdomain_ok = true;

    if(tc->challenge.len == 0) return e;

    PROP(&e, dstr_append(challenge, &tc->challenge) );
    *challenge_ok = true;

    return e;
}

#define EXPECT_SUBSCRIBER_GO(e, got, exp, label) do { \
    test_subscriber_t *_got = CONTAINER_OF((got), test_subscriber_t, sub); \
    test_subscriber_t *_exp = (exp); \
    if(_got == _exp) break; \
    ORIG_GO(e, \
        E_VALUE, \
        "expected subscriber \"%x\" but got \"%x\"", \
        label, \
        FS(_exp->name), \
        FS(_got->name) \
    ); \
} while(0)

#define EXPECT_CALL_GO(e, idx, typ, label) do { \
    if(idx >= T->ncalls) { \
        ORIG_GO(e, \
            E_VALUE, #typ " makes for too many calls (%x)", label, FU(idx+1) \
        ); \
    } \
    call_e t = T->calls[idx].type; \
    if(typ != t){ \
        ORIG_GO(e, \
            E_VALUE, "expected %x but got " #typ, label, FS(call2str(t)) \
        ); \
    } \
} while(0)

#define expect_call(_call) do { \
    if(T.ncalls >= sizeof(T.calls) / sizeof(*T.calls)){ \
        LOG_FATAL("too many expected calls!\n"); \
    } \
    T.calls[T.ncalls++] = (_call); \
} while(0)

static void Tfail(kvpsend_test_t *T, derr_t e){
    if(!T->failing){
        T->failing = true;
        T->error = e;
    }else{
        DROP_VAR(&e);
    }
}

static void _scan_timer_start(kvpsend_i *iface, xtime_t deadline){
    kvpsend_test_t *T = CONTAINER_OF(iface, kvpsend_test_t, iface);

    if(T->failing) return;

    derr_t e = E_OK;

    size_t idx = T->call_idx++;
    EXPECT_CALL_GO(&e, idx, SCAN_TIMER_START, fail);
    EXPECT_U_GO(&e, "deadline", deadline, T->calls[idx].deadline, fail);
    EXPECT_B_GO(&e, "active", T->scan_timer.active, false, fail);

    T->scan_timer.deadline = deadline;
    T->scan_timer.active = true;
    heap_put(&T->heap, &T->scan_timer.hnode);

    return;

fail:
    Tfail(T, e);
}

static void _timeout_timer_start(kvpsend_i *iface, xtime_t deadline){
    kvpsend_test_t *T = CONTAINER_OF(iface, kvpsend_test_t, iface);

    if(T->failing) return;

    derr_t e = E_OK;

    size_t idx = T->call_idx++;
    EXPECT_CALL_GO(&e, idx, TIMEOUT_TIMER_START, fail);
    EXPECT_U_GO(&e, "deadline", deadline, T->calls[idx].deadline, fail);
    EXPECT_B_GO(&e, "active", T->timeout_timer.active, false, fail);

    T->timeout_timer.deadline = deadline;
    T->timeout_timer.active = true;
    heap_put(&T->heap, &T->timeout_timer.hnode);

    return;

fail:
    Tfail(T, e);
}

static void _timeout_timer_stop(kvpsend_i *iface){
    kvpsend_test_t *T = CONTAINER_OF(iface, kvpsend_test_t, iface);

    if(T->failing) return;

    derr_t e = E_OK;

    size_t idx = T->call_idx++;
    EXPECT_CALL_GO(&e, idx, TIMEOUT_TIMER_STOP, fail);
    EXPECT_B_GO(&e, "active", T->timeout_timer.active, true, fail);

    T->timeout_timer.active = false;
    hnode_remove(&T->timeout_timer.hnode);

    return;

fail:
    Tfail(T, e);
}

static void _subscriber_close(kvpsend_i *iface, subscriber_t *sub){
    kvpsend_test_t *T = CONTAINER_OF(iface, kvpsend_test_t, iface);
    if(T->failing) return;

    derr_t e = E_OK;

    size_t idx = T->call_idx++;
    EXPECT_CALL_GO(&e, idx, SUBSCRIBER_CLOSE, fail);
    EXPECT_SUBSCRIBER_GO(&e, sub, T->calls[idx].sub, fail);

    return;

fail:
    Tfail(T, e);
    return;
}

static void _subscriber_respond(
    kvpsend_i *iface, subscriber_t *sub, dstr_t msg
){
    kvpsend_test_t *T = CONTAINER_OF(iface, kvpsend_test_t, iface);
    if(T->failing) return;

    derr_t e = E_OK;

    size_t idx = T->call_idx++;
    EXPECT_CALL_GO(&e, idx, SUBSCRIBER_RESPOND, fail);
    EXPECT_SUBSCRIBER_GO(&e, sub, T->calls[idx].sub, fail);
    EXPECT_D_GO(&e, "msg", msg, dstr_from_cstr(T->calls[idx].msg), fail);

    return;

fail:
    Tfail(T, e);
}

static derr_t _sender_send_pkt(
    kvpsend_i *iface, sender_t *sender, const kvp_update_t *pkt
){
    derr_t e = E_OK;

    kvpsend_test_t *T = CONTAINER_OF(iface, kvpsend_test_t, iface);

    if(is_error(T->error)){
        e = T->error;
        T->error = (derr_t){0};
        return e;
    }

    // figure out which sender it is
    ptrdiff_t sender_idx = sender - T->k.senders;

    size_t idx = T->call_idx++;
    EXPECT_CALL_GO(&e, idx, SENDER_SEND_PKT, fail);
    EXPECT_I_GO(&e, "sender", sender_idx, T->calls[idx].sender, fail);

    // store the packet for external inspection
    *T->calls[idx].pkt = *pkt;

    return e;

fail:
    // don't let other errors get generated
    T->failing = true;
    return e;
}

static void _sender_timer_start(
    kvpsend_i *iface, sender_t *sender, xtime_t deadline
){
    kvpsend_test_t *T = CONTAINER_OF(iface, kvpsend_test_t, iface);

    if(T->failing) return;

    derr_t e = E_OK;

    size_t idx = T->call_idx++;
    EXPECT_CALL_GO(&e, idx, SENDER_TIMER_START, fail);

    // figure out which sender it is
    ptrdiff_t sender_idx = sender - T->k.senders;
    EXPECT_I_GO(&e, "sender", sender_idx, T->calls[idx].sender, fail);

    EXPECT_U_GO(&e, "deadline", deadline, T->calls[idx].deadline, fail);

    // get the right timer
    deadline_t *timer = &T->sender_timers[sender_idx];

    EXPECT_B_GO(&e, "active", timer->active, false, fail);
    timer->deadline = deadline;
    timer->active = true;
    heap_put(&T->heap, &timer->hnode);

    return;

fail:
    Tfail(T, e);
}

static void _sender_timer_stop(kvpsend_i *iface, sender_t *sender){
    kvpsend_test_t *T = CONTAINER_OF(iface, kvpsend_test_t, iface);
    if(T->failing) return;

    derr_t e = E_OK;

    size_t idx = T->call_idx++;
    EXPECT_CALL_GO(&e, idx, SENDER_TIMER_STOP, fail);

    // figure out which sender it is
    ptrdiff_t sender_idx = sender - T->k.senders;
    EXPECT_I_GO(&e, "sender", sender_idx, T->calls[idx].sender, fail);

    // get the right timer
    deadline_t *timer = &T->sender_timers[sender_idx];

    EXPECT_B_GO(&e, "active", timer->active, true, fail);
    timer->deadline = XTIME_MAX;
    timer->active = false;
    hnode_remove(&timer->hnode);

    return;

fail:
    Tfail(T, e);
}

static void kvpsend_test_prep(kvpsend_test_t *T){
    *T = (kvpsend_test_t){
        .iface = {
            .scan_timer_start = _scan_timer_start,
            .timeout_timer_start = _timeout_timer_start,
            .timeout_timer_stop = _timeout_timer_stop,
            .subscriber_close = _subscriber_close,
            .subscriber_respond = _subscriber_respond,
            .sender_send_pkt = _sender_send_pkt,
            .sender_timer_start = _sender_timer_start,
            .sender_timer_stop = _sender_timer_stop,

            .challenges_first = _challenges_first,
            .challenges_next = _challenges_next,
            .challenges_free = _challenges_free,
            .get_installation_challenge = _get_installation_challenge,
        },
        .scan_timer = { .type = SCAN_TIMER },
        .timeout_timer = { .type = TIMEOUT_TIMER },
    };
    jsw_ainit(&T->tree, jsw_cmp_dstr, jsw_challenge_get);
    heap_prep(&T->heap, jsw_cmp_uint64, deadline_heap_get, false);
    size_t ntimers = sizeof(T->sender_timers) / sizeof(*T->sender_timers);
    for(size_t i = 0; i < ntimers; i++){
        T->sender_timers[i].type = SENDER_TIMER;
    }
}

static deadline_t *deadline_pop(kvpsend_test_t *T){
    hnode_t *hnode = heap_pop(&T->heap);
    deadline_t *deadline = CONTAINER_OF(hnode, deadline_t, hnode);
    if(deadline) deadline->active = false;
    return deadline;
}

#define EXPECT_UPDATE_TYPE_GO(e, got, exp, label) do { \
    kvp_update_type_e _got = (got).type; \
    kvp_update_type_e _exp = (exp); \
    if(_got == _exp) break; \
    ORIG_GO((e), \
        E_VALUE, \
        "expected "#got".type == %x but got %x\n", \
        label, \
        FS(kvp_update_type_name(_exp)), \
        FS(kvp_update_type_name(_got)) \
    ); \
} while(0)

#define EXPECT_UPDATE_KEY_GO(e, got, exp, label) do { \
    kvp_update_t _got = (got); \
    dstr_t _dgot = dstr_from_cstrn(_got.key, _got.klen, false); \
    dstr_t _dexp = dstr_from_cstr(exp); \
    if(dstr_eq(_dgot, _dexp)) break; \
    EXPECT_D_GO((e), #got".key", _dgot, _dexp, label); \
} while(0)

#define EXPECT_UPDATE_VAL_GO(e, got, exp, label) do { \
    kvp_update_t _got = (got); \
    dstr_t _dgot = dstr_from_cstrn(_got.val, _got.vlen, false); \
    dstr_t _dexp = dstr_from_cstr(exp); \
    if(dstr_eq(_dgot, _dexp)) break; \
    EXPECT_D_GO((e), #got".val", _dgot, _dexp, label); \
} while(0)

static kvp_ack_t ack(kvp_update_t update){
    return (kvp_ack_t){
        .sync_id = update.sync_id,
        .update_id = update.update_id,
    };
}

#define EVENT_GO(e, call, label) do { \
    PROP_GO(e, call, label); \
    PROP_VAR_GO(e, &T.error, label); \
    if(T.call_idx != T.ncalls){ \
        ORIG_GO(e, \
            E_VALUE, \
            "expected %x call%x but got %x", \
            label, \
            FU(T.ncalls), \
            FS(T.ncalls == 1 ? "" : "s"), \
            FU(T.call_idx) \
        ); \
    }; \
    T.ncalls = 0; \
    T.call_idx = 0; \
} while(0)

#define SEND_CB_GO(e, _sender, now, label) \
    EVENT_GO((e), sender_send_cb(&T.k, &T.k.senders[_sender], now), label)

#define ACK_GO(e, _sender, _pkt, now, label) \
    EVENT_GO((e), \
        sender_recv_cb(&T.k, &T.k.senders[_sender], ack(_pkt), now), label \
    )

#define EXPECT_SCAN_TIMER_START(_deadline) \
    expect_call(((call_t){ \
        .type = SCAN_TIMER_START, .deadline = _deadline \
    }))

#define EXPECT_TIMEOUT_TIMER_START(_deadline) \
    expect_call(((call_t){ \
        .type = TIMEOUT_TIMER_START, .deadline = _deadline \
    }))

#define EXPECT_TIMEOUT_TIMER_STOP() \
    expect_call(((call_t){ .type = TIMEOUT_TIMER_STOP }))

#define EXPECT_SENDER_TIMER_START(_sender, _deadline) \
    expect_call(((call_t){ \
        .type = SENDER_TIMER_START, .sender = _sender, .deadline = _deadline \
    }))

#define EXPECT_SENDER_TIMER_STOP(_sender) \
    expect_call(((call_t){ .type = SENDER_TIMER_STOP, .sender = _sender }))

#define EXPECT_SENDER_SEND_PKT(_sender, _pkt) \
    expect_call(((call_t){ \
        .type = SENDER_SEND_PKT, .sender = _sender, .pkt = _pkt \
    }))

#define EXPECT_SUB_CLOSE(_sub) \
    expect_call(((call_t){ .type = SUBSCRIBER_CLOSE, .sub = (_sub) }))

#define EXPECT_SUB_RESPOND(_sub, _resp) \
    expect_call(((call_t){ \
        .type = SUBSCRIBER_RESPOND, .sub = (_sub), .msg = _resp \
    }))

#define EXPECT_INSERT(_pkt, _key, _val) do { \
    EXPECT_UPDATE_TYPE_GO(&e, _pkt, KVP_UPDATE_INSERT, cu); \
    EXPECT_UPDATE_KEY_GO(&e, _pkt, _key, cu); \
    EXPECT_UPDATE_VAL_GO(&e, _pkt, _val, cu); \
} while(0)

#define EXPECT_DELETE(_pkt, _key) do { \
    EXPECT_UPDATE_TYPE_GO(&e, _pkt, KVP_UPDATE_DELETE, cu); \
    EXPECT_UPDATE_KEY_GO(&e, _pkt, _key, cu); \
} while(0)

#define EXPECT_OK_EXPIRY(_pkt, _time) \
    EXPECT_U_GO(&e, "ok_expiry", (_pkt).ok_expiry, (_time), cu)

#define EXPECT_FLUSH(_pkt) \
    EXPECT_UPDATE_TYPE_GO(&e, _pkt, KVP_UPDATE_FLUSH, cu)

#define ADD_SUB(_sub, id, ch, now) do { \
    test_subscriber_t *tsub = (_sub); \
    dstr_t _id = dstr_from_cstr(id); \
    dstr_t _ch = dstr_from_cstr(ch); \
    EVENT_GO(&e, subscriber_read_cb(&T.k, &tsub->sub, _id, _ch, now), cu); \
} while(0)

static derr_t test_kvpsend(void){
    derr_t e = E_OK;

    kvpsend_test_t T;
    kvpsend_test_prep(&T);

    // initial users
    test_challenge_t u0 = test_challenge("id-0", "sd-0", "ch-0");
    test_challenge_t u1 = test_challenge("id-1", "sd-1", "ch-1");
    test_challenge_t u2 = test_challenge("id-2", "sd-2", "");
    challenge_insert(&T, &u0);
    challenge_insert(&T, &u1);
    challenge_insert(&T, &u2);

    // fake peers
    struct sockaddr_storage peers[2];
    size_t npeers = sizeof(peers)/sizeof(*peers);
    PROP(&e, read_addr(&peers[0], "7.7.7.0", 7) );
    PROP(&e, read_addr(&peers[1], "7.7.7.1", 7) );

    xtime_t now = 1;
    PROP_GO(&e, kvpsend_init(&T.k, &T.iface, now, peers, npeers), cu);

    kvp_update_t p0_start, p1_start;
    EXPECT_SENDER_SEND_PKT(0, &p0_start);
    EXPECT_SENDER_SEND_PKT(1, &p1_start);
    EXPECT_SCAN_TIMER_START(now + 15 * SECOND);
    EVENT_GO(&e, initial_actions(&T.k, now), cu);

    EXPECT_UPDATE_TYPE_GO(&e, p0_start, KVP_UPDATE_START, cu);
    EXPECT_UPDATE_TYPE_GO(&e, p1_start, KVP_UPDATE_START, cu);
    EXPECT_OK_EXPIRY(p0_start, 0);
    EXPECT_OK_EXPIRY(p1_start, 0);

    // wrote packets, expect wait
    EXPECT_SENDER_TIMER_START(0, now + 1 * SECOND);
    SEND_CB_GO(&e, 0, now, cu);
    EXPECT_SENDER_TIMER_START(1, now + 1 * SECOND);
    SEND_CB_GO(&e, 1, now, cu);
    now++;

    // introduce the first subscriber
    // [S] sub with noop change
    test_subscriber_t sub0 = {"sub0"};
    EXPECT_TIMEOUT_TIMER_START(now + 60 * SECOND);
    ADD_SUB(&sub0, "id-0", "ch-0", now);

    test_subscriber_t sub_bogus = {"sub_bogus"};
    // [H] sub for bad uuid gets closed
    EXPECT_SUB_CLOSE(&sub_bogus);
    ADD_SUB(&sub_bogus, "wrong", "ch-0", now);
    // [I] sub for bad challenge gets closed
    EXPECT_SUB_CLOSE(&sub_bogus);
    ADD_SUB(&sub_bogus, "id-0", "wrong", now);

    // ack pkt
    kvp_update_t p0_ins1, p1_ins1;
    EXPECT_SENDER_TIMER_STOP(0);
    EXPECT_SENDER_SEND_PKT(0, &p0_ins1);
    ACK_GO(&e, 0, p0_start, now, cu);

    EXPECT_SENDER_TIMER_STOP(1);
    EXPECT_SENDER_SEND_PKT(1, &p1_ins1);
    ACK_GO(&e, 1, p1_start, now, cu);

    EXPECT_INSERT(p0_ins1, "sd-0", "ch-0");
    EXPECT_INSERT(p1_ins1, "sd-0", "ch-0");
    EXPECT_OK_EXPIRY(p0_ins1, 0);
    EXPECT_OK_EXPIRY(p1_ins1, 0);

    // packets done writing, expect new ones
    kvp_update_t p0_ins2, p1_ins2;
    EXPECT_SENDER_SEND_PKT(0, &p0_ins2);
    SEND_CB_GO(&e, 0, now, cu);

    EXPECT_SENDER_SEND_PKT(1, &p1_ins2);
    SEND_CB_GO(&e, 1, now, cu);

    EXPECT_INSERT(p0_ins2, "sd-1", "ch-1");
    EXPECT_INSERT(p1_ins2, "sd-1", "ch-1");
    EXPECT_OK_EXPIRY(p0_ins2, 0);
    EXPECT_OK_EXPIRY(p1_ins2, 0);
    xtime_t p1_ins2_send_time = now;

    // wrote packets, expect wait
    EXPECT_SENDER_TIMER_START(0, now + 1 * SECOND);
    SEND_CB_GO(&e, 0, now, cu);

    EXPECT_SENDER_TIMER_START(1, now + 1 * SECOND);
    SEND_CB_GO(&e, 1, now, cu);
    now++;

    // acks
    ACK_GO(&e, 0, p0_ins1, now, cu);
    ACK_GO(&e, 1, p1_ins1, now, cu);
    now++;

    // [A] the first recv becomes ok -> pending entries transition
    kvp_update_t p0_flush;
    EXPECT_SUB_RESPOND(&sub0, "k");
    EXPECT_TIMEOUT_TIMER_STOP();
    EXPECT_SENDER_TIMER_STOP(0);
    EXPECT_SENDER_SEND_PKT(0, &p0_flush);
    ACK_GO(&e, 0, p0_ins2, now, cu);
    EXPECT_UPDATE_TYPE_GO(&e, p0_flush, KVP_UPDATE_FLUSH, cu);
    EXPECT_OK_EXPIRY(p0_flush, now + 15 * SECOND);
    xtime_t p0_flush_send_time = now;

    EXPECT_SENDER_TIMER_START(0, now + 1 * SECOND);
    SEND_CB_GO(&e, 0, now, cu);
    now++;

    EXPECT_SENDER_TIMER_STOP(0);
    EXPECT_SENDER_TIMER_START(0, p0_flush_send_time + 12 * SECOND);
    ACK_GO(&e, 0, p0_flush, now, cu);
    now++;

    // add sub0 for a new challenge
    // [P] sub to add entry
    kvp_update_t p0_ins3, p1_ins3;
    test_challenge_t u3 = test_challenge("id-3", "sd-3", "ch-3");
    challenge_insert(&T, &u3);
    EXPECT_SENDER_TIMER_STOP(0);
    EXPECT_SENDER_SEND_PKT(0, &p0_ins3);
    EXPECT_SENDER_TIMER_STOP(1);
    EXPECT_SENDER_SEND_PKT(1, &p1_ins3);
    EXPECT_TIMEOUT_TIMER_START(now + 60 * SECOND);
    ADD_SUB(&sub0, "id-3", "ch-3", now);
    EXPECT_INSERT(p0_ins3, "sd-3", "ch-3");
    EXPECT_INSERT(p1_ins3, "sd-3", "ch-3");
    xtime_t sender0_not_ok = now + 15 * SECOND;
    xtime_t p1_ins3_send_time = now;

    EXPECT_SENDER_TIMER_START(0, now + 1 * SECOND);
    SEND_CB_GO(&e, 0, now, cu);
    EXPECT_SENDER_TIMER_START(1, p1_ins2_send_time + 1 * SECOND);
    SEND_CB_GO(&e, 1, now, cu);
    now++;

    // [B] the second recv becomes ok -> pending entries remain
    EXPECT_SENDER_TIMER_STOP(1);
    EXPECT_SENDER_TIMER_START(1, p1_ins3_send_time + 1 * SECOND);
    ACK_GO(&e, 1, p1_ins2, now, cu);

    now += 4 * SECOND;
    kvp_update_t p1_flush;
    EXPECT_SENDER_TIMER_STOP(1);
    EXPECT_SENDER_SEND_PKT(1, &p1_flush);
    ACK_GO(&e, 1, p1_ins3, now, cu);
    EXPECT_FLUSH(p1_flush);

    // [C] the first recv becomes not-ok -> pending entries transition
    EXPECT_SENDER_TIMER_START(1, now + 1 * SECOND);
    SEND_CB_GO(&e, 1, now, cu);
    now++;
    EXPECT_SENDER_TIMER_STOP(1);
    EXPECT_SENDER_TIMER_START(1, now-1 + 12 * SECOND);
    ACK_GO(&e, 1, p1_flush, now, cu);

    deadline_t *deadline;
    kvp_update_t p_resend;
    while((deadline = deadline_pop(&T))){
        now = deadline->deadline;
        switch(deadline->type){
            case SENDER_TIMER:
                ptrdiff_t i = deadline - T.sender_timers;
                EXPECT_I_GO(&e, "sender idx", i, 0, cu);
                // is this what we are waiting for?
                if(deadline->deadline >= sender0_not_ok) goto found_deadline;
                sender_t *sender = &T.k.senders[0];
                EXPECT_SENDER_SEND_PKT(0, &p_resend);
                EVENT_GO(&e, sender_timer_cb(&T.k, sender, now), cu);
                EXPECT_SENDER_TIMER_START(0, now + 1 * SECOND);
                SEND_CB_GO(&e, 0, now, cu);
                EXPECT_INSERT(p_resend, "sd-3", "ch-3");
                EXPECT_OK_EXPIRY(p_resend, sender0_not_ok);
                break;
            case SCAN_TIMER:
                // [O] fullscan to noop
                EXPECT_SCAN_TIMER_START(now + 15 * SECOND);
                EVENT_GO(&e, scan_timer_cb(&T.k, now), cu);
                break;
            case TIMEOUT_TIMER:
                ORIG_GO(&e, E_VALUE, "got timeout timer too early\n", cu);
                break;
            default:
                ORIG_GO(&e, E_VALUE, "got bad deadline type\n", cu);
        }
    }
    ORIG_GO(&e, E_VALUE, "ran out of deadlines\n", cu);

found_deadline:
    now += 17;
    EXPECT_SUB_RESPOND(&sub0, "k");
    EXPECT_TIMEOUT_TIMER_STOP();
    EXPECT_SENDER_SEND_PKT(0, &p_resend);
    EVENT_GO(&e, sender_timer_cb(&T.k, &T.k.senders[0], now), cu);

    now++;

    // [E] sub appears, waits for ok, gets response
    test_challenge_t u4 = test_challenge("id-4", "sd-4", "ch-4");
    challenge_insert(&T, &u4);
    kvp_update_t p1_ins4;
    EXPECT_SENDER_TIMER_STOP(1);
    EXPECT_SENDER_SEND_PKT(1, &p1_ins4);
    EXPECT_TIMEOUT_TIMER_START(now + 60 * SECOND);
    ADD_SUB(&sub0, "id-4", "ch-4", now);
    EXPECT_INSERT(p1_ins4, "sd-4", "ch-4");
    EXPECT_OK_EXPIRY(p1_ins4, now + 15 * SECOND);
    xtime_t p1_ins4_send_time = now;

    EXPECT_SENDER_TIMER_START(1, now + 1 * SECOND);
    SEND_CB_GO(&e, 1, now, cu);
    now++;

    EXPECT_SUB_RESPOND(&sub0, "k");
    EXPECT_TIMEOUT_TIMER_STOP();
    EXPECT_SENDER_TIMER_STOP(1);
    EXPECT_SENDER_TIMER_START(1, p1_ins4_send_time + 12 * SECOND);
    ACK_GO(&e, 1, p1_ins4, now, cu);
    now++;

    // [F] sub appears, gets immediate ok
    EXPECT_SUB_RESPOND(&sub0, "k");
    ADD_SUB(&sub0, "id-4", "ch-4", now);

    // [Q] sub to delete entry
    kvp_update_t p1_del4;
    challenge_delete(&T, &u4);
    test_challenge_t u4_empty = test_challenge("id-4", "sd-4", "");
    challenge_insert(&T, &u4_empty);
    EXPECT_SENDER_TIMER_STOP(1);
    EXPECT_SENDER_SEND_PKT(1, &p1_del4);
    EXPECT_SUB_CLOSE(&sub_bogus);
    ADD_SUB(&sub_bogus, "id-4", "ch-4", now);
    EXPECT_DELETE(p1_del4, "sd-4");
    xtime_t p1_del4_send_time = now;

    EXPECT_SENDER_TIMER_START(1, now + 1 * SECOND);
    SEND_CB_GO(&e, 1, now, cu);
    now++;

    EXPECT_SENDER_TIMER_STOP(1);
    EXPECT_SENDER_TIMER_START(1, p1_del4_send_time + 12 * SECOND);
    ACK_GO(&e, 1, p1_del4, now, cu);
    now++;

    // [J] sub for changed challenge gets closed
    // [R] sub to change entry
    challenge_delete(&T, &u4_empty);
    test_challenge_t u4_b = test_challenge("id-4", "sd-4", "ch-4-b");
    challenge_insert(&T, &u4_b);
    test_subscriber_t sub_changed = {"sub_changed"};
    kvp_update_t p1_ins4b;
    EXPECT_SENDER_TIMER_STOP(1);
    EXPECT_SENDER_SEND_PKT(1, &p1_ins4b);
    EXPECT_TIMEOUT_TIMER_START(now + 60 * SECOND);
    ADD_SUB(&sub_changed, "id-4", "ch-4-b", now);
    EXPECT_INSERT(p1_ins4b, "sd-4", "ch-4-b");
    xtime_t p1_ins4b_send_time = now;

    EXPECT_SENDER_TIMER_START(1, now + 1 * SECOND);
    SEND_CB_GO(&e, 1, now, cu);
    now++;

    challenge_delete(&T, &u4_b);
    challenge_insert(&T, &u4);
    EXPECT_SUB_CLOSE(&sub_changed);
    EXPECT_TIMEOUT_TIMER_STOP();
    EXPECT_SENDER_TIMER_STOP(1);
    EXPECT_SENDER_SEND_PKT(1, &p1_del4);
    EXPECT_TIMEOUT_TIMER_START(now + 60 * SECOND);
    ADD_SUB(&sub0, "id-4", "ch-4", now);
    EXPECT_DELETE(p1_del4, "sd-4");
    p1_del4_send_time = now;
    now++;

    EXPECT_SENDER_SEND_PKT(1, &p1_ins4);
    SEND_CB_GO(&e, 1, now, cu);
    p1_ins4_send_time = now;
    EXPECT_INSERT(p1_ins4, "sd-4", "ch-4");
    now++;

    EXPECT_SENDER_TIMER_START(1, p1_ins4b_send_time + 1 * SECOND);
    SEND_CB_GO(&e, 1, now, cu);
    now++;

    EXPECT_SENDER_TIMER_STOP(1);
    EXPECT_SENDER_TIMER_START(1, p1_del4_send_time + 1 * SECOND);
    ACK_GO(&e, 1, p1_ins4b, now, cu);
    now++;

    EXPECT_SENDER_TIMER_STOP(1);
    EXPECT_SENDER_TIMER_START(1, p1_ins4_send_time + 1 * SECOND);
    ACK_GO(&e, 1, p1_del4, now, cu);
    now++;

    EXPECT_SUB_RESPOND(&sub0, "k");
    EXPECT_TIMEOUT_TIMER_STOP();
    EXPECT_SENDER_TIMER_STOP(1);
    EXPECT_SENDER_TIMER_START(1, p1_ins4b_send_time + 12 * SECOND);
    ACK_GO(&e, 1, p1_ins4, now, cu);
    now++;

    // [K] sub for deleted entry gets closed
    challenge_delete(&T, &u4);
    challenge_insert(&T, &u4_empty);
    EXPECT_SENDER_TIMER_STOP(1);
    EXPECT_SENDER_SEND_PKT(1, &p1_del4);
    EXPECT_SUB_CLOSE(&sub_bogus);
    ADD_SUB(&sub_bogus, "id-4", "wrong", now);
    EXPECT_DELETE(p1_del4, "sd-4");
    p1_del4_send_time = now;

    EXPECT_SENDER_TIMER_START(1, now + 1 * SECOND);
    SEND_CB_GO(&e, 1, now, cu);
    now++;

    challenge_delete(&T, &u4_empty);
    challenge_insert(&T, &u4);
    EXPECT_SENDER_TIMER_STOP(1);
    EXPECT_SENDER_SEND_PKT(1, &p1_ins4);
    EXPECT_TIMEOUT_TIMER_START(now + 60 * SECOND);
    ADD_SUB(&sub0, "id-4", "ch-4", now);
    p1_ins4_send_time = now;

    EXPECT_SENDER_TIMER_START(1, p1_del4_send_time + 1 * SECOND);
    SEND_CB_GO(&e, 1, now, cu);
    now++;

    EXPECT_SENDER_TIMER_STOP(1);
    EXPECT_SENDER_TIMER_START(1, p1_ins4_send_time + 1 * SECOND);
    ACK_GO(&e, 1, p1_del4, now, cu);
    now++;

    challenge_delete(&T, &u4);
    challenge_insert(&T, &u4_empty);
    EXPECT_SUB_CLOSE(&sub0);
    EXPECT_TIMEOUT_TIMER_STOP();
    EXPECT_SENDER_TIMER_STOP(1);
    EXPECT_SENDER_SEND_PKT(1, &p1_del4);
    EXPECT_SUB_CLOSE(&sub_bogus);
    ADD_SUB(&sub_bogus, "id-4", "wrong", now);
    p1_del4_send_time = now;

    EXPECT_SENDER_TIMER_START(1, p1_ins4_send_time + 1 * SECOND);
    SEND_CB_GO(&e, 1, now, cu);
    now++;

    EXPECT_SENDER_TIMER_STOP(1);
    EXPECT_SENDER_TIMER_START(1, p1_del4_send_time + 1 * SECOND);
    ACK_GO(&e, 1, p1_ins4, now, cu);
    now++;

    EXPECT_SENDER_TIMER_STOP(1);
    EXPECT_SENDER_TIMER_START(1, p1_ins4_send_time + 12 * SECOND);
    ACK_GO(&e, 1, p1_del4, now, cu);
    now++;

    // [G] sub appears, waits and gets timeout
    // [D] the second recv becomes not-ok -> pending entries remain
    challenge_delete(&T, &u4_empty);
    challenge_insert(&T, &u4);
    EXPECT_SENDER_TIMER_STOP(1);
    EXPECT_SENDER_SEND_PKT(1, &p1_ins4);
    EXPECT_TIMEOUT_TIMER_START(now + 60 * SECOND);
    ADD_SUB(&sub0, "id-4", "ch-4", now);
    p1_ins4_send_time = now;
    xtime_t timeout_time = now + 60 * SECOND;
    // timeout should be well after the ok_expiry time
    EXPECT_U_GT_GO(&e, "timeout", timeout_time, p1_ins4.ok_expiry, cu);

    EXPECT_SENDER_TIMER_START(1, now + 1 * SECOND);
    SEND_CB_GO(&e, 1, now, cu);
    now++;

    while((deadline = deadline_pop(&T))){
        now = deadline->deadline;
        switch(deadline->type){
            case SENDER_TIMER:
                ptrdiff_t ptr_i = deadline - T.sender_timers;
                int i = (int)ptr_i;
                sender_t *sender = &T.k.senders[i];
                EXPECT_SENDER_SEND_PKT(i, &p_resend);
                EVENT_GO(&e, sender_timer_cb(&T.k, sender, now), cu);
                EXPECT_SENDER_TIMER_START(i, now + 1 * SECOND);
                SEND_CB_GO(&e, i, now, cu);
                EXPECT_INSERT(p_resend, "sd-4", "ch-4");
                break;
            case SCAN_TIMER:
                EXPECT_SCAN_TIMER_START(now + 15 * SECOND);
                EVENT_GO(&e, scan_timer_cb(&T.k, now), cu);
                break;
            case TIMEOUT_TIMER:
                EXPECT_U_GO(&e, "now", now, timeout_time, cu);
                goto found_timeout;
            default:
                ORIG_GO(&e, E_VALUE, "got bad deadline type\n", cu);
        }
    }
    ORIG_GO(&e, E_VALUE, "ran out of deadlines\n", cu);

found_timeout:
    now += 37;
    EXPECT_SUB_RESPOND(&sub0, "t");
    EVENT_GO(&e, timeout_timer_cb(&T.k, now), cu);
    now++;

    // get the recv back in a working state
    kvp_update_t p1_empty;
    EXPECT_SENDER_TIMER_STOP(1);
    EXPECT_SENDER_SEND_PKT(1, &p1_empty);
    ACK_GO(&e, 1, p1_ins4, now, cu);

    EXPECT_SENDER_TIMER_START(1, now + 1 * SECOND);
    SEND_CB_GO(&e, 1, now, cu);
    EXPECT_UPDATE_TYPE_GO(&e, p1_empty, KVP_UPDATE_EMPTY, cu);
    EXPECT_OK_EXPIRY(p1_empty, now + 15 * SECOND);
    now++;

    EXPECT_SENDER_TIMER_STOP(1);
    EXPECT_SENDER_TIMER_START(1, now-1 + 12 * SECOND);
    ACK_GO(&e, 1, p1_empty, now, cu);
    now++;

    // [L] fullscan to add entry
    // [M] fullscan to delete entry
    // [N] fullscan to change entry
    bool deleted_entry = false;
    bool added_entry = false;
    bool changed_entry = false;
    test_challenge_t u1b = test_challenge("id-1b", "sd-1b", "ch-1b");;
    while((deadline = deadline_pop(&T))){
        now = deadline->deadline;
        switch(deadline->type){
            case SENDER_TIMER:
                ptrdiff_t ptr_i = deadline - T.sender_timers;
                int i = (int)ptr_i;
                sender_t *sender = &T.k.senders[i];
                EXPECT_SENDER_SEND_PKT(i, &p_resend);
                EVENT_GO(&e, sender_timer_cb(&T.k, sender, now), cu);
                EXPECT_SENDER_TIMER_START(i, now + 1 * SECOND);
                SEND_CB_GO(&e, i, now, cu);
                if(i == 1){
                    // keep responding to keepalives
                    now++;
                    EXPECT_UPDATE_TYPE_GO(&e, p1_empty, KVP_UPDATE_EMPTY, cu);
                    EXPECT_SENDER_TIMER_STOP(1);
                    EXPECT_SENDER_TIMER_START(1, now-1 + 12 * SECOND);
                    ACK_GO(&e, 1, p_resend, now, cu);
                }
                break;
            case SCAN_TIMER:
                if(!deleted_entry){
                    deleted_entry = true;
                    challenge_delete(&T, &u4);
                    challenge_insert(&T, &u4_empty);
                    // get the delete packet
                    EXPECT_SENDER_TIMER_STOP(1);
                    EXPECT_SENDER_SEND_PKT(1, &p1_del4);
                    EXPECT_SCAN_TIMER_START(now + 15 * SECOND);
                    EVENT_GO(&e, scan_timer_cb(&T.k, now), cu);
                    EXPECT_DELETE(p1_del4, "sd-4");
                    // send and ack the packet
                    EXPECT_SENDER_TIMER_START(1, now + 1 * SECOND);
                    SEND_CB_GO(&e, 1, now, cu);
                    now++;
                    EXPECT_SENDER_TIMER_STOP(1);
                    EXPECT_SENDER_TIMER_START(1, now-1 + 12 * SECOND);
                    ACK_GO(&e, 1, p1_del4, now, cu);
                }else if(!added_entry){
                    added_entry = true;
                    challenge_delete(&T, &u4_empty);
                    challenge_insert(&T, &u4);
                    // get the delete packet
                    EXPECT_SENDER_TIMER_STOP(1);
                    EXPECT_SENDER_SEND_PKT(1, &p1_ins4);
                    EXPECT_SCAN_TIMER_START(now + 15 * SECOND);
                    EVENT_GO(&e, scan_timer_cb(&T.k, now), cu);
                    EXPECT_INSERT(p1_ins4, "sd-4", "ch-4");
                    // send and ack the packet
                    EXPECT_SENDER_TIMER_START(1, now + 1 * SECOND);
                    SEND_CB_GO(&e, 1, now, cu);
                    now++;
                    EXPECT_SENDER_TIMER_STOP(1);
                    EXPECT_SENDER_TIMER_START(1, now-1 + 12 * SECOND);
                    ACK_GO(&e, 1, p1_ins4, now, cu);
                }else if(!changed_entry){
                    changed_entry = true;
                    challenge_delete(&T, &u4);
                    challenge_insert(&T, &u4_b);
                    // get the delete packet
                    EXPECT_SENDER_TIMER_STOP(1);
                    EXPECT_SENDER_SEND_PKT(1, &p1_del4);
                    EXPECT_SCAN_TIMER_START(now + 15 * SECOND);
                    EVENT_GO(&e, scan_timer_cb(&T.k, now), cu);
                    EXPECT_DELETE(p1_del4, "sd-4");
                    // send the delete packet and get the insert packet
                    EXPECT_SENDER_SEND_PKT(1, &p1_ins4);
                    SEND_CB_GO(&e, 1, now, cu);
                    EXPECT_INSERT(p1_ins4, "sd-4", "ch-4-b");
                    EXPECT_SENDER_TIMER_START(1, now + 1 * SECOND);
                    SEND_CB_GO(&e, 1, now, cu);
                    now++;

                    ACK_GO(&e, 1, p1_del4, now, cu);
                    now++;

                    EXPECT_SENDER_TIMER_STOP(1);
                    EXPECT_SENDER_TIMER_START(1, now-2 + 12 * SECOND);
                    ACK_GO(&e, 1, p1_ins4, now, cu);
                }else{
                    // extra cases to hit all code paths
                    challenge_insert(&T, &u1b);
                    challenge_delete(&T, &u3);
                    // get the insert packet
                    EXPECT_SENDER_TIMER_STOP(1);
                    kvp_update_t p1_del3;
                    EXPECT_SENDER_SEND_PKT(1, &p1_del3);
                    EXPECT_SCAN_TIMER_START(now + 15 * SECOND);
                    EVENT_GO(&e, scan_timer_cb(&T.k, now), cu);
                    EXPECT_DELETE(p1_del3, "sd-3");
                    EXPECT_SENDER_SEND_PKT(1, &p1_ins1);
                    SEND_CB_GO(&e, 1, now, cu);
                    EXPECT_INSERT(p1_ins1, "sd-1b", "ch-1b");

                    // done!
                    goto cu;
                }
                break;
            case TIMEOUT_TIMER:
                ORIG_GO(&e, E_VALUE, "unexpected timeout deadline", cu);
            default:
                ORIG_GO(&e, E_VALUE, "got bad deadline type\n", cu);
        }
    }
    ORIG_GO(&e, E_VALUE, "ran out of deadlines\n", cu);

    /* cases to test:
     x [A] the first recv becomes ok -> pending entries transition
     x [B] the second recv becomes ok -> pending entries remain
     x [C] the first recv becomes not-ok -> pending entries transition
     x [D] the second recv becomes not-ok -> pending entries remain
     x [E] sub appears, waits for ok, gets response
     x [F] sub appears, gets immediate ok
     x [G] sub appears, waits and gets timeout
     x [H] sub for bad uuid gets closed
     x [I] sub for bad challenge gets closed
     x [J] sub for changed challenge gets closed
     x [K] sub for deleted entry gets closed
     x [L] fullscan to add entry
     x [M] fullscan to delete entry
     x [N] fullscan to change entry
     x [O] fullscan to noop
     x [P] sub to add entry
     x [Q] sub to delete entry
     x [R] sub to change entry
     x [S] sub with noop change
    */

cu:
    // ignore any further errors
    T.failing = true;
    kvpsend_free(&T.k);

    return e;
}

static derr_t test_free(void){
    derr_t e = E_OK;

    MYSQL sql = {0};
    kvpsend_t k = {0};

    mysql_close(&sql);
    kvpsend_free(&k);

    return e;
}


int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_kvpsend(), test_fail);
    PROP_GO(&e, test_free(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
