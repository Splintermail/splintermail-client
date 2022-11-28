// "l"abel string, or "l"ength string, whatever
typedef struct {
    size_t len;
    const char *str;
} lstr_t;

#define LSTR(literal) (lstr_t){ .str = literal, .len = sizeof(literal)-1 }

bool lstr_eq(const lstr_t a, const lstr_t b);

typedef enum {
    BP_MINVAL                = (size_t)-13,

    BP_ENDS_OPT_DATA_OVERRUN = (size_t)-14,
    BP_ENDS_OPT_HDR_OVERRUN  = (size_t)-13,
    BP_ENDS_NONEMPTY_NAME    = (size_t)-12,
    BP_DOUBLE_EDNS           = (size_t)-11,
    BP_RR_DATA_OVERRUN       = (size_t)-10,
    BP_RR_HDR_OVERRUN        = (size_t)-9,
    BP_QSTN_OVERRUN          = (size_t)-8,
    BP_LONG_NAME             = (size_t)-7,
    BP_LABEL_OVERRUN         = (size_t)-6,
    BP_LONG_LABEL            = (size_t)-5,
    BP_DOUBLE_POINTER        = (size_t)-4,
    BP_PTR_LEN_BYTE          = (size_t)-3,
    BP_LABEL_LEN_BYTE        = (size_t)-2,
    BP_HDR_OVERRUN           = (size_t)-1,
} bad_parse_e;

bool is_bad_parse(size_t n);
const char *bp2str(bad_parse_e bp);

// mnemonics
#define A 1
#define NS 2
#define SOA 6
#define TXT 16
#define AAAA 28
#define CAA 257
#define EDNS 41

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

// read labels until the zero label
// fail if any label exceeds 63 characters
// fail if length exceeds 255 characters
// allow pointers, but not double-pointers
// return the number of bytes used until zero label or first pointer
size_t parse_name(const char *ptr, size_t len, size_t used);

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
// does not include name or rdata
#define RR_HDR_SIZE 10

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
