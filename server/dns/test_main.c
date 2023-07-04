#include "test/test_utils.h"

#define NPEERS 2
#define BASEPORT 9912

#define BUILD_TEST
#include "server/dns/main.c"

#define EXPECT_ADDRS_GO(e, name, got, exp, label) do { \
    if(addrs_eq(got, exp)) break; \
    ORIG_GO(e, \
        E_VALUE, \
        "expected %x == %x/%x but got %x/%x\n", \
        label, \
        FS(name), \
        FNTOPS(got), FU(must_addrs_port(got)), \
        FNTOPS(exp), FU(must_addrs_port(exp)) \
    ); \
} while(0)

#define EXPECT_EMPTY_GO(e, name, list, label) do { \
    if(link_list_isempty(list)) break; \
    ORIG_GO(e, \
        E_VALUE, \
        "expected %x to be empty but it is not\n", \
        label, \
        FS(name) \
    ); \
} while(0)

#define EXPECT_ACK_GO(e, buf, _sync_id, _update_id, label) do { \
    kvp_ack_t _ack; \
    bool ok = kvpsync_ack_read(buf, &_ack); \
    EXPECT_B_GO(e, "ok", ok, true, label); \
    EXPECT_U_GO(e, "sync_id", _ack.sync_id, _sync_id, cu); \
    EXPECT_U_GO(e, "update_id", _ack.update_id, _update_id, cu); \
} while(0)

#define EXPECT_SYNC_MSG_GO(e, _peer, _sync_id, _update_id, label) do { \
    sentmsg_t s; \
    PROP_GO(e, peek_sentmsg(&s), label); \
    EXPECT_P_GO(e, "udp", s.udp, &g->sync_udp, label); \
    EXPECT_ADDRS_GO(e, "dst", s.dst, &g->peers[_peer], label); \
    EXPECT_ACK_GO(e, s.buf, _sync_id, _update_id, label); \
    complete_sentmsg(&s); \
} while(0)

globals_t *g;
static link_t sends;  // membuf->link
bool recving = false;

static struct sockaddr_storage must_read_addr(const char *addr, uint16_t port){
    derr_t e = E_OK;

    struct sockaddr_storage out;
    PROP_GO(&e, read_addr(&out, addr, port), fail);
    return out;

fail:
    DROP_VAR(&e);
    LOG_FATAL("unable to read addr (%x)\n", FS(addr));
    return out;
}

// hook for testing
derr_t recv_start(uv_udp_t*, uv_alloc_cb, uv_udp_recv_cb){
    recving = true;
    return E_OK;
}

// hook for testing
int recv_stop(uv_udp_t*){
    recving = false;
    return 0;
}

// hook for testing
derr_t udp_send(
    uv_udp_send_t *req,
    uv_udp_t *udp,
    const uv_buf_t bufs[],
    unsigned int nbufs,
    const struct sockaddr *dst,
    uv_udp_send_cb cb
){
    derr_t e = E_OK;

    EXPECT_U(&e, "nbufs", nbufs, 1);

    membuf_t *membuf = CONTAINER_OF(req, membuf_t, req);
    req->handle = udp;
    // private fields, used for convenience since this is just a test
    req->cb = cb;
    req->bufsml[0].len = bufs[0].len;
    PROP(&e, addr_copy(dst, &req->addr) );

    link_list_append(&sends, &membuf->link);

    return e;
}

typedef struct {
    membuf_t *membuf;
    dstr_t buf;
    uv_udp_t *udp;
    const struct sockaddr_storage *dst;
} sentmsg_t;

static derr_t peek_sentmsg(sentmsg_t *s){
    derr_t e = E_OK;

    if(link_list_isempty(&sends)) ORIG(&e, E_VALUE, "no more sends");
    link_t *link = sends.next;

    s->membuf = CONTAINER_OF(link, membuf_t, link);
    DSTR_WRAP(s->buf, s->membuf->resp, s->membuf->req.bufsml[0].len, false);
    s->dst = &s->membuf->req.addr;
    s->udp = s->membuf->req.handle;

    return e;
}

static void complete_sentmsg(sentmsg_t *s){
    if(!s->membuf) return;
    link_remove(&s->membuf->link);
    s->membuf->req.cb(&s->membuf->req, 0);
    *s = (sentmsg_t){0};
}

