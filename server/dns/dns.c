#include <string.h>

#include <uv.h>

#include "libdstr/libdstr.h"
#include "server/dns/dns.h"


typedef size_t (*respond_f)(
    const dns_pkt_t pkt, const lstr_t user, char *out, size_t cap
);

static size_t norespond(
    const dns_pkt_t pkt, const lstr_t user, char *out, size_t cap
){
    (void)pkt;
    (void)user;
    (void)out;
    (void)cap;
    printf("no response\n");
    return 0;
}

// some states we accumulate when we've written part of a packet out.
typedef struct {
    size_t len;
    size_t used;
    bool tc;
    uint16_t nscount;
    uint16_t arcount;
} partial_t;

static size_t negative_resp(
    const dns_pkt_t pkt,
    uint16_t rcode,
    bool aa,
    bool soa,
    char *out,
    size_t cap
){
    // skip header for now
    size_t used = DNS_HDR_SIZE;
    size_t len = used;

    // edns outranks other data, so reserve space
    size_t edns_resv = pkt.edns.found ? BARE_EDNS_SIZE : 0;

    used = write_qstn(pkt.qstn, out, cap - edns_resv, used);
    if(used <= cap) len = used;

    uint16_t nscount = 0;
    if(soa){
        used = write_soa(0, out, cap - edns_resv, used);
        if(used <= cap){
            len = used;
            nscount++;
        }
    }

    uint16_t arcount = 0;
    if(pkt.edns.found){
        used = write_edns(out, cap, used);
        if(used <= cap){
            len = used;
            arcount++;
        }
    }

    bool tc = used > cap;
    put_hdr(pkt.hdr, rcode, aa, tc, 0, nscount, arcount, out);

    return len;
}

static size_t positive_resp(
    writer_f *writers,
    size_t nwriters,
    const dns_pkt_t pkt,
    char *out,
    size_t cap
){
    // skip header for now
    size_t used = DNS_HDR_SIZE;
    size_t len = used;

    // edns outranks other data, so reserve space
    size_t edns_resv = pkt.edns.found ? BARE_EDNS_SIZE : 0;

    used = write_qstn(pkt.qstn, out, cap - edns_resv, used);
    if(used <= cap) len = used;

    uint16_t ancount = 0;
    for(size_t i = 0;  i < nwriters; i++){
        used = writers[i](DNS_HDR_SIZE, out, cap, used);
        if(used <= cap){
            len = used;
            ancount++;
        }
    }

    uint16_t arcount = 0;
    if(pkt.edns.found){
        used = write_edns(out, cap, used);
        if(used <= cap){
            len = used;
            arcount++;
        }
    }

    // we only serve records for which we are authoritative
    bool aa = true;
    bool tc = used > cap;
    put_hdr(pkt.hdr, RCODE_OK, aa, tc, ancount, 0, arcount, out);

    return len;
}
#define POSITIVE_RESP(...) \
    positive_resp( \
        (writer_f[]){__VA_ARGS__}, \
        sizeof((writer_f[]){__VA_ARGS__}) / sizeof(writer_f*), \
        pkt, \
        out, \
        cap \
    )

static size_t respond_notimpl(
    const dns_pkt_t pkt, const lstr_t user, char *out, size_t cap
){
    (void)user;

    bool aa = false;
    bool soa = false;
    return negative_resp(pkt, RCODE_NOTIMPL, aa, soa, out, cap);
}

static size_t respond_refused(
    const dns_pkt_t pkt, const lstr_t user, char *out, size_t cap
){
    (void)user;

    bool aa = false;
    bool soa = false;
    return negative_resp(pkt, RCODE_REFUSED, aa, soa, out, cap);
}

static size_t respond_name_error(
    const dns_pkt_t pkt, const lstr_t user, char *out, size_t cap
){
    (void)user;

    bool aa = true;
    bool soa = true;
    return negative_resp(pkt, RCODE_NAMEERR, aa, soa, out, cap);
}

static size_t norecord_resp(const dns_pkt_t pkt, char *out, size_t cap){
    bool aa = true;
    bool soa = true;
    return negative_resp(pkt, RCODE_OK, aa, soa, out, cap);
}

