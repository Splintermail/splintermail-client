#include <string.h>

#include <uv.h>

#include "server/dns/libdns.h"

#include "server/dns/config.h"

static size_t put_uint8(uint8_t n, char *out, size_t used){
    uint8_t *uptr = (uint8_t*)out;
    uptr[used] = n;
    return used + 1;
}

static size_t put_uint16(uint16_t n, char *out, size_t used){
    uint8_t *uptr = (uint8_t*)out;
    uptr[used+0] = (n >> 8) & 0xff;
    uptr[used+1] = (n >> 0) & 0xff;
    return used + 2;
}

static size_t put_uint32(uint32_t n, char *out, size_t used){
    uint8_t *uptr = (uint8_t*)out;
    uptr[used+0] = (n >> 24) & 0xff;
    uptr[used+1] = (n >> 16) & 0xff;
    uptr[used+2] = (n >> 8 ) & 0xff;
    uptr[used+3] = (n >> 0 ) & 0xff;
    return used + 4;
}

static size_t put_lstr(const lstr_t l, char *out, size_t used){
    memcpy(out + used, l.str, l.len);
    return used + l.len;
}

// TODO: support name pointers in messages we write
size_t namelen(const lstr_t *labels, size_t nlabels){
    size_t used = 1;  // for the terminating '\0'
    for(size_t i = 0; i < nlabels; i++){
        used += labels[i].len + 1;
    }
    return used;
}

// TODO: support name pointers in messages we write
size_t put_name(
    const lstr_t *labels, size_t nlabels, char *out, size_t used
){
    for(size_t i = 0; i < nlabels; i++){
        used = put_uint8(labels[i].len, out, used);
        used = put_lstr(labels[i], out, used);
    }
    used = put_uint8(0, out, used);
    return used;
}

static size_t put_name_ptr(size_t offset, char *out, size_t used){
    uint16_t off14 = MIN(offset, 0x3FFF);
    return put_uint16(0xC000 | off14, out, used);
}

size_t write_hdr(
    const dns_hdr_t hdr,
    char *out,
    size_t cap,
    size_t used
){
    if(used + DNS_HDR_SIZE > cap) return used + DNS_HDR_SIZE;
    used = put_uint16(hdr.id, out, used);
    used = put_uint8(
        (hdr.qr & 0x01) << 7
        | hdr.opcode << 3
        | (hdr.aa & 0x01) << 2
        | (hdr.tc & 0x01) << 1
        | (hdr.rd & 0x01),
        out,
        used
    );
    used = put_uint8(
        (hdr.ra & 0x01) << 7
        | 0x00 // z bits always zero
        | (hdr.rcode & 0x0f),
        out,
        used
    );
    used = put_uint16(hdr.qdcount, out, used);
    used = put_uint16(hdr.ancount, out, used);
    used = put_uint16(hdr.nscount, out, used);
    used = put_uint16(hdr.arcount, out, used);
    return used;
}

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
){
    return write_hdr(
        (dns_hdr_t){
            .id = hdr.id,
            .qr = 1, // qr bit always "response" (1)
            .opcode = 0, // always standard query = 0
            .aa = aa,
            .tc = tc,
            .rd = hdr.rd,
            .ra = 0, // recursion allowed always 0
            .rcode = rcode,
            .qdcount = hdr.qdcount,
            .ancount = ancount,
            .nscount = nscount,
            .arcount = arcount,
        },
        out,
        cap,
        used
    );
}

// rewrites the question without any pointers
size_t write_qstn(const dns_qstn_t qstn, char *out, size_t cap, size_t used){
    if(qstn.qdcount != 1){
        fprintf(stderr, "multiple questions not supported in write_qstn\n");
        return used;
    }

    labels_t it;
    size_t needed = 5;  // qtype + qclass + null-termination of name
    lstr_t *l = labels_iter(&it, qstn.ptr, qstn.off);
    for(; l; l = labels_next(&it)){
        needed += l->len + 1;
    }

    if(used + needed > cap) return used + needed;

    // qname
    l = labels_iter(&it, qstn.ptr, qstn.off);
    for(; l; l = labels_next(&it)){
        used = put_uint8(l->len, out, used);
        used = put_lstr(*l, out, used);
    }
    used = put_uint8(0, out, used);
    // qtype
    used = put_uint16(qstn.qtype, out, used);
    used = put_uint16(qstn.qclass, out, used);

    return used;
}


static size_t put_rr_hdr(
    uint16_t type,
    uint32_t ttl,
    uint16_t rdlen,
    char *out,
    size_t used
){
    used = put_uint16(type, out, used);
    // only support IN class
    used = put_uint16(1, out, used);
    used = put_uint32(ttl, out, used);
    used = put_uint16(rdlen, out, used);
    return used;
}

size_t write_edns(char *out, size_t cap, size_t used){
    if(used + BARE_EDNS_SIZE > cap) return used + BARE_EDNS_SIZE;
    // required empty name
    used = put_uint8(0, out, used);
    // rr.type
    used = put_uint16(EDNS, out, used);
    // rr.class: our max packet size
    used = put_uint16(UDP_MAX_SIZE, out, used);
    // rr.ttl[0]: extended rcode (we support none of them)
    // rr.ttl[1]: edns version
    // rr.ttl[2]: dnssec_ok bit and zeros
    // rr.ttl[3]: zeros
    used = put_uint32(0, out, used);
    // rr.rdlen: zero
    used = put_uint16(0, out, used);
    return used;
}


