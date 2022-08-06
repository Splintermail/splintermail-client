/* Why write a custom dns server?

   - Need dynamic updates.  And when we support DNSSEC, we want the signing key
     to not live on the machine running DNS.  DNS will be in a cloud machine,
     and we do not put keys on cloud machines.

   - RFC-defined wildcard behavior is not really correct for us, so it's far
     easier to hand-write the wildcard logic.

   - One record we want to emit is dynamic.  Existing solutions which are
     complete enough to allow plugin behavior are also crazy complex.

       - Bind has a crappy plugin interface; also their wildcard RRL is broken.

       - PowerDNS has a plugin architecture that would work, but I am unwilling
         to introduce C++ into the splintermail tech stack, just so I can
         include a heavyweight and complex dns server in production that I
         don't really want anyway.

       - DJB's tinydns is similar in spirit to our dns server, but is not
         capable enough to meet our needs.

   - Also it was fun to write. */

#define NMEMBUFS 256
#define MEMBUFSIZE 4096

// this is the lower limit recommended in rfc6891, not sure how to pick better
#define UDP_MAX_SIZE 1280

typedef enum {
    BP_MINVAL                = (size_t)-13,

    BP_ENDS_OPT_DATA_OVERRUN = (size_t)-13,
    BP_ENDS_OPT_HDR_OVERRUN  = (size_t)-12,
    BP_ENDS_NONEMPTY_NAME    = (size_t)-11,
    BP_DOUBLE_EDNS           = (size_t)-10,
    BP_RR_DATA_OVERRUN       = (size_t)-9,
    BP_RR_HDR_OVERRUN        = (size_t)-8,
    BP_QSTN_OVERRUN          = (size_t)-7,
    BP_LONG_NAME             = (size_t)-6,
    BP_LABEL_OVERRUN         = (size_t)-5,
    BP_LONG_LABEL            = (size_t)-4,
    BP_DOUBLE_POINTER        = (size_t)-3,
    BP_LABEL_LEN_BYTE        = (size_t)-2,
    BP_HDR_OVERRUN           = (size_t)-1,
} bad_parse_e;

#define RCODE_OK 0
#define RCODE_NAMEERR 3
#define RCODE_NOTIMPL 4
#define RCODE_REFUSED 5

typedef struct {
    uint16_t id;
    bool qr : 1;  // query (0) or response (1)
    uint8_t opcode : 4;  // standard query = 0
    bool aa : 1; // authoritative answer
    bool tc : 1; // truncation
    bool rd : 1; // recursion desired
    bool ra : 1; // recursion available
    uint8_t z : 3;
    uint16_t rcode : 12;  // big enough for edns extended-rcode
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_hdr_t;
#define DNS_HDR_SIZE 12

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
    bool found;
    uint8_t extrcode;
    uint8_t version;
    bool dnssec_ok : 1;
    uint16_t z : 15;
    uint16_t udp_size;
    // options
    const char *ptr;
    size_t optoff;
    size_t optcount;
} edns_t;
#define BARE_EDNS_SIZE 11 // we never emit options in our edns responses

typedef struct {
    dns_hdr_t hdr;
    dns_qstn_t qstn;
    dns_rr_t ans;
    dns_rr_t auth;
    dns_rr_t addl;
    edns_t edns;
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
    char resp[MEMBUFSIZE];
    link_t link; // globals_t->membufs;
    link_t *pool; // for easily returning a membuf to its pool
    // structs for sending the response
    uv_udp_send_t req;
    uv_buf_t uvbuf;
} membuf_t;
DEF_CONTAINER_OF(membuf_t, base, char);
DEF_CONTAINER_OF(membuf_t, link, link_t);
DEF_CONTAINER_OF(membuf_t, req, uv_udp_send_t);
DEF_STEAL_PTR(membuf_t);

// main.c //

// only to be called from the top-level libuv callbacks
void dns_close(globals_t *g, derr_t e);

// membuf.c //

void membuf_free(membuf_t **ptr);
derr_t membuf_init(membuf_t **ptr);
void membufs_free(link_t *membufs);
derr_t membufs_init(link_t *membufs, size_t n);
membuf_t *membufs_pop(link_t *membufs);
void membuf_return(membuf_t **ptr);

// parse.c //

bool is_bad_parse(size_t n);
const char *bp2str(bad_parse_e bp);

typedef struct {
    size_t len;
    const char *str;
} lstr_t;

#define LSTR(literal) (lstr_t){ .str = literal, .len = sizeof(literal)-1 }

bool lstr_eq(const lstr_t a, const lstr_t b);

size_t skip_name(const char *ptr, size_t used);

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

void labels_reverse(lstr_t *lstrs, size_t n);

// returns size_t nlabels; nlabels > cap indicates an error
size_t labels_read(const char *ptr, size_t start, lstr_t *lstrs, size_t cap);

// like lables_read, but reverse name before returning
size_t labels_read_reverse(
    const char *ptr, size_t start, lstr_t *lstrs, size_t cap
);


typedef struct {
    const char *ptr;
    size_t nameoff;
    uint16_t type;
    uint16_t class;
    uint32_t ttl;
    uint16_t rdlen;
    size_t rdoff;
} rr_t;

typedef struct {
    const char *ptr;
    size_t pos;
    size_t count;
    size_t seen;
    rr_t rr;
} rrs_t;

rr_t *dns_rr_iter(rrs_t *it, const dns_rr_t dns_rr);

rr_t *rrs_iter(rrs_t *it, const char *ptr, size_t start, size_t count);

rr_t *rrs_next(rrs_t *it);

typedef struct {
    uint16_t code;
    const char *ptr;
    size_t off;
    size_t len;
} opt_t;

typedef struct {
    const char *ptr;
    size_t pos;
    size_t count;
    size_t seen;
    opt_t opt;
} opts_t;

opt_t *opts_iter(opts_t *it, const edns_t edns);
opt_t *opts_next(opts_t *it);

// returns -1 on failure
int parse_qtype(const char *qtype);

// returns UNKNOWN on error
const char *qtype_tostr(int qtype);

// returns 0 if ok, or BAD_PARSE if not ok
size_t parse_pkt(dns_pkt_t *pkt, const char *ptr, size_t len);
void print_pkt(const dns_pkt_t pkt);

void print_bytes(const char *bytes, size_t len);

// write.c //

void put_hdr(
    const dns_hdr_t hdr,
    uint16_t rcode,
    bool aa,
    bool tc,
    uint16_t ancount,
    uint16_t nscount,
    uint16_t arcount,
    char *out
);

size_t write_soa(char *out, size_t cap, size_t used);

// dns.c //

size_t handle_packet(char *qbuf, size_t qlen, char *rbuf, size_t rcap);
size_t write_edns(char *out, size_t cap, size_t used);
