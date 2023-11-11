#include <string.h>

#include <uv.h>

#include "libdstr/libdstr.h"
#include "server/dns/libdns.h"

DEF_CONTAINER_OF(_fmt_lstr_t, iface, fmt_i)

derr_type_t _fmt_lstr(const fmt_i *iface, writer_i *out){
    lstr_t l = CONTAINER_OF(iface, _fmt_lstr_t, iface)->l;
    return out->w->puts(out, l.str, l.len);
}

derr_type_t _fmt_lstr_dbg(const fmt_i *iface, writer_i *out){
    lstr_t l = CONTAINER_OF(iface, _fmt_lstr_t, iface)->l;
    return fmt_strn_dbg(l.str, l.len, out);
}

static derr_type_t fmt_bytes(writer_i *out, const char *bytes, size_t len){
    derr_type_t etype;
    writer_t w = *out->w;
    etype = w.puts(out, "  ", 2);
    if(etype) return etype;
    for(size_t i = 0; i < len; i++){
        if(i == 0){
        }else if(i > 0 && i%16 == 0){
            etype = w.puts(out, "\n  ", 3);
            if(etype) return etype;
        }else if(i%4 == 0){
            etype = w.puts(out, "  ", 2);
            if(etype) return etype;
        }else{
            etype = w.putc(out, ' ');
            if(etype) return etype;
        }
        char buf[3];
        unsigned char c = ((unsigned char*)bytes)[i];
        snprintf(buf, sizeof(buf), "%.2x", (unsigned int)c);
        etype = w.puts(out, buf, 2);
        if(etype) return etype;
    }
    return E_NONE;
}

bool is_bad_parse(size_t n){
    return n >= BP_MINVAL;
}

const char *bp2str(bad_parse_e bp){
    switch(bp){
        case BP_ENDS_OPT_DATA_OVERRUN: return "BP_ENDS_OPT_DATA_OVERRUN";
        case BP_ENDS_OPT_HDR_OVERRUN: return "BP_ENDS_OPT_HDR_OVERRUN";
        case BP_ENDS_NONEMPTY_NAME: return "BP_ENDS_NONEMPTY_NAME";
        case BP_DOUBLE_EDNS: return "BP_DOUBLE_EDNS";
        case BP_RR_DATA_OVERRUN: return "BP_RR_DATA_OVERRUN";
        case BP_RR_HDR_OVERRUN: return "BP_RR_HDR_OVERRUN";
        case BP_QSTN_OVERRUN: return "BP_QSTN_OVERRUN";
        case BP_LONG_NAME: return "BP_LONG_NAME";
        case BP_LABEL_OVERRUN: return "BP_LABEL_OVERRUN";
        case BP_LONG_LABEL: return "BP_LONG_LABEL";
        case BP_DOUBLE_POINTER: return "BP_DOUBLE_POINTER";
        case BP_PTR_LEN_BYTE: return "BP_PTR_LEN_BYTE";
        case BP_LABEL_LEN_BYTE: return "BP_LABEL_LEN_BYTE";
        case BP_HDR_OVERRUN: return "BP_HDR_OVERRUN";
    }
    return "parse was ok";
}

bool lstr_ieq(const lstr_t a, const lstr_t b){
    return a.len == b.len && strncasecmp(a.str, b.str, a.len) == 0;
}

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

// returns bad_parse_e on invalid input, or updated `used`
static size_t parse_hdr(
    dns_hdr_t *hdr, const char *ptr, size_t len, size_t used
){
    if(is_bad_parse(used)) return used;
    const uint8_t *uptr = (uint8_t*)ptr;
    uptr += used;

    if(len < used + 12) return BP_HDR_OVERRUN;

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

    return used + 12;
}

