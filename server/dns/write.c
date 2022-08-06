#include <string.h>

#include <uv.h>

#include "libdstr/libdstr.h"
#include "server/dns/dns.h"

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
static size_t namelen(const lstr_t *labels, size_t nlabels){
    size_t used = 1;  // for the terminating '\0'
    for(size_t i = 0; i < nlabels; i++){
        used += labels[i].len + 1;
    }
    return used;
}

// TODO: support name pointers in messages we write
static size_t put_name(
    const lstr_t *labels, size_t nlabels, char *out, size_t used
){
    for(size_t i = 0; i < nlabels; i++){
        used = put_uint8(labels[i].len, out, used);
        used = put_lstr(labels[i], out, used);
    }
    used = put_uint8(0, out, used);
    return used;
}

void put_hdr(
    const dns_hdr_t hdr,
    uint16_t rcode,
    bool aa,
    bool tc,
    uint16_t ancount,
    uint16_t nscount,
    uint16_t arcount,
    char *out
){
    uint8_t *uptr = (uint8_t*)out;
    uptr[0] = (hdr.id >> 8) & 0xff;
    uptr[1] = (hdr.id >> 0) & 0xff;
    uptr[2] = 0x80 // qr bit always "response" (1)
            | 0x00 // opcode always 0
            | (aa & 0x01) << 2
            | (tc & 0x01) << 1
            | (hdr.rd & 0x01);
    uptr[3] = 0x00 // recursion allowed always 0
            | 0x00 // z bits always zero
            | (rcode & 0x07);
    uptr[4] = (hdr.qdcount >> 8) & 0xff;
    uptr[5] = (hdr.qdcount >> 0) & 0xff;
    uptr[6] = (ancount >> 8) & 0xff;
    uptr[7] = (ancount >> 0) & 0xff;
    uptr[8] = (nscount >> 8) & 0xff;
    uptr[9] = (nscount >> 0) & 0xff;
    uptr[10] = (arcount >> 8) & 0xff;
    uptr[11] = (arcount >> 0) & 0xff;
}

static size_t rrlen(const lstr_t *labels, size_t nlabels, size_t rdlen){
    return namelen(labels, nlabels) + 10 + rdlen;
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
    // edns type = 41
    used = put_uint16(41, out, used);
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


size_t write_soa(char *out, size_t cap, size_t used){
    // count only once
    static size_t rdlen = 0;
    static size_t needed = 0;
    if(rdlen == 0 || needed == 0){
        rdlen = namelen(MNAME, MNAME_COUNT) + namelen(RNAME, RNAME_COUNT) + 20;
        needed = rrlen(SOA_NAME, SOA_NAME_COUNT, rdlen);
    }

    if(used + needed > cap) return used + needed;
    used = put_name(SOA_NAME, SOA_NAME_COUNT, out, used);
    used = put_rr_hdr(6, SOA_TTL, rdlen, out, used);

    used = put_name(MNAME, MNAME_COUNT, out, used);
    used = put_name(RNAME, RNAME_COUNT, out, used);
    // SERIAL, always 0 since it seems to be only useful for zone transfers
    used = put_uint32(0, out, used);
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
