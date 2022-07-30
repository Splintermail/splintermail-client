#include <string.h>

#include <uv.h>

#include "libdstr/libdstr.h"
#include "server/dns/dns.h"

char *UNKNOWN = "unknown";

// returns -1 on failure
int parse_qtype(const char *qtype){
    // handle rfc1035 qtypes
    if(strcmp(qtype, "A") == 0) return 1;
    if(strcmp(qtype, "NS") == 0) return 2;
    if(strcmp(qtype, "MD") == 0) return 3;
    if(strcmp(qtype, "MF") == 0) return 4;
    if(strcmp(qtype, "CNAME") == 0) return 5;
    if(strcmp(qtype, "SOA") == 0) return 6;
    if(strcmp(qtype, "MB") == 0) return 7;
    if(strcmp(qtype, "MG") == 0) return 8;
    if(strcmp(qtype, "MR") == 0) return 9;
    if(strcmp(qtype, "NULL") == 0) return 10;
    if(strcmp(qtype, "WKS") == 0) return 11;
    if(strcmp(qtype, "PTR") == 0) return 12;
    if(strcmp(qtype, "HINFO") == 0) return 13;
    if(strcmp(qtype, "MINFO") == 0) return 14;
    if(strcmp(qtype, "MX") == 0) return 15;
    if(strcmp(qtype, "TXT") == 0) return 16;
    if(strcmp(qtype, "AXFR") == 0) return 252;
    if(strcmp(qtype, "MAILB") == 0) return 253;
    if(strcmp(qtype, "MAILA") == 0) return 254;
    if(strcmp(qtype, "*") == 0) return 255;
    // add an alias of "*"
    if(strcmp(qtype, "ANY") == 0) return 255;
    // also handle AAAA
    if(strcmp(qtype, "AAAA") == 0) return 28;
    return -1;
}

// returns UNKNOWN on error
const char *qtype_tostr(int qtype){
    switch(qtype){
        // handle rfc1035 qtypes
        case 1: return "A";
        case 2: return "NS";
        case 3: return "MD";
        case 4: return "MF";
        case 5: return "CNAME";
        case 6: return "SOA";
        case 7: return "MB";
        case 8: return "MG";
        case 9: return "MR";
        case 10: return "NULL";
        case 11: return "WKS";
        case 12: return "PTR";
        case 13: return "HINFO";
        case 14: return "MINFO";
        case 15: return "MX";
        case 16: return "TXT";
        case 252: return "AXFR";
        case 253: return "MAILB";
        case 254: return "MAILA";
        case 255: return "*";

        // also handle AAAA
        case 28: return "AAAA";

        default: return UNKNOWN;
    }
}

// returns BAD_PARSE on invalid input, or updated `used`
static size_t parse_hdr(
    dns_hdr_t *hdr, const char *ptr, size_t len, size_t used
){
    if(used == BAD_PARSE) return BAD_PARSE;
    const uint8_t *uptr = (uint8_t*)ptr;
    uptr += used;

    if(len < used + 12) return BAD_PARSE;

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

    return used + 12;
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
    printf("\n");
}

// read labels until the zero label
// fail if any label exceeds 63 characters
// fail if length exceeds 255 characters
// allow pointers, but not double-pointers
// return the number of bytes used until zero label or first pointer
static size_t parse_name(const char *ptr, size_t len, size_t used){
    if(used == BAD_PARSE) return BAD_PARSE;
    const uint8_t *uptr = (uint8_t*)ptr;
    bool after_ptr = false;
    bool was_ptr = false;
    size_t preptr_used = 0;
    size_t namelen = 0;
    uint8_t l = 0;
    do {
        // read the length byte
        if(len < used + 1) return BAD_PARSE;
        l = uptr[used++];
        if((l & 0xC0) == 0xC0){
            // pointer
            if(was_ptr) return BAD_PARSE;  // double pointer
            was_ptr = true;
            if(!after_ptr){
                after_ptr = true;
                preptr_used = used;
            }
            used = l & ~0xC0;
        }else{
            // regular label
            was_ptr = false;
            if(l > 63) return BAD_PARSE;
            if(len < used + l) return BAD_PARSE;
            used += l;
            // namelen limit includes the length bytes
            namelen += l + 1;
            if(namelen > 255) return BAD_PARSE;
        }
    } while(l);

    return after_ptr ? preptr_used : used;
}

lstr_t *labels_iter(labels_t *it, const char *ptr, size_t start){
    *it = (labels_t){
        .ptr = ptr,
        .pos = start,
    };
    return labels_next(it);
}

lstr_t *labels_next(labels_t *it){
    const uint8_t *uptr = (uint8_t*)it->ptr;
    while(true){
        uint8_t l = uptr[it->pos++];
        if(l == 0){
            if(!it->after_ptr) it->used = it->pos;
            return NULL;
        }
        if((l & 0xC0) == 0xC0){
            // follow pointer
            if(!it->after_ptr){
                it->after_ptr = true;
                it->used = it->pos;
            }
            it->pos = l & ~0xC0;
            continue;
        }
        it->lstr = (lstr_t){
            .len = l,
            .str = it->ptr + it->pos,
        };
        it->pos += l;
        return &it->lstr;
    }
}