// for user.splintermail.com
static size_t respond_root(
    const dns_pkt_t pkt, const lstr_t user, char *out, size_t cap
){
    (void)user;

    if(pkt.qstn.qtype == SOA){
        return POSITIVE_RESP(write_soa);
    }

    if(pkt.qstn.qtype == NS){
        return POSITIVE_RESP(write_ns1, write_ns2, write_ns3);
    }

    // no matching record
    return norecord_resp(pkt, out, cap);
}

// for *.user.splintermail.com
static size_t respond_user(
    const dns_pkt_t pkt, const lstr_t user, char *out, size_t cap
){
    (void)user;

    if(pkt.qstn.qtype == A){
        return POSITIVE_RESP(write_a);
    }

    if(pkt.qstn.qtype == AAAA){
        return POSITIVE_RESP(write_aaaa);
    }

    if(pkt.qstn.qtype == CAA){
        return POSITIVE_RESP(write_caa);
    }

    // no matching record
    return norecord_resp(pkt, out, cap);
}

// for _acme-challenge.*.user.splintermail.com
static size_t respond_acme(
    const dns_pkt_t pkt, const lstr_t user, char *out, size_t cap
){
    if(pkt.qstn.qtype == TXT){
        (void)user;
        // right now we report NOTFOUND to all txt queries.
        return POSITIVE_RESP(write_notfound);
    }

    // no matching record
    return norecord_resp(pkt, out, cap);
}

// always sets *respond and *user
static respond_f sort_pkt(const dns_pkt_t pkt, const lstr_t *rname, size_t n){
    // only service queries
    if(pkt.hdr.qr != 0) return norespond;
    // only service standard queries
    if(pkt.hdr.opcode != 0) return norespond;
    // only service one question at a time, like tinydns
    if(pkt.hdr.qdcount != 1) return norespond;
    // only respond to IN-class queries
    if(pkt.qstn.qclass != 1) return norespond;
    // 0 is not a valid qtype
    if(pkt.qstn.qtype == 0) return norespond;

    // refuse to respond for to we're note responsible for
    if(n < 3) return respond_refused;
    if(!lstr_eq(rname[0], LSTR("com"))) return respond_refused;
    if(!lstr_eq(rname[1], LSTR("splintermail"))) return respond_refused;
    if(!lstr_eq(rname[2], LSTR("user"))) return respond_refused;

    // we only implement the basic qtypes from rfc1035 + AAAA + CAA
    uint16_t qtype = pkt.qstn.qtype;
    if(qtype > TXT && qtype != AAAA && qtype != CAA) return respond_notimpl;

    if(n == 3){
        // matched: user.splintermail.com
        return respond_root;
    }
    if(n == 4){
        // matched: *.user.splintermail.com
        return respond_user;
    }
    if(n == 5 && lstr_eq(rname[4], LSTR("_acme-challenge"))){
        // matched: _acme-challenge.*.user.splintermail.com
        return respond_acme;
    }

    return respond_name_error;
}

// owns membuf
size_t handle_packet(char *qbuf, size_t qlen, char *rbuf, size_t rcap){
    print_bytes(qbuf, qlen);
    printf("\n");

    // require a minimum rcap size to function
    if(rcap < 512){
        fprintf(stderr, "rcap too small to function!\n");
        return 0;
    }

    dns_pkt_t pkt;
    size_t pret = parse_pkt(&pkt, qbuf, qlen);
    if(is_bad_parse(pret)){
        printf("bad packet!\n");
        return 0;
    }

    print_pkt(pkt);

    lstr_t rname[5] = {0};
    size_t cap = sizeof(rname) / sizeof(*rname);
    size_t n = labels_read_reverse(pkt.qstn.ptr, pkt.qstn.off, rname, cap);
    respond_f respond = sort_pkt(pkt, rname, n);
    // username would be at index 3: com.splintermail.user.*
    const lstr_t user = n > 3 ? rname[3] : (lstr_t){0};

    // pick a good rcap
    if(pkt.edns.found){
        rcap = MIN(rcap, pkt.edns.udp_size);
        // don't allow edns to force packet size smaller than 512
        rcap = MAX(rcap, 512);
    }else{
        rcap = MIN(rcap, 512);
    }

    return respond(pkt, user, rbuf, rcap);
}
