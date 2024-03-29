#include <string.h>
#include <errno.h>

#include "libdstr.h"

DEF_CONTAINER_OF(_fmt_ntop_t, iface, fmt_i)
DEF_CONTAINER_OF(_fmt_ntops_t, iface, fmt_i)
DEF_CONTAINER_OF(_fmt_ntop4_t, iface, fmt_i)
DEF_CONTAINER_OF(_fmt_ntop6_t, iface, fmt_i)

static derr_type_t do_ntop4(const struct sockaddr_in *sin, writer_i *out){
    char buf[INET_ADDRSTRLEN];
    const char *ret = inet_ntop(
        sin->sin_family, &sin->sin_addr, buf, (socklen_t)sizeof(buf)
    );
    if(!ret) return E_OS;

    return out->w->puts(out, buf, strnlen(buf, sizeof(buf)));
}
static derr_type_t do_ntop6(const struct sockaddr_in6 *sin6, writer_i *out){
    char buf[INET6_ADDRSTRLEN];
    const char *ret = inet_ntop(
        sin6->sin6_family, &sin6->sin6_addr, buf, (socklen_t)sizeof(buf)
    );
    if(!ret) return E_OS;

    return out->w->puts(out, buf, strnlen(buf, sizeof(buf)));
}

derr_type_t _fmt_ntop(const fmt_i *iface, writer_i *out){
    const struct sockaddr *sa = CONTAINER_OF(iface, _fmt_ntop_t, iface)->sa;
    if(sa->sa_family == AF_INET){
        return do_ntop4((const struct sockaddr_in*)sa, out);
    }else if(sa->sa_family == AF_INET6){
        return do_ntop6((const struct sockaddr_in6*)sa, out);
    }
    return E_PARAM;
}

derr_type_t _fmt_ntops(const fmt_i *iface, writer_i *out){
    const struct sockaddr_storage *ss =
        CONTAINER_OF(iface, _fmt_ntops_t, iface)->ss;
    if(ss->ss_family == AF_INET){
        return do_ntop4((const struct sockaddr_in*)ss, out);
    }else if(ss->ss_family == AF_INET6){
        return do_ntop6((const struct sockaddr_in6*)ss, out);
    }
    return E_PARAM;
}

derr_type_t _fmt_ntop4(const fmt_i *iface, writer_i *out){
    const struct sockaddr_in *sin =
        CONTAINER_OF(iface, _fmt_ntop4_t, iface)->sin;
    return do_ntop4(sin, out);
}

derr_type_t _fmt_ntop6(const fmt_i *iface, writer_i *out){
    const struct sockaddr_in6 *sin6 =
        CONTAINER_OF(iface, _fmt_ntop6_t, iface)->sin6;
    return do_ntop6(sin6, out);
}

const struct sockaddr *ss2sa(const struct sockaddr_storage *ss){
    return (const struct sockaddr*)ss;
}

uint16_t must_addr_port(const struct sockaddr *sa){
    if(sa->sa_family == AF_INET){
        return ntohs(((const struct sockaddr_in*)sa)->sin_port);
    }else if(sa->sa_family == AF_INET6){
        return ntohs(((const struct sockaddr_in6*)sa)->sin6_port);
    }
    LOG_FATAL("unhandled addr family: %x\n", FU(sa->sa_family));
}

uint16_t must_addrs_port(const struct sockaddr_storage *ss){
    return must_addr_port((const struct sockaddr*)ss);
}

derr_type_t addr_copy_quiet(
    const struct sockaddr *in, struct sockaddr_storage *ss
){
    if(in->sa_family == AF_INET){
        memcpy(ss, in, sizeof(struct sockaddr_in));
        return E_NONE;
    }else if(in->sa_family == AF_INET6){
        memcpy(ss, in, sizeof(struct sockaddr_in6));
        return E_NONE;
    }
    return E_PARAM;
}

derr_t addr_copy(const struct sockaddr *in, struct sockaddr_storage *ss){
    derr_t e = E_OK;

    derr_type_t etype = addr_copy_quiet(in, ss);
    if(etype) ORIG(&e, etype, "unhandled address in");

    return e;
}

derr_t read_addr(struct sockaddr_storage *ss, const char *addr, uint16_t port){
    derr_t e = E_OK;

    *ss = (struct sockaddr_storage){0};

    // detect ipv4 vs ipv6
    unsigned char family = AF_INET;
    size_t addrlen = strlen(addr);
    for(size_t i = 0; i < addrlen; i++){
        if(addr[i] == ':'){
            family = AF_INET6;
            break;
        }
    }

    if(family == AF_INET){
        struct sockaddr_in sin = {0};
        sin.sin_family = family;
        sin.sin_port = htons(port);
        struct in_addr in_addr;
        int ret = inet_pton(family, addr, &in_addr);
        if(ret != 1){
            TRACE(&e, "unable to parse address: %x\n", FE(errno));
            ORIG(&e, E_PARAM, "unable to parse address");
        }
        sin.sin_addr = in_addr;
        struct sockaddr_in *out = (struct sockaddr_in*)ss;
        *out = sin;
    }else{
        struct sockaddr_in6 sin6 = {0};
        sin6.sin6_family = family;
        sin6.sin6_port = htons(port);
        struct in6_addr in6_addr;
        int ret = inet_pton(family, addr, &in6_addr);
        if(ret != 1){
            TRACE(&e, "unable to parse address: %x\n", FE(errno));
            ORIG(&e, E_PARAM, "unable to parse address");
        }
        sin6.sin6_addr = in6_addr;
        struct sockaddr_in6 *out = (struct sockaddr_in6*)ss;
        *out = sin6;
    }

    return e;
}

bool addr_eq(const struct sockaddr *a, const struct sockaddr *b){
    if(a->sa_family != b->sa_family) return false;
    if(a->sa_family == AF_INET){
        const struct sockaddr_in *ain = (const struct sockaddr_in*)a;
        const struct sockaddr_in *bin = (const struct sockaddr_in*)b;
        if(ain->sin_port != bin->sin_port) return false;
        return memcmp(
            &ain->sin_addr, &bin->sin_addr, sizeof(ain->sin_addr)
        ) == 0;
    }
    if(a->sa_family == AF_INET6){
        const struct sockaddr_in6 *ain6 = (const struct sockaddr_in6*)a;
        const struct sockaddr_in6 *bin6 = (const struct sockaddr_in6*)b;
        if(ain6->sin6_port != bin6->sin6_port) return false;
        return memcmp(
            &ain6->sin6_addr, &bin6->sin6_addr, sizeof(ain6->sin6_addr)
        ) == 0;
    }
    LOG_FATAL("unhandled addr family %x\n", FI(a->sa_family));
}

bool addrs_eq(
    const struct sockaddr_storage *a, const struct sockaddr_storage *b
){
    return addr_eq((const struct sockaddr*)a, (const struct sockaddr*)b);
}
