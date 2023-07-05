#include "libdstr/libdstr.h"
#include "libweb/libweb.h"
#include "libduv/libduv.h"
#include "server/libsmsql.h"
#include "server/libkvpsync/libkvpsync.h"

/* protocol for IPC communication is basically just:
    - connector sends uuid:challenge\n
    - kvpsyncsend responds with "k" for "ok" when the dns entry is ready,
      or "t" for timeout after one minute and closes the connection. */

// kvpsend_i is the interface for testing
struct kvpsend_i;
typedef struct kvpsend_i kvpsend_i;
struct kvpsend_t;
typedef struct kvpsend_t kvpsend_t;

typedef struct {
    kvpsend_t *k;
    kvpsync_send_t send;
    const struct sockaddr *addr;
    // char wbuf[4096];
    bool inwrite;
    // uv_udp_send_t req;
    // uv_timer_t timer;
    // the current deadline we're waiting for, or XTIME_MAX
    xtime_t deadline;
} sender_t;
DEF_CONTAINER_OF(sender_t, send, kvpsync_send_t)

#define MAX_PEERS 8

#define FULL_SCAN_PERIOD (15 * SECOND)

struct kvpsend_t {
    kvpsend_i *I;
    //uv_loop_t loop;
    //uv_pipe_t listener;
    jsw_atree_t sorted;  // entry_t->node
    sender_t senders[MAX_PEERS];
    size_t nsenders;
    //uv_udp_t sync_udp;
    // // we only need one recv buf for all udps
    // char recvbuf[2048];
    // derr_t close_reason;
    // bool closing;
    //uv_timer_t scan_timer;
    link_t allsubs; // subscriber_t->tlink
    //uv_timer_t timeout_timer;
    xtime_t next_timeout;

    #if MAX_PEERS > 32
    #error "too many peers to fit into global_t->okmask"
    #endif
    unsigned int okmask;
};

derr_t sender_init(
    kvpsend_t *k, sender_t *sender, const struct sockaddr *addr, xtime_t now
);
void sender_free(sender_t *sender);

derr_t kvpsend_init(
    kvpsend_t *k,
    kvpsend_i *I,
    xtime_t now,
    struct sockaddr_storage *peers,
    size_t npeers
);
void kvpsend_free(kvpsend_t *k);

#define SUBSCRIBER_TIMEOUT (60 * SECOND)

typedef struct {
    size_t deadline;
    // uv_pipe_t pipe;
    // // expect to read: hex(inst_uuid) ':' + challenge + '\n'
    // char rbuf[SMSQL_UUID_SIZE*2 + 1 + SMSQL_CHALLENGE_SIZE + 1];
    // size_t rlen;
    // uv_write_t req;
    link_t link;  // entry_t->subscribers
    link_t tlink;  // kvpsend_t->allsubs
} subscriber_t;
DEF_CONTAINER_OF(subscriber_t, link, link_t)
DEF_CONTAINER_OF(subscriber_t, tlink, link_t)

typedef struct {
    jsw_anode_t node;  // kvpsend_t->order
    dstr_t subdomain;
    char subdomainbuf[SMSQL_SUBDOMAIN_SIZE];
    dstr_t challenge;
    char challengebuf[SMSQL_CHALLENGE_SIZE];
    bool ready;
    link_t subscribers;  // subscriber_t->link
    link_t link;  // full_scan()::new

    #if MAX_PEERS > 32
    #error "too many peers to fit into entry_t->readymask"
    #endif
    unsigned int readymask;
} entry_t;
DEF_CONTAINER_OF(entry_t, node, jsw_anode_t)
DEF_CONTAINER_OF(entry_t, link, link_t)

entry_t *entry_first(jsw_atrav_t *trav, jsw_atree_t *tree);
entry_t *entry_next(jsw_atrav_t *trav);
entry_t *entry_pop_to_next(jsw_atrav_t *trav);
void delete_entry(kvpsend_t *k, entry_t *entry, bool in_sorted);

struct kvpsend_i {
    // event sinks
    void (*scan_timer_start)(kvpsend_i*, xtime_t deadline);
    void (*timeout_timer_start)(kvpsend_i*, xtime_t deadline);
    void (*timeout_timer_stop)(kvpsend_i*);
    void (*subscriber_close)(kvpsend_i*, subscriber_t *sub);
    void (*subscriber_respond)(kvpsend_i*, subscriber_t *sub, dstr_t msg);
    derr_t (*sender_send_pkt)(
        kvpsend_i*, sender_t *sender, const kvp_update_t *pkt
    );
    void (*sender_timer_start)(kvpsend_i*, sender_t *sender, xtime_t deadline);
    void (*sender_timer_stop)(kvpsend_i*, sender_t *sender);

    // database lookups
    derr_t (*challenges_first)(kvpsend_i*, challenge_iter_t *it);
    derr_t (*challenges_next)(kvpsend_i*, challenge_iter_t *it);
    void (*challenges_free)(kvpsend_i*, challenge_iter_t *it);
    derr_t (*get_installation_challenge)(
        kvpsend_i*,
        const dstr_t inst_uuid,
        dstr_t *subdomain,
        bool *subdomain_ok,
        dstr_t *challenge,
        bool *challenge_ok
    );
};

// event sources
derr_t initial_actions(kvpsend_t *k, xtime_t now);
derr_t scan_timer_cb(kvpsend_t *k, xtime_t now);
derr_t timeout_timer_cb(kvpsend_t *k, xtime_t now);
derr_t subscriber_read_cb(
    kvpsend_t *k,
    subscriber_t *sub,
    dstr_t inst_uuid,
    dstr_t req_challenge,
    xtime_t now
);
derr_t sender_timer_cb(kvpsend_t *k, sender_t *sender, xtime_t now);
derr_t sender_send_cb(kvpsend_t *k, sender_t *sender, xtime_t now);
derr_t sender_recv_cb(
    kvpsend_t *k, sender_t *sender, kvp_ack_t ack, xtime_t now
);
void healthcheck_read_cb(kvpsend_t *k, subscriber_t *sub);
