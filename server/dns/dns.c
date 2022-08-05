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
   - "user": *.user.splintermail.com. A / AAAA
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

static size_t respond_notimpl(
    const dns_pkt_t pkt, const lstr_t user, char *out, size_t cap
){
    (void)user;

    // reserve the header bytes
    size_t used = DNS_HDR_SIZE;
    size_t len = used;

    used = copy_qstn(pkt.qstn, out, cap, used);
    if(used <= cap) len = used;

    bool aa = false;
    bool tc = used > cap;
    write_hdr(pkt.hdr, RCODE_NOTIMPL, aa, tc, 0, 0, 0, out);

    return len;
}

static size_t respond_name_error(
    const dns_pkt_t pkt, const lstr_t user, char *out, size_t cap
){
    (void)pkt;
    (void)user;
    (void)out;
    (void)cap;
    printf("name error\n");
    return 0;
}

static size_t respond_norecord(
    const dns_pkt_t pkt, const lstr_t user, char *out, size_t cap
){
    (void)pkt;
    (void)user;
    (void)out;
    (void)cap;
    printf("no record\n");
    return 0;
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
    return respond_norecord(pkt, user, out, cap);
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

    // no matching record
    return respond_norecord(pkt, user, out, cap);
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
    return respond_norecord(pkt, user, out, cap);
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
    // we only implement the basic qtypes from rfc1035 + AAAA
    if(pkt.qstn.qtype > 16 && pkt.qstn.qtype != 28) return respond_notimpl;

    if(n < 3) return respond_name_error;
    if(!lstr_eq(rname[0], LSTR("com"))) return respond_name_error;
    if(!lstr_eq(rname[1], LSTR("splintermail"))) return respond_name_error;
    if(!lstr_eq(rname[2], LSTR("user"))) return respond_name_error;
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

    rcap = MIN(rcap, 512); // where to handle EDNS analysis?

    return respond(pkt, user, rbuf, rcap);
}
