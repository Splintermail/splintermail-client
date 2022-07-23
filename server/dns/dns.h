#define NMEMBUFS 256
#define MEMBUFSIZE 4096

typedef struct {
    uint16_t id;
    bool qr : 1;
    uint8_t opcode : 4;
    bool aa : 1;
    bool tc : 1;
    bool rd : 1;
    bool ra : 1;
    uint8_t z : 3;
    uint8_t rcode : 4;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_hdr_t;

typedef struct {
    const char *ptr;
    size_t len;
    uint16_t qdcount;
    uint16_t qtype;
    uint16_t qclass;
} dns_qstn_t;

typedef struct {
} dns_ans_t;

typedef struct {
} dns_auth_t;

typedef struct {
} dns_addl_t;

typedef struct {
    dns_hdr_t hdr;
    dns_qstn_t qstn;
    dns_ans_t ans;
    dns_auth_t auth;
    dns_addl_t addl;
} dns_pkt_t;

typedef struct {
    uv_loop_t loop;
    uv_udp_t udp;
    link_t membufs;  // membuf_t->link
    bool closing;
    derr_t close_reason;
} globals_t;

typedef struct {
    char base[MEMBUFSIZE];
    size_t len;
    link_t link; // globals_t->membufs;
} membuf_t;
DEF_CONTAINER_OF(membuf_t, base, char);
DEF_CONTAINER_OF(membuf_t, link, link_t);
DEF_STEAL_PTR(membuf_t);

// main.c //

// only to be called from the top-level libuv callbacks
void dns_close(globals_t *g, derr_t e);

// membuf.c //

void membuf_free(membuf_t **ptr);
derr_t membuf_init(membuf_t **ptr);
void membufs_free(link_t *membufs);
derr_t membufs_init(link_t *membufs, size_t n);
void membuf_return(globals_t *g, membuf_t **ptr);

// parse.c //

// returns bool ok
bool parse_pkt(dns_pkt_t *pkt, const char *ptr, size_t len);
void print_pkt(const dns_pkt_t pkt);

// dns.c //

// owns membuf
derr_t handle_packet(globals_t *g, membuf_t *membuf);