static membuf_t *allocate_for_recv(uv_udp_t *udp){
    uv_buf_t buf;
    allocator(duv_udp_handle(udp), 0, &buf);
    if(!buf.base || !buf.len){
        return NULL;
    }
    return CONTAINER_OF(buf.base, membuf_t, base);
}

static size_t put_uint16(uint16_t n, char *out, size_t used){
    uint8_t *uptr = (uint8_t*)out;
    uptr[used+0] = (unsigned char)((n >> 8) & 0xff);
    uptr[used+1] = (unsigned char)((n >> 0) & 0xff);
    return used + 2;
}

static derr_t _send_dns_query(
    const struct sockaddr *src,
    uint16_t id,
    uint16_t qtype,
    const lstr_t *labels,
    size_t nlabels
){
    derr_t e = E_OK;

    membuf_t *allocated = NULL;

    allocated = allocate_for_recv(&g->dns_udp);
    EXPECT_NOT_NULL_GO(&e, "allocated", allocated, cu);
    dstr_t wbuf;
    DSTR_WRAP_ARRAY(wbuf, STEAL(membuf_t, &allocated)->base);
    dns_hdr_t hdr = { .id = id, .qdcount = 1 };
    wbuf.len = write_hdr(hdr, wbuf.data, wbuf.size, wbuf.len);
    wbuf.len = put_name(labels, nlabels, wbuf.data, wbuf.len);
    wbuf.len = put_uint16(qtype, wbuf.data, wbuf.len);
    wbuf.len = put_uint16(1, wbuf.data, wbuf.len);
    EXPECT_U_LE_GO(&e, "wbuf.len", wbuf.len, wbuf.size, cu);

    uv_buf_t buf = { .base = wbuf.data, .len = wbuf.size };
    on_recv(
        &g->dns_udp,
        (ssize_t)wbuf.len,
        &buf,
        src,
        0
    );
    EXPECT_B_GO(&e, "closing", g->closing, false, cu);

cu:
    if(allocated) membuf_return(&allocated);

    return e;

}
#define send_dns_query(src, id, qtype, ...) \
    _send_dns_query( \
        src, \
        id, \
        qtype, \
        (const lstr_t[]){__VA_ARGS__}, \
        sizeof((const lstr_t[]){__VA_ARGS__})/sizeof(lstr_t) \
    )

static derr_t expect_rr(
    const struct sockaddr_storage *src, uint16_t id, const dstr_t rdata_exp
){
    derr_t e = E_OK;

    sentmsg_t s;
    PROP_GO(&e, peek_sentmsg(&s), cu);

    EXPECT_P_GO(&e, "udp", s.udp, &g->dns_udp, cu);
    EXPECT_ADDRS_GO(&e, "dst", s.dst, src, cu);

    dns_pkt_t pkt;
    size_t zret = parse_pkt(&pkt, s.buf.data, s.buf.len);
    EXPECT_B_GO(&e, "is_bad_parse()", is_bad_parse(zret), false, cu);
    EXPECT_U_GO(&e, "id", pkt.hdr.id, id, cu);

    // expect exactly one answer
    rrs_t it;
    rr_t *rr = dns_rr_iter(&it, pkt.ans);
    EXPECT_NOT_NULL_GO(&e, "rr", rr, cu);
    rr_t ans = *rr;
    rr = rrs_next(&it);
    EXPECT_NULL_GO(&e, "rr", rr, cu);

    // expect the rdata that was provided
    dstr_t rdata;
    DSTR_WRAP(rdata, s.buf.data + ans.rdoff, ans.rdlen, false);
    EXPECT_D3_GO(&e, "rdata", rdata, rdata_exp, cu);

    complete_sentmsg(&s);

cu:
    return e;
}

static derr_t expect_srverr(const struct sockaddr_storage *src, uint16_t id){
    derr_t e = E_OK;

    sentmsg_t s;
    PROP_GO(&e, peek_sentmsg(&s), cu);

    EXPECT_P_GO(&e, "udp", s.udp, &g->dns_udp, cu);
    EXPECT_ADDRS_GO(&e, "dst", s.dst, src, cu);

    dns_pkt_t pkt;
    size_t zret = parse_pkt(&pkt, s.buf.data, s.buf.len);
    EXPECT_B_GO(&e, "is_bad_parse()", is_bad_parse(zret), false, cu);
    EXPECT_U_GO(&e, "id", pkt.hdr.id, id, cu);
    EXPECT_U_GO(&e, "rcode", pkt.hdr.rcode, RCODE_SRVERR, cu);

    complete_sentmsg(&s);

cu:
    return e;
}