static size_t parse_qstn(
    dns_qstn_t *qstn,
    uint16_t qdcount,
    const char *ptr,
    size_t len,
    size_t used
){
    if(used == BAD_PARSE) return BAD_PARSE;
    const uint8_t *uptr = (uint8_t*)ptr;
    qstn->off = used;

    // read qdcount qnames
    for(uint16_t i = 0; i < qdcount; i++){
        used = parse_name(ptr, len, used);
        if(used == BAD_PARSE) return BAD_PARSE;
    }
    if(len < used + 4) return BAD_PARSE;
    qstn->qtype = (uptr[used+0] << 8) | uptr[used+1];
    qstn->qclass = (uptr[used+2] << 8) | uptr[used+3];
    used += 4;

    qstn->ptr = ptr;
    qstn->len = used - qstn->off;
    qstn->qdcount = qdcount;

    // TODO: is there an in-band response we should send instead?
    if(qtype_tostr(qstn->qtype) == UNKNOWN) return BAD_PARSE;
    if(qtype_tostr(qstn->qclass) == UNKNOWN) return BAD_PARSE;

    return used;
}

static void print_qstn(const dns_qstn_t qstn){
    // print each label
    size_t used = qstn.off;
    for(uint16_t i = 0; i < qstn.qdcount; i++){
        printf("qname=");
        labels_t labels;
        lstr_t *lstr = labels_iter(&labels, qstn.ptr, used);
        for(; lstr; lstr = labels_next(&labels)){
            printf("%.*s.", (int)lstr->len, lstr->str);
        }
        used = labels.used;
    }
    printf("\nqtype=%u", (unsigned)qstn.qtype);
    printf("\nqclass=%u", (unsigned)qstn.qclass);
}

// really just validation; we re-parse it on the fly.
static size_t parse_rr(
    dns_rr_t *rr, uint16_t count, const char *ptr, size_t len, size_t used
){
    if(used == BAD_PARSE) return BAD_PARSE;
    const uint8_t *uptr = (uint8_t*)ptr;
    rr->ptr = ptr;
    rr->off = used;
    rr->count = count;

    for(uint16_t i = 0; i < count; i++){
        used = parse_name(ptr, len, used);
        if(used == BAD_PARSE) return BAD_PARSE;
        if(len < used + 10) return BAD_PARSE;
        uint16_t type = (uptr[used+0] << 8) | uptr[used+1];
        uint16_t class = (uptr[used+2] << 8) | uptr[used+3];
        uint32_t ttl = (uptr[used+4] << 24)
                     | (uptr[used+5] << 16)
                     | (uptr[used+6] << 8)
                     |  uptr[used+7];
        uint16_t rdlen = (uptr[used+8] << 8) | uptr[used+9];
        // TODO: check type, class, and rdlen
        (void)type; (void)class; (void)ttl;
        used += 10;
        if(len < used + rdlen) return BAD_PARSE;
        // TODO: parse/validate the rdata as well
        used += rdlen;
    }

    rr->len = used - rr->off;

    return used;
}

static void print_rr(const dns_rr_t rr){
    const uint8_t *uptr = (uint8_t*)rr.ptr;
    size_t used = rr.off;

    for(uint16_t i = 0; i < rr.count; i++){
        labels_t labels = {0};
        lstr_t *lstr = labels_iter(&labels, rr.ptr, used);
        printf("name=");
        for(; lstr; lstr = labels_next(&labels)){
            printf("%.*s.", (int)lstr->len, lstr->str);
        }
        used = labels.used;
        unsigned type = (uptr[used+0] << 8) | uptr[used+1];
        unsigned class = (uptr[used+2] << 8) | uptr[used+3];
        unsigned ttl = (uptr[used+4] << 24)
                     | (uptr[used+5] << 16)
                     | (uptr[used+6] << 8)
                     |  uptr[used+7];
        unsigned rdlen = (uptr[used+8] << 8) | uptr[used+9];
        used += 10;
        printf(" type=%u", type);
        printf(" class=%u", class);
        printf(" ttl=%u", ttl);
        printf(" rdlen=%u\n", rdlen);
        print_bytes(rr.ptr + used, rdlen);
        used += rdlen;
    }
}

// returns 0 if ok, or BAD_PARSE if not ok
size_t parse_pkt(dns_pkt_t *pkt, const char *ptr, size_t len){
    size_t used = 0;
    *pkt = (dns_pkt_t){0};

    used = parse_hdr(&pkt->hdr, ptr, len, used);
    used = parse_qstn(&pkt->qstn, pkt->hdr.qdcount, ptr, len, used);
    used = parse_rr(&pkt->ans, pkt->hdr.ancount, ptr, len, used);
    used = parse_rr(&pkt->auth, pkt->hdr.nscount, ptr, len, used);
    used = parse_rr(&pkt->addl, pkt->hdr.arcount, ptr, len, used);
    if(used == BAD_PARSE) return BAD_PARSE;

    return 0;
};

void print_pkt(const dns_pkt_t pkt){
    print_hdr(pkt.hdr);
    print_qstn(pkt.qstn);

    printf("\nANSWER ");
    print_rr(pkt.ans);
    printf("\nAUTHORITY ");
    print_rr(pkt.auth);
    printf("\nADDITIONAL ");
    print_rr(pkt.addl);
    printf("\n");
}
