#include <string.h>

#include <uv.h>

#include "libdstr/libdstr.h"
#include "server/dns/dns.h"

void write_hdr(
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
            | (rcode & 0x04);
    uptr[4] = (hdr.qdcount >> 8) & 0xff;
    uptr[5] = (hdr.qdcount >> 0) & 0xff;
    uptr[6] = (ancount >> 8) & 0xff;
    uptr[7] = (ancount >> 0) & 0xff;
    uptr[8] = (nscount >> 8) & 0xff;
    uptr[9] = (nscount >> 0) & 0xff;
    uptr[10] = (arcount >> 8) & 0xff;
    uptr[11] = (arcount >> 0) & 0xff;
}

size_t write_edns(char *out, size_t cap, size_t used){
    uint8_t *uptr = (uint8_t*)out;
    if(used + 11 <= cap){
        uptr[used+0] = '\0'; // required empty name
        // edns type = 41
        uptr[used+1] = 0;
        uptr[used+2] = 41;
        // rr.class: our max packet size
        uptr[used+3] = (UDP_MAX_SIZE >> 8) & 0xff;
        uptr[used+4] = (UDP_MAX_SIZE >> 0) & 0xff;
        // rr.ttl[0]: extended rcode (we support none of them)
        uptr[used+5] = 0;
        // rr.ttl[1]: edns version
        uptr[used+6] = 0;
        // rr.ttl[2]: dnssec_ok bit and zeros
        uptr[used+7] = 0;
        // rr.ttl[3]: zeros
        uptr[used+8] = 0;
        // rr.rdlen: zero
        uptr[used+9] = 0;
        uptr[used+10] = 0;
    }
    return used + 11;
}
