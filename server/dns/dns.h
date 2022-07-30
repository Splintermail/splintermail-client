#define NMEMBUFS 256
#define MEMBUFSIZE 4096

#define BAD_PARSE ((size_t)-1)

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
    size_t off;
    size_t len;
    uint16_t qdcount;
    uint16_t qtype;
    uint16_t qclass;
} dns_qstn_t;

typedef struct {
    const char *ptr;
    size_t off;
    size_t len;
    uint16_t count;
} dns_rr_t;

typedef struct {
    dns_hdr_t hdr;
    dns_qstn_t qstn;
    dns_rr_t ans;
    dns_rr_t auth;
    dns_rr_t addl;
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

void print_bytes(const char *bytes, size_t len);

// only to be called from the top-level libuv callbacks
void dns_close(globals_t *g, derr_t e);

// membuf.c //

void membuf_free(membuf_t **ptr);
derr_t membuf_init(membuf_t **ptr);
void membufs_free(link_t *membufs);
derr_t membufs_init(link_t *membufs, size_t n);
void membuf_return(globals_t *g, membuf_t **ptr);

// parse.c //

typedef struct {
    size_t len;
    const char *str;
} lstr_t;

typedef struct {
    const char *ptr;
    size_t pos;
    lstr_t lstr;
    // the number of bytes used after consuming this label,
    // ignoring any bytes read after a label pointer;
    // only valid after iteration is complete
    size_t used;
    bool after_ptr;
} labels_t;

lstr_t *labels_iter(labels_t *it, const char *ptr, size_t start);
lstr_t *labels_next(labels_t *it);

// returns -1 on failure
int parse_qtype(const char *qtype);

// returns UNKNOWN on error
const char *qtype_tostr(int qtype);

// returns 0 if ok, or BAD_PARSE if not ok
size_t parse_pkt(dns_pkt_t *pkt, const char *ptr, size_t len);
void print_pkt(const dns_pkt_t pkt);

// dns.c //

// owns membuf
derr_t handle_packet(globals_t *g, membuf_t *membuf);