static derr_t test_dns_responses(bool recv_ok){
    derr_t e = E_OK;

    #define ACME LSTR("_acme-challenge")
    #define X LSTR("x")
    #define USER LSTR("user")
    #define SPLINTERMAIL LSTR("splintermail")
    #define COM LSTR("com")

    #define A 1
    #define NS 2
    #define SOA 6
    #define TXT 16
    #define AAAA 28

    struct sockaddr_storage qaddr = must_read_addr("1.2.3.4", 53);
    PROP_GO(&e,
        send_dns_query(ss2sa(&qaddr), 11, A, X, USER, SPLINTERMAIL, COM),
    cu);
    // A-rdata for 127.0.0.1
    dstr_t rdata = DSTR_LIT("\x7f\x00\x00\x01");
    PROP_GO(&e, expect_rr(&qaddr, 11, rdata), cu);
    EXPECT_EMPTY_GO(&e, "sends", &sends, cu);

    // TXT request for known user
    PROP_GO(&e,
        send_dns_query(
            ss2sa(&qaddr), 12, TXT, ACME, X, USER, SPLINTERMAIL, COM
        ),
    cu);
    if(!recv_ok){
        // not synced; expect a srverr
        PROP_GO(&e, expect_srverr(&qaddr, 12) ,cu);
    }else{
        // synced, expect a valid result
        rdata = DSTR_LIT("\x04""abcd");
        PROP_GO(&e, expect_rr(&qaddr, 12, rdata), cu);
        EXPECT_EMPTY_GO(&e, "sends", &sends, cu);
    }

cu:
    return e;
}

static derr_t send_kvp_update(size_t peerid, kvp_update_t update){
    derr_t e = E_OK;

    membuf_t *allocated = NULL;

    // respond to resync request
    allocated = allocate_for_recv(&g->sync_udp);
    EXPECT_NOT_NULL_GO(&e, "allocated", allocated, cu);
    EXPECT_B_GO(&e, "closing", g->closing, false, cu);

    dstr_t wbuf;
    DSTR_WRAP_ARRAY(wbuf, STEAL(membuf_t, &allocated)->base);
    PROP_GO(&e, kvpsync_update_write(&update, &wbuf), cu);
    uv_buf_t buf = { .base = wbuf.data, .len = wbuf.size };
    on_recv(
        &g->sync_udp,
        (ssize_t)wbuf.len,
        &buf,
        (struct sockaddr*)&g->peers[peerid],
        0
    );
    EXPECT_B_GO(&e, "closing", g->closing, false, cu);

cu:
    if(allocated) membuf_return(&allocated);

    return e;
}

static derr_t test_sync_sequence(size_t i){
    derr_t e = E_OK;

    // respond to resync request
    PROP_GO(&e,
        send_kvp_update(1, (kvp_update_t){
            .ok_expiry = 0,
            .sync_id = 10,
            .update_id = 1,
            .type = KVP_UPDATE_START,
            .resync_id = g->recv[i].recv_id,
        }),
    cu);
    EXPECT_SYNC_MSG_GO(&e, i, 10, 1, cu);
    EXPECT_EMPTY_GO(&e, "sends", &sends, cu);

    // send an insert
    PROP_GO(&e,
        send_kvp_update(1, (kvp_update_t){
            .ok_expiry = 0,
            .sync_id = 10,
            .update_id = 2,
            .type = KVP_UPDATE_INSERT,
            .klen = 1,
            .key = "x",
            .vlen = 4,
            .val = "abcd",
        }),
    cu);
    EXPECT_SYNC_MSG_GO(&e, i, 10, 2, cu);
    EXPECT_EMPTY_GO(&e, "sends", &sends, cu);

    // send a flush
    PROP_GO(&e,
        send_kvp_update(1, (kvp_update_t){
            .ok_expiry = xtime() + 100000000000UL,
            .sync_id = 10,
            .update_id = 3,
            .type = KVP_UPDATE_FLUSH,
        }),
    cu);
    EXPECT_SYNC_MSG_GO(&e, i, 10, 3, cu);
    EXPECT_EMPTY_GO(&e, "sends", &sends, cu);

cu:
    return e;
}

