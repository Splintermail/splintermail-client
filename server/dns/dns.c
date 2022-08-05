#include <string.h>

#include <uv.h>

#include "libdstr/libdstr.h"
#include "server/dns/dns.h"

typedef derr_t (*respond_f)(const dns_pkt_t pkt, const lstr_t user);

/* we only have data for:
   - "root": user.splintermail.com. SOA / NS
   - "user": *.user.splintermail.com. A / AAAA
   - "acme": _acme-challenge.*.user.splintermail.com. TXT */

static derr_t norespond(const dns_pkt_t pkt, const lstr_t user){
    (void)pkt;
    (void)user;
    printf("no response\n");
    return E_OK;
}

static derr_t respond_notimpl(const dns_pkt_t pkt, const lstr_t user){
    derr_t e = E_OK;
    (void)pkt;
    (void)user;
    printf("not implemented\n");
    return e;
}

static derr_t respond_name_error(const dns_pkt_t pkt, const lstr_t user){
    derr_t e = E_OK;
    (void)pkt;
    (void)user;
    printf("name error\n");
    return e;
}

static derr_t respond_norecord(const dns_pkt_t pkt, const lstr_t user){
    derr_t e = E_OK;
    (void)pkt;
    (void)user;
    printf("no record\n");
    return e;
}

// for user.splintermail.com
static derr_t respond_root(const dns_pkt_t pkt, const lstr_t user){
    derr_t e = E_OK;
    (void)user;

    if(pkt.qstn.qtype == 6){
        // 'SOA' record
        printf("SOA\n");
        return e;
    }

    if(pkt.qstn.qtype == 2){
        // 'NS' record
        printf("NS\n");
        return e;
    }

    // no matching record
    PROP(&e, respond_norecord(pkt, user) );

    return e;
}

// for *.user.splintermail.com
static derr_t respond_user(const dns_pkt_t pkt, const lstr_t user){
    derr_t e = E_OK;
    (void)user;

    if(pkt.qstn.qtype == 1){
        // 'A' record
        printf("A\n");
        return e;
    }

    if(pkt.qstn.qtype == 28){
        // 'AAAA' record
        printf("AAAA\n");
        return e;
    }

    // no matching record
    PROP(&e, respond_norecord(pkt, user) );

    return e;
}

// for _acme-challenge.*.user.splintermail.com
static derr_t respond_acme(const dns_pkt_t pkt, const lstr_t user){
    derr_t e = E_OK;

    if(pkt.qstn.qtype == 16){
        // 'TXT' record
        (void)user;
        printf("TXT\n");
        return e;
    }

    // no matching record
    PROP(&e, respond_norecord(pkt, user) );

    return e;
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
derr_t handle_packet(globals_t *g, membuf_t *membuf){
    derr_t e = E_OK;

    (void)g;

    print_bytes(membuf->base, membuf->len);
    printf("\n");

    dns_pkt_t pkt;
    size_t pret = parse_pkt(&pkt, membuf->base, membuf->len);
    if(pret == BAD_PARSE){
        printf("bad packet!\n");
        goto cu;
    }

    print_pkt(pkt);

    lstr_t rname[5] = {0};
    size_t cap = sizeof(rname) / sizeof(*rname);
    size_t n = labels_read_reverse(pkt.qstn.ptr, pkt.qstn.off, rname, cap);
    respond_f respond = sort_pkt(pkt, rname, n);
    // username would be at index 3:  com.splintermail.user.*
    lstr_t user = n > 3 ? rname[3] : (lstr_t){0};

    PROP_GO(&e, respond(pkt, user), cu);

cu:
    membuf_return(&membuf);

    return e;
}
