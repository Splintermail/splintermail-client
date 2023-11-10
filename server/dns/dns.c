#include <string.h>

#include "server/dns/libdns.h"

// for tracking UNSURE after converting to lstr_t
char C_UNSURE = 0;

size_t norespond(void *arg, const dns_pkt_t pkt, char *out, size_t cap){
    (void)arg;
    (void)pkt;
    (void)out;
    (void)cap;
    LOG_DEBUG("no response\n");
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
        used = write_soa(NULL, 0, out, cap - edns_resv, used);
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
    write_response_hdr(
        pkt.hdr, rcode, aa, tc, 0, nscount, arcount, out, cap, 0
    );

    return len;
}

static size_t positive_resp(
    writer_f *writers,
    size_t nwriters,
    void *arg,
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
        used = writers[i](arg, DNS_HDR_SIZE, out, cap, used);
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
    write_response_hdr(
        pkt.hdr, RCODE_OK, aa, tc, ancount, 0, arcount, out, cap, 0
    );

    return len;
}
#define POSITIVE_RESP(...) \
    positive_resp( \
        (writer_f[]){__VA_ARGS__}, \
        sizeof((writer_f[]){__VA_ARGS__}) / sizeof(writer_f*), \
        arg, \
        pkt, \
        out, \
        cap \
    )

size_t respond_notimpl(void *arg, const dns_pkt_t pkt, char *out, size_t cap){
    (void)arg;

    bool aa = false;
    bool soa = false;
    return negative_resp(pkt, RCODE_NOTIMPL, aa, soa, out, cap);
}

size_t respond_refused(void *arg, const dns_pkt_t pkt, char *out, size_t cap){
    (void)arg;

    bool aa = false;
    bool soa = false;
    return negative_resp(pkt, RCODE_REFUSED, aa, soa, out, cap);
}

size_t respond_name_error(
    void *arg, const dns_pkt_t pkt, char *out, size_t cap
){
    (void)arg;

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
size_t respond_root(void *arg, const dns_pkt_t pkt, char *out, size_t cap){
    if(pkt.qstn.qtype == SOA){
        return POSITIVE_RESP(write_soa);
    }

    if(pkt.qstn.qtype == NS){
        return POSITIVE_RESP(write_ns1, write_ns2);
    }

    if(pkt.qstn.qtype == CAA){
        return POSITIVE_RESP(write_caa);
    }

    // no matching record
    return norecord_resp(pkt, out, cap);
}

// for *.user.splintermail.com
size_t respond_user(void *arg, const dns_pkt_t pkt, char *out, size_t cap){
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
size_t respond_acme(void *arg, const dns_pkt_t pkt, char *out, size_t cap){
    if(pkt.qstn.qtype != TXT){
        // only respond to TXT queries
        return norecord_resp(pkt, out, cap);
    }

    lstr_t secret = *((lstr_t*)arg);

    if(secret.str == NULL){
        // confident "no"
        return norecord_resp(pkt, out, cap);
    }

    if(secret.str == &C_UNSURE){
        // no confident response
        bool aa = true;
        bool soa = false;
        return negative_resp(pkt, RCODE_SRVERR, aa, soa, out, cap);
    }

    // send the secret TXT
    // right now we report NOTFOUND to all txt queries.
    return POSITIVE_RESP(write_secret);

}

// always sets *respond and *user
respond_f sort_pkt(const dns_pkt_t pkt, const lstr_t *rname, size_t n){
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

    // refuse to respond for domains to we're not responsible for
    if(n < 3) return respond_refused;
    if(!lstr_ieq(rname[0], LSTR("com"))) return respond_refused;
    if(!lstr_ieq(rname[1], LSTR("splintermail"))) return respond_refused;
    if(!lstr_ieq(rname[2], LSTR("user"))) return respond_refused;

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
    if(n == 5 && lstr_ieq(rname[4], LSTR("_acme-challenge"))){
        // matched: _acme-challenge.*.user.splintermail.com
        return respond_acme;
    }

    return respond_name_error;
}

size_t handle_packet(
    char *qbuf, size_t qlen, kvp_i *kvp, char *rbuf, size_t rcap
){
    // require a minimum rcap size to function
    if(rcap < 512){
        fprintf(stderr, "rcap too small to function!\n");
        return 0;
    }

    dns_pkt_t pkt;
    size_t pret = parse_pkt(&pkt, qbuf, qlen);
    if(is_bad_parse(pret)){
        LOG_DEBUG("bad packet!\n");
        return 0;
    }

    LOG_DEBUG("%x", FPKT(pkt));

    lstr_t rname[5] = {0};
    size_t cap = sizeof(rname) / sizeof(*rname);
    size_t n = labels_read_reverse(pkt.qstn.ptr, pkt.qstn.off, rname, cap);
    respond_f respond = sort_pkt(pkt, rname, n);

    void *arg = NULL;
    lstr_t secret = {0};
    if(respond == respond_acme){
        // username would be at index 3: com.splintermail.user.*
        if(n < 4) LOG_FATAL("respond_acme doesn't have enough labels\n");
        lstr_t user = rname[3];
        const dstr_t *dsecret = kvp->iget(kvp, user);
        if(dsecret == UNSURE){
            secret.str = &C_UNSURE;
        }else if(dsecret){
            secret.str = dsecret->data;
            secret.len = dsecret->len;
        }
        arg = (void*)&secret;
    }

    // pick a good rcap
    if(pkt.edns.found){
        rcap = MIN(rcap, pkt.edns.udp_size);
        // don't allow edns to force packet size smaller than 512
        rcap = MAX(rcap, 512);
    }else{
        rcap = MIN(rcap, 512);
    }

    return respond(arg, pkt, rbuf, rcap);
}