// write_soa happens to ignore nameoff, so it can be used in negative responses
size_t write_soa(
    void *arg, size_t nameoff, char *out, size_t cap, size_t used
){
    (void)arg;
    (void)nameoff;

    (void)namelen;
    // size_t rdlen = namelen(MNAME, MNAME_COUNT)
    //              + namelen(RNAME, RNAME_COUNT)
    //              + 20;
    // size_t needed = namelen(ZONE_NAME, ZONE_NAME_COUNT)
    //               + RR_HDR_SIZE
    //               + rdlen;
    // printf("rdlen=%zu, needed=%zu\n", rdlen, needed);

    // checked in tests
    static const size_t rdlen = 71;
    static const size_t needed = 104;

    if(used + needed > cap) return used + needed;
    used = put_name(ZONE_NAME, ZONE_NAME_COUNT, out, used);
    used = put_rr_hdr(SOA, SOA_TTL, rdlen, out, used);

    used = put_name(MNAME, MNAME_COUNT, out, used);
    used = put_name(RNAME, RNAME_COUNT, out, used);
    // SERIAL, constant since it seems to be only useful for zone transfers
    used = put_uint32(SERIAL, out, used);
    // REFRESH: 5 min
    used = put_uint32(300, out, used);
    // RETRY: 1 min
    used = put_uint32(60, out, used);
    // EXPIRE: 1 day
    used = put_uint32(86400, out, used);
    // MINIMUM: 1 min
    used = put_uint32(60, out, used);
    return used;
}

size_t write_a(
    void *arg, size_t nameoff, char *out, size_t cap, size_t used
){
    (void)arg;
    static const size_t rdlen = 4;  // 32-bit IPv4 address size
    static const size_t needed = 2 + RR_HDR_SIZE + rdlen;

    if(used + needed > cap) return used + needed;
    // name pointer
    used = put_name_ptr(nameoff, out, used);
    // rr header
    used = put_rr_hdr(A, A_TTL, rdlen, out, used);
    // A record: always 127.0.0.1
    used = put_uint8(127, out, used);
    used = put_uint8(0, out, used);
    used = put_uint8(0, out, used);
    used = put_uint8(1, out, used);
    return used;
}

static size_t _write_ns(
    const lstr_t *name, size_t count, char *out, size_t cap, size_t used
){
    (void)namelen;
    // size_t rdlen = namelen(NS1_NAME, NS1_NAME_COUNT);
    // size_t needed = namelen(ZONE_NAME, ZONE_NAME_COUNT)
    //               + RR_HDR_SIZE
    //               + rdlen;
    // printf("rdlen=%zu, needed=%zu\n", rdlen, needed);

    // checked in tests
    static const size_t rdlen = 22;
    static const size_t needed = 55;

    if(used + needed > cap) return used + needed;
    used = put_name(ZONE_NAME, ZONE_NAME_COUNT, out, used);
    used = put_rr_hdr(NS, NS_TTL, rdlen, out, used);
    used = put_name(name, count, out, used);
    return used;
}

// ignores nameoff
size_t write_ns1(
    void *arg, size_t nameoff, char *out, size_t cap, size_t used
){
    (void)arg;
    (void)nameoff;
    return _write_ns(NS1_NAME, NS1_NAME_COUNT, out, cap, used);
}
size_t write_ns2(
    void *arg, size_t nameoff, char *out, size_t cap, size_t used
){
    (void)arg;
    (void)nameoff;
    return _write_ns(NS2_NAME, NS2_NAME_COUNT, out, cap, used);
}

size_t write_secret(
    void *arg, size_t nameoff, char *out, size_t cap, size_t used
){
    lstr_t secret = *((lstr_t*)arg);

    const size_t rdlen = 1 + secret.len;
    const size_t needed = 2 + RR_HDR_SIZE + rdlen;

    if(used + needed > cap) return used + needed;
    // name pointer
    used = put_name_ptr(nameoff, out, used);
    // rr header
    used = put_rr_hdr(TXT, TXT_TTL, rdlen, out, used);
    // TXT content
    if(secret.len > UINT8_MAX) LOG_FATAL("secret too long\n");
    used = put_uint8((uint8_t)secret.len, out, used);
    used = put_lstr(secret, out, used);
    return used;
}

size_t write_aaaa(
    void *arg, size_t nameoff, char *out, size_t cap, size_t used
){
    (void)arg;
    static const size_t rdlen = 16;  // 128-bit IPv6 address size
    static const size_t needed = 2 + RR_HDR_SIZE + rdlen;

    if(used + needed > cap) return used + needed;
    // name pointer
    used = put_name_ptr(nameoff, out, used);
    // rr header
    used = put_rr_hdr(AAAA, AAAA_TTL, rdlen, out, used);
    // AAAA record: always ::1
    used = put_uint32(0, out, used);
    used = put_uint32(0, out, used);
    used = put_uint32(0, out, used);
    used = put_uint32(1, out, used);
    return used;
}

size_t write_caa(
    void *arg, size_t nameoff, char *out, size_t cap, size_t used
){
    (void)arg;
    static const size_t rdlen = 2 + CAA_TAG.len + CAA_VALUE.len;
    static const size_t needed = 2 + RR_HDR_SIZE + rdlen;

    if(used + needed > cap) return used + needed;
    // name pointer
    used = put_name_ptr(nameoff, out, used);
    // rr header
    used = put_rr_hdr(CAA, CAA_TTL, rdlen, out, used);
    // critical byte
    used = put_uint8(CAA_FLAGS, out, used);
    // tag len and tag
    used = put_uint8(CAA_TAG.len, out, used);
    used = put_lstr(CAA_TAG, out, used);
    // CA name
    used = put_lstr(CAA_VALUE, out, used);
    return used;
}
