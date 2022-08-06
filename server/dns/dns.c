#include <string.h>

#include <uv.h>

#include "libdstr/libdstr.h"
#include "server/dns/dns.h"

// writes nothing if question doesn't fit, but returns what used would be
static size_t copy_qstn(
    const dns_qstn_t qstn, char *out, size_t cap, size_t used
){
    if(used + qstn.len <= cap){
        memcpy(out + used, qstn.ptr + qstn.off, qstn.len);
    }
    return used + qstn.len;
}

typedef size_t (*respond_f)(
    const dns_pkt_t pkt, const lstr_t user, char *out, size_t cap
);

/* we only have data for:
   - "root": user.splintermail.com. SOA / NS
   - "user": *.user.splintermail.com. A / AAAA / CAA
   - "acme": _acme-challenge.*.user.splintermail.com. TXT */

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

    used = copy_qstn(pkt.qstn, out, cap - edns_resv, used);
    if(used <= cap) len = used;

    uint16_t nscount = 0;
    if(soa){
        used = write_soa(out, cap - edns_resv, used);
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

    if(pkt.qstn.qtype == 6){
        // 'SOA' record
        printf("SOA\n");
        return 0;
    }

    if(pkt.qstn.qtype == 2){
        // 'NS' record
        printf("NS\n");
        return 0;
    }

    // no matching record
    return norecord_resp(pkt, out, cap);
}

// for *.user.splintermail.com
static size_t respond_user(
    const dns_pkt_t pkt, const lstr_t user, char *out, size_t cap
){
    (void)user;

    if(pkt.qstn.qtype == 1){
        // 'A' record
        printf("A\n");
        return 0;
    }

    if(pkt.qstn.qtype == 28){
        // 'AAAA' record
        printf("AAAA\n");
        return 0;
    }

    if(pkt.qstn.qtype == 257){
        // 'CAA' record
        printf("CAA\n");
        return 0;
    }

    // no matching record
    return norecord_resp(pkt, out, cap);
}

// for _acme-challenge.*.user.splintermail.com
static size_t respond_acme(
    const dns_pkt_t pkt, const lstr_t user, char *out, size_t cap
){
    if(pkt.qstn.qtype == 16){
        // 'TXT' record
        (void)user;
        printf("TXT\n");
        return 0;
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
    if(qtype > 16 && qtype != 28 && qtype != 257) return respond_notimpl;

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
    // username would be at index 3:  com.splintermail.user.*
    lstr_t user = n > 3 ? rname[3] : (lstr_t){0};

    // pick a good rcap
    if(pkt.edns.found){
        rcap = MIN(rcap, pkt.edns.udp_size);
    }else{
        rcap = MIN(rcap, 512);
    }

    return respond(pkt, user, rbuf, rcap);
}