static void complete_one_message(void){
    link_t *link = link_list_pop_first(&sends);
    if(link){
        membuf_t *membuf = CONTAINER_OF(link, membuf_t, link);
        membuf->req.cb(&membuf->req, 0);
    }
}

static void complete_all_messages(void){
    while(!link_list_isempty(&sends)){
        complete_one_message();
    }
}

static derr_t test_membuf_limit(void){
    derr_t e = E_OK;

    EXPECT_EMPTY_GO(&e, "sends", &sends, cu);

    link_t *last = &sends;

    for(size_t i = 0; i < NMEMBUFS; i++){
        // a dns query
        char buf[32];
        snprintf(buf, sizeof(buf), "%zu.%zu.%zu.%zu",
            (i >> 24) & 0xff,
            (i >> 16) & 0xff,
            (i >> 8) & 0xff,
            (i >> 0) & 0xff
        );
        struct sockaddr_storage qaddr = must_read_addr(buf, 53);
        uint16_t id = (uint16_t)(i & 0xffff);
        PROP_GO(&e,
            send_dns_query(ss2sa(&qaddr), id, A, X, USER, SPLINTERMAIL, COM),
        cu);
        if(sends.prev == last){
            ORIG_GO(&e, E_VALUE, "expected a new sent message\n", cu);
        }
        last = sends.prev;
        EXPECT_B_GO(&e, "recving", recving, i+1 < NMEMBUFS, cu);
    }

    // should start receiving messages after one message returns
    EXPECT_B_GO(&e, "recving", recving, false, cu);
    complete_one_message();
    EXPECT_B_GO(&e, "recving", recving, true, cu);

    // then stop again with one more pending
    struct sockaddr_storage qaddr = must_read_addr("1.2.3.4", 53);
    PROP_GO(&e,
        send_dns_query(ss2sa(&qaddr), NMEMBUFS, A, X, USER, SPLINTERMAIL, COM),
    cu);
    EXPECT_B_GO(&e, "recving", recving, false, cu);

cu:
    complete_all_messages();
    return e;
}

static derr_t runloop(uv_loop_t *loop){
    derr_t e = E_OK;

    sentmsg_t s;
    membuf_t *allocated = NULL;

    g = (globals_t*)loop->data;

    // run once to send initial resyncs
    PROP(&e, duv_run(loop) );
    if(is_error(g->close_reason)){
        return E_OK;
    }

    // check initial resync requests
    for(size_t i = 0; i < NPEERS; i++){
        PROP_GO(&e, peek_sentmsg(&s), cu);
        EXPECT_P_GO(&e, "udp", s.udp, &g->sync_udp, cu);
        EXPECT_ADDRS_GO(&e, "dst", s.dst, &g->peers[i], cu);
        EXPECT_ACK_GO(&e, s.buf, g->recv[i].recv_id, 0, cu);
        complete_sentmsg(&s);
    }
    EXPECT_EMPTY_GO(&e, "sends", &sends, cu);

    // send any type of response
    PROP_GO(&e, test_dns_responses(false), cu);

    // configure the last recv with one key value pair
    PROP_GO(&e, test_sync_sequence(NPEERS-1), cu);

    // send any type of response
    PROP_GO(&e, test_dns_responses(true), cu);

    PROP_GO(&e, test_membuf_limit(), cu);

cu:
    // release anything we allocated but did not use
    if(allocated) membuf_return(&allocated);
    // complete any unsent messages
    complete_all_messages();
    if(!g->closing) dns_close(g, E_OK);
    if(is_error(e)){
        DROP_CMD( duv_run(loop) );
    }else{
        PROP(&e, duv_run(loop) );
    }

    return e;
}

static derr_t test_main(void){
    derr_t e = E_OK;

    #define ADDRSPEC(s) must_parse_addrspec(&DSTR_LIT(s))
    addrspec_t dnsspec = ADDRSPEC("localhost:9910");
    addrspec_t syncspec = ADDRSPEC("localhost:9911");
    struct sockaddr_storage peers[NPEERS];
    for(uint16_t i = 0; i < NPEERS; i++){
        peers[i] = must_read_addr("127.0.0.1", BASEPORT + i);
    }

    int fd = -1;
    PROP(&e, dns_main(dnsspec, &fd, &fd, syncspec, &fd, peers, NPEERS, 997) );

    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_DEBUG);

    PROP_GO(&e, test_main(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
