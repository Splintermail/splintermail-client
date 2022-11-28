// this is the lower limit recommended in rfc6891, not sure how to pick better
#define UDP_MAX_SIZE 1280

size_t namelen(const lstr_t *labels, size_t nlabels);
size_t put_name(
    const lstr_t *labels, size_t nlabels, char *out, size_t used
);

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

// rewrites the question without any pointers
size_t write_qstn(const dns_qstn_t qstn, char *out, size_t cap, size_t used);

size_t write_edns(char *out, size_t cap, size_t used);

typedef size_t (*writer_f)(size_t nameoff, char *out, size_t cap, size_t used);

// write_soa happens to ignore nameoff, so it can be used in negative responses
size_t write_soa(size_t nameoff, char *out, size_t cap, size_t used);

size_t write_a(size_t nameoff, char *out, size_t cap, size_t used);
size_t write_ns1(size_t nameoff, char *out, size_t cap, size_t used);
size_t write_ns2(size_t nameoff, char *out, size_t cap, size_t used);
size_t write_ns3(size_t nameoff, char *out, size_t cap, size_t used);
size_t write_notfound(size_t nameoff, char *out, size_t cap, size_t used);
size_t write_aaaa(size_t nameoff, char *out, size_t cap, size_t used);
size_t write_caa(size_t nameoff, char *out, size_t cap, size_t used);
