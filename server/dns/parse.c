#include <uv.h>

#include "libdstr/libdstr.h"
#include "server/dns/dns.h"

#define BAD_PARSE ((size_t)-1)

char *UNKNOWN = "unknown";

// returns BAD_PARSE on invalid input, or length consumed
static size_t parse_hdr(dns_hdr_t *hdr, const char *ptr, size_t len){
    const uint8_t *uptr = (uint8_t*)ptr;

    if(len < 12) return BAD_PARSE;

    hdr->id = (uptr[0] << 8) | uptr[1];
    hdr->qr = (0x80 & uptr[2]) >> 7;
    hdr->opcode = (0x78 & uptr[2]) >> 3;
    hdr->aa = (0x04 & uptr[2]) >> 2;
    hdr->tc = (0x02 & uptr[2]) >> 1;
    hdr->rd = (0x01 & uptr[2]);
    hdr->ra = (0x80 & uptr[3]) >> 7;
    hdr->z = (0x70 & uptr[3]) >> 4;
    hdr->rcode = (0x0f & uptr[3]);
    hdr->qdcount = (uptr[4] << 8) | uptr[5];
    hdr->ancount = (uptr[6] << 8) | uptr[7];
    hdr->nscount = (uptr[8] << 8) | uptr[9];
    hdr->arcount = (uptr[10] << 8) | uptr[11];

    // TODO: validate some of these values

    return 12;
}

static void print_hdr(const dns_hdr_t hdr){
    printf("id=%x ", (unsigned)hdr.id);
    printf("qr=%u ", (unsigned)hdr.qr);
    printf("opcode=%u ", (unsigned)hdr.opcode);
    printf("aa=%u ", (unsigned)hdr.aa);
    printf("tc=%u ", (unsigned)hdr.tc);
    printf("rd=%u ", (unsigned)hdr.rd);
    printf("ra=%u ", (unsigned)hdr.ra);
    printf("z=%u ", (unsigned)hdr.z);
    printf("rcode=%u ", (unsigned)hdr.rcode);
    printf("qdcount=%u ", (unsigned)hdr.qdcount);
    printf("ancount=%u ", (unsigned)hdr.ancount);
    printf("nscount=%u ", (unsigned)hdr.nscount);
    printf("arcount=%u ", (unsigned)hdr.arcount);
}

static size_t parse_qstn(
    dns_qstn_t *qstn, uint16_t qdcount, const char *ptr, size_t len
){
    const uint8_t *uptr = (uint8_t*)ptr;
    size_t used = 0;

    // read qdcount qnames
    for(uint16_t i = 0; i < qdcount; i++){
        // read labels until the zero label
        uint8_t l = 0;
        do {
            // read the length byte
            if(len - used < 1) return BAD_PARSE;
            l = uptr[used++];
            if(len - used < l) return BAD_PARSE;
            // we don't actually process the labels yet
            used += l;
        } while(l);
    }
    if(len - used < 4) return BAD_PARSE;
    qstn->qtype = (uptr[used+0] << 8) | uptr[used+1];
    qstn->qclass = (uptr[used+2] << 8) | uptr[used+3];
    used += 4;

    qstn->ptr = ptr;
    qstn->len = used;
    qstn->qdcount = qdcount;

    // TODO: is there an in-band response we should send instead?
    // if(qtype_tostr(qstn->qtype) == UNKNOWN) return BAD_PARSE;
    // if(qtype_tostr(qstn->qclass) == UNKNOWN) return BAD_PARSE;

    return used;
}

static void print_qstn(const dns_qstn_t qstn){
    uint8_t *uptr = (uint8_t*)qstn.ptr;
    // print each label
    size_t used = 0;
    for(uint16_t i = 0; i < qstn.qdcount; i++){
        // read labels until the zero label
        printf("\nqname=");
        while(true){
            // read the length byte
            int l = uptr[used++];
            if(l == 0) break;
            printf("%.*s.", l, qstn.ptr + used);
            used += l;
        }
    }
    printf("\nqtype=%u", (unsigned)qstn.qtype);
    printf("\nqclass=%u", (unsigned)qstn.qclass);
}


// returns true if ok
bool parse_pkt(dns_pkt_t *pkt, const char *ptr, size_t len){
    size_t used = 0;
    *pkt = (dns_pkt_t){0};

    size_t zret;

    zret = parse_hdr(&pkt->hdr, ptr + used, len - used);
    if(zret == BAD_PARSE) return false;
    used += zret;

    zret = parse_qstn(&pkt->qstn, pkt->hdr.qdcount, ptr + used, len - used);
    if(zret == BAD_PARSE) return false;
    used += zret;

    return true;
};

void print_pkt(const dns_pkt_t pkt){
    print_hdr(pkt.hdr);
    print_qstn(pkt.qstn);
    printf("\n");
}