static derr_type_t fmt_hdr(writer_i *out, const dns_hdr_t hdr){
    derr_type_t etype;
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%x", (unsigned)hdr.id);
    if(len < 0 || len > (int)sizeof(buf)) return E_INTERNAL;
    etype = FMT_UNLOCKED(out, "id=%x ", FSN(buf, (size_t)len));
    if(etype) return etype;
    etype = FMT_UNLOCKED(out, "qr=%x ", FU(hdr.qr));
    if(etype) return etype;
    etype = FMT_UNLOCKED(out, "opcode=%x ", FU(hdr.opcode));
    if(etype) return etype;
    etype = FMT_UNLOCKED(out, "aa=%x ", FU(hdr.aa));
    if(etype) return etype;
    etype = FMT_UNLOCKED(out, "tc=%x ", FU(hdr.tc));
    if(etype) return etype;
    etype = FMT_UNLOCKED(out, "rd=%x ", FU(hdr.rd));
    if(etype) return etype;
    etype = FMT_UNLOCKED(out, "ra=%x ", FU(hdr.ra));
    if(etype) return etype;
    etype = FMT_UNLOCKED(out, "z=%x ", FU(hdr.z));
    if(etype) return etype;
    etype = FMT_UNLOCKED(out, "rcode=%x ", FU(hdr.rcode));
    if(etype) return etype;
    etype = FMT_UNLOCKED(out, "qdcount=%x ", FU(hdr.qdcount));
    if(etype) return etype;
    etype = FMT_UNLOCKED(out, "ancount=%x ", FU(hdr.ancount));
    if(etype) return etype;
    etype = FMT_UNLOCKED(out, "nscount=%x ", FU(hdr.nscount));
    if(etype) return etype;
    etype = FMT_UNLOCKED(out, "arcount=%x\n", FU(hdr.arcount));
    if(etype) return etype;
    return E_NONE;
}

// read labels until the zero label
// fail if any label exceeds 63 characters
// fail if length exceeds 255 characters
// allow pointers, but not double-pointers
// return the number of bytes used until zero label or first pointer
size_t parse_name(const char *ptr, size_t len, size_t used){
    if(is_bad_parse(used)) return used;
    const uint8_t *uptr = (uint8_t*)ptr;
    bool after_ptr = false;
    bool was_ptr = false;
    size_t preptr_used = 0;
    size_t namelen = 0;
    uint8_t l = 0;
    do {
        // read the length byte
        if(len < used + 1) return BP_LABEL_LEN_BYTE;
        l = uptr[used++];
        if((l & 0xC0) == 0xC0){
            // pointer
            if(was_ptr) return BP_DOUBLE_POINTER;
            was_ptr = true;
            if(len < used + 1) return BP_PTR_LEN_BYTE;
            uint8_t l2 = uptr[used++];
            if(!after_ptr){
                after_ptr = true;
                preptr_used = used;
            }
            used = ((l & ~0xC0) << 8) | l2;
        }else{
            // regular label
            was_ptr = false;
            if(l > 63) return BP_LONG_LABEL;
            if(len < used + l) return BP_LABEL_OVERRUN;
            used += l;
            // namelen limit includes the length bytes
            namelen += l + 1;
            if(namelen > 255) return BP_LONG_NAME;
        }
    } while(l);

    return after_ptr ? preptr_used : used;
}

