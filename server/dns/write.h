// this is the lower limit recommended in rfc6891, not sure how to pick better
#define UDP_MAX_SIZE 1280

size_t namelen(const lstr_t *labels, size_t nlabels);
size_t put_name(
    const lstr_t *labels, size_t nlabels, char *out, size_t used
);

size_t write_hdr(
    const dns_hdr_t hdr,
    char *out,
    size_t cap,
    size_t used
);

size_t write_response_hdr(
    const dns_hdr_t hdr, // the query hdr
    uint16_t rcode,
    bool aa,
    bool tc,
    uint16_t ancount,
    uint16_t nscount,
    uint16_t arcount,
    char *out,
    size_t cap,
    size_t used
);

// rewrites the question without any pointers
size_t write_qstn(const dns_qstn_t qstn, char *out, size_t cap, size_t used);

size_t write_edns(char *out, size_t cap, size_t used);

typedef size_t (*writer_f)(
    void *arg, size_t nameoff, char *out, size_t cap, size_t used
);

// write_soa happens to ignore nameoff, so it can be used in negative responses
size_t write_soa(void*, size_t, char*, size_t, size_t);

size_t write_a(void*, size_t, char*, size_t, size_t);
size_t write_ns1(void*, size_t, char*, size_t, size_t);
size_t write_ns2(void*, size_t, char*, size_t, size_t);
size_t write_secret(void*, size_t, char*, size_t, size_t);
size_t write_aaaa(void*, size_t, char*, size_t, size_t);
size_t write_caa(void*, size_t, char*, size_t, size_t);