// don't care about the content, just skip it
size_t skip_name(const char *ptr, size_t used){
    const uint8_t *uptr = (uint8_t*)ptr;
    uint8_t l = 0;
    do {
        // read the length byte
        l = uptr[used++];
        if((l & 0xC0) == 0xC0){
            // pointer
            return used + 1;
        }else{
            // regular label
            used += l;
        }
    } while(l);

    return used;
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
            uint8_t l2 = uptr[it->pos++];
            // follow pointer
            if(!it->after_ptr){
                it->after_ptr = true;
                it->used = it->pos;
            }
            it->pos = ((l & ~0xC0) << 8) | l2;
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

void labels_reverse(lstr_t *lstrs, size_t n){
    // operate on 0 to n/2; if n is odd the middle is untouched
    size_t end = n/2;
    for(size_t i = 0; i < end; i++){
        lstr_t temp = lstrs[i];
        lstrs[i] = lstrs[n-i-1];
        lstrs[n-i-1] = temp;
    }
}

// returns size_t nlabels; nlabels > cap indicates an error
size_t labels_read(const char *ptr, size_t start, lstr_t *lstrs, size_t cap){
    labels_t it;
    size_t n = 0;
    for(lstr_t *l = labels_iter(&it, ptr, start); l; l = labels_next(&it)){
        if(n == cap) return cap+1;
        lstrs[n++] = *l;
    }
    return n;
}

size_t labels_read_reverse(
    const char *ptr, size_t start, lstr_t *lstrs, size_t cap
){
    size_t n = labels_read(ptr, start, lstrs, cap);
    labels_reverse(lstrs, n);
    return n;
}

static size_t parse_qstn(
    dns_qstn_t *qstn,
    uint16_t qdcount,
    const char *ptr,
    size_t len,
    size_t used
){
    if(is_bad_parse(used)) return used;
    const uint8_t *uptr = (uint8_t*)ptr;
    qstn->off = used;

    // read qdcount qnames
    for(uint16_t i = 0; i < qdcount; i++){
        used = parse_name(ptr, len, used);
        if(is_bad_parse(used)) return used;
    }
    if(len < used + 4) return BP_QSTN_OVERRUN;
    qstn->qtype = (uptr[used+0] << 8) | uptr[used+1];
    qstn->qclass = (uptr[used+2] << 8) | uptr[used+3];
    used += 4;

    qstn->ptr = ptr;
    qstn->len = used - qstn->off;
    qstn->qdcount = qdcount;

    return used;
}

static derr_type_t fmt_qstn(writer_i *out, const dns_qstn_t qstn){
    derr_type_t etype;
    writer_t w = *out->w;
    // print each label
    size_t used = qstn.off;
    for(uint16_t i = 0; i < qstn.qdcount; i++){
        etype = w.puts(out, "qname=", 6);
        if(etype) return etype;
        labels_t labels;
        lstr_t *lstr = labels_iter(&labels, qstn.ptr, used);
        bool first = true;
        for(; lstr; lstr = labels_next(&labels)){
            if(first){
                first = false;
            }else{
                w.putc(out, '.');
            }
            etype = w.puts(out, lstr->str, lstr->len);
            if(etype) return etype;
        }
        used = labels.used;
    }
    const char *qtypestr = qtype_tostr(qstn.qtype);
    fmt_i *fqtype = qtypestr == UNKNOWN ? FU(qstn.qtype) : FS(qtypestr);
    etype = FMT_UNLOCKED(out, "\nqtype=%x", fqtype);
    if(etype) return etype;
    etype = FMT_UNLOCKED(out, "\nqclass=%x", FU(qstn.qclass));
    if(etype) return etype;
    return E_NONE;
}

// really just validation; we re-parse it on the fly.
static size_t parse_rr(
    dns_rr_t *rr, uint16_t count, const char *ptr, size_t len, size_t used
){
    if(is_bad_parse(used)) return used;
    const uint8_t *uptr = (uint8_t*)ptr;
    rr->ptr = ptr;
    rr->off = used;
    rr->count = count;

    for(uint16_t i = 0; i < count; i++){
        used = parse_name(ptr, len, used);
        if(is_bad_parse(used)) return used;
        if(len < used + 10) return BP_RR_HDR_OVERRUN;
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
        if(len < used + rdlen) return BP_RR_DATA_OVERRUN;
        // TODO: parse/validate the rdata as well
        used += rdlen;
    }

    rr->len = used - rr->off;

    return used;
}

rr_t *dns_rr_iter(rrs_t *it, const dns_rr_t dns_rr){
    return rrs_iter(it, dns_rr.ptr, dns_rr.off, dns_rr.count);
}

rr_t *rrs_iter(rrs_t *it, const char *ptr, size_t start, size_t count){
    *it = (rrs_t){
        .ptr = ptr,
        .pos = start,
        .count = count,
    };
    return rrs_next(it);
}

rr_t *rrs_next(rrs_t *it){
    if(it->seen == it->count) return NULL;

    const uint8_t *uptr = (uint8_t*)it->ptr;
    size_t used = it->pos;

    rr_t rr = { .ptr = it->ptr };

    rr.nameoff = used;
    used = skip_name(it->ptr, used);
    rr.type = (uptr[used+0] << 8) | uptr[used+1];
    rr.class = (uptr[used+2] << 8) | uptr[used+3];
    rr.ttl = (uptr[used+4] << 24)
                 | (uptr[used+5] << 16)
                 | (uptr[used+6] << 8)
                 |  uptr[used+7];
    rr.rdlen = (uptr[used+8] << 8) | uptr[used+9];
    used += 10;
    rr.rdoff = used;

    it->rr = rr;
    it->pos = used;
    it->seen++;

    return &it->rr;
}


static derr_type_t fmt_rr(writer_i *out, const dns_rr_t dns_rr){
    derr_type_t etype;
    writer_t w = *out->w;
    rrs_t it;
    for(rr_t *rr = dns_rr_iter(&it, dns_rr); rr; rr = rrs_next(&it)){
        etype = w.puts(out, "\n  name=", 8);
        if(etype) return etype;
        labels_t labels = {0};
        lstr_t *lstr = labels_iter(&labels, rr->ptr, rr->nameoff);
        for(; lstr; lstr = labels_next(&labels)){
            etype = w.puts(out, lstr->str, lstr->len);
            if(etype) return etype;
        }
        etype = FMT_UNLOCKED(out, " type=%x", FU(rr->type));
        if(etype) return etype;
        etype = FMT_UNLOCKED(out, " class=%x", FU(rr->class));
        if(etype) return etype;
        etype = FMT_UNLOCKED(out, " ttl=%x", FU(rr->ttl));
        if(etype) return etype;
        etype = FMT_UNLOCKED(out, " rdlen=%x", FU(rr->rdlen));
        if(etype) return etype;
        etype = fmt_bytes(out, rr->ptr + rr->rdoff, rr->rdlen);
        if(etype) return etype;
    }
    return E_NONE;
}

// returns 0 if not found, 1 if found, BP_DOUBLE_EDNS if more than one found
static size_t find_edns(rr_t *edns, const dns_rr_t addl){
    *edns = (rr_t){0};
    rrs_t it;
    bool found = false;
    for(rr_t *rr = dns_rr_iter(&it, addl); rr; rr = rrs_next(&it)){
        if(rr->type != EDNS) continue;
        if(found) return BP_DOUBLE_EDNS;
        found = true;
        *edns = *rr;
    }
    return found;
}

// returns 0 if ok, bad_parse_e if not ok
static size_t parse_edns(edns_t *edns, const rr_t rr){
    *edns = (edns_t){ .found = true, .ptr = rr.ptr };
    // rr.name must be empty
    if(rr.ptr[rr.nameoff] != '\0') return BP_ENDS_NONEMPTY_NAME;
    edns->udp_size = rr.class;
    edns->extrcode = (rr.ttl >> 24) & 0xff;
    edns->version = (rr.ttl >> 16) & 0xff;
    edns->dnssec_ok = rr.ttl & 0x8000;
    edns->z = rr.ttl & 0x7fff;
    edns->optoff = rr.rdoff;
    size_t used = rr.rdoff;
    size_t len = rr.rdoff + rr.rdlen;
    const uint8_t *uptr = (uint8_t*)rr.ptr;
    while(used < len){
        if(len < used + 4) return BP_ENDS_OPT_HDR_OVERRUN;
        // uint16_t optcode = (uptr[used+0] << 8) | uptr[used+1];
        uint16_t optlen = (uptr[used+2] << 8) | uptr[used+3];
        used += 4;
        if(len < used + optlen) return BP_ENDS_OPT_DATA_OVERRUN;
        used += optlen;
        edns->optcount++;
    }
    return 0;
}

opt_t *opts_iter(opts_t *it, const edns_t edns){
    *it = (opts_t){
        .ptr = edns.ptr,
        .pos = edns.optoff,
        .count = edns.optcount,
    };
    return opts_next(it);
}

opt_t *opts_next(opts_t *it){
    if(it->seen >= it->count) return NULL;
    const uint8_t *uptr = (uint8_t*)it->ptr;
    uint16_t optcode = (uptr[it->pos+0] << 8) | uptr[it->pos+1];
    uint16_t optlen = (uptr[it->pos+2] << 8) | uptr[it->pos+3];
    size_t off = it->pos + 4;
    it->opt = (opt_t){
        .ptr = it->ptr,
        .code = optcode,
        .len = optlen,
        .off = off,
    };

    it->pos += 4 + optlen;
    it->seen++;
    return &it->opt;
}


static derr_type_t fmt_edns(writer_i *out, const edns_t edns){
    derr_type_t etype;
    writer_t w = *out->w;
    if(!edns.found){
        return w.puts(out, "(not found)", 11);
    }
    etype = FMT_UNLOCKED(out, "extended-rcode=%x", FU(edns.extrcode));
    if(etype) return etype;
    etype = FMT_UNLOCKED(out, " version=%x", FU(edns.version));
    if(etype) return etype;
    etype = FMT_UNLOCKED(out, " dnssec_ok=%x", FU(edns.dnssec_ok));
    if(etype) return etype;
    etype = FMT_UNLOCKED(out, " z=%x", FU(edns.z));
    if(etype) return etype;
    etype = FMT_UNLOCKED(out, " udp_size=%x", FU(edns.udp_size));
    if(etype) return etype;
    etype = FMT_UNLOCKED(out, " optcount=%x", FU(edns.optcount));
    if(etype) return etype;
    opts_t opts;
    for(opt_t *opt = opts_iter(&opts, edns); opt; opt = opts_next(&opts)){
        etype = FMT_UNLOCKED(out, "\noption-code=%x:\n", FU(opt->code));
        if(etype) return etype;
        etype = fmt_bytes(out, opt->ptr + opt->off, opt->len);
        if(etype) return etype;
    }
    return E_NONE;
}

// returns 0 if ok, or bad_parse_e if not ok
size_t parse_pkt(dns_pkt_t *pkt, const char *ptr, size_t len){
    size_t used = 0;
    *pkt = (dns_pkt_t){0};

    used = parse_hdr(&pkt->hdr, ptr, len, used);
    used = parse_qstn(&pkt->qstn, pkt->hdr.qdcount, ptr, len, used);
    used = parse_rr(&pkt->ans, pkt->hdr.ancount, ptr, len, used);
    used = parse_rr(&pkt->auth, pkt->hdr.nscount, ptr, len, used);
    used = parse_rr(&pkt->addl, pkt->hdr.arcount, ptr, len, used);
    if(is_bad_parse(used)) return used;

    // detect edns
    rr_t edns;
    size_t zret = find_edns(&edns, pkt->addl);
    if(is_bad_parse(zret)) return zret;
    if(zret == 1){
        zret = parse_edns(&pkt->edns, edns);
        if(is_bad_parse(zret)) return zret;
    }

    return 0;
};

static derr_type_t fmt_pkt(writer_i *out, const dns_pkt_t pkt){
    derr_type_t etype;
    writer_t w = *out->w;

    etype = fmt_hdr(out, pkt.hdr);
    if(etype) return etype;
    etype = fmt_qstn(out, pkt.qstn);
    if(etype) return etype;

    etype = w.puts(out, "\nANSWER ", 8);
    if(etype) return etype;
    etype = fmt_rr(out, pkt.ans);
    if(etype) return etype;
    etype = w.puts(out, "\nAUTHORITY ", 11);
    if(etype) return etype;
    etype = fmt_rr(out, pkt.auth);
    if(etype) return etype;
    etype = w.puts(out, "\nADDITIONAL ", 12);
    if(etype) return etype;
    etype = fmt_rr(out, pkt.addl);
    if(etype) return etype;
    etype = w.puts(out, "\nEDNS ", 6);
    if(etype) return etype;
    etype = fmt_edns(out, pkt.edns);
    if(etype) return etype;
    etype = w.putc(out, '\n');
    if(etype) return etype;

    return E_NONE;
}

DEF_CONTAINER_OF(_fmt_pkt_t, iface, fmt_i)

derr_type_t _fmt_pkt(const fmt_i *iface, writer_i *out){
    return fmt_pkt(out, CONTAINER_OF(iface, _fmt_pkt_t, iface)->pkt);
}
