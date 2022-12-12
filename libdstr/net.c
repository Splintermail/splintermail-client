#include <string.h>
#include <errno.h>

#include "libdstr.h"

derr_type_t fmthook_ntop4(dstr_t* out, const void* arg){
    const struct sockaddr_in *sin = arg;

    DSTR_VAR(buf, INET_ADDRSTRLEN);
    const char *ret = inet_ntop(
        sin->sin_family, &sin->sin_addr, buf.data, (socklen_t)buf.size
    );
    if(!ret) return E_OS;
    buf.len = strlen(ret);

    return fmt_dstr_append_quiet(out, &buf);
}


derr_type_t fmthook_ntop6(dstr_t* out, const void* arg){
    const struct sockaddr_in6 *sin6 = arg;

    DSTR_VAR(buf, INET6_ADDRSTRLEN);
    const char *ret = inet_ntop(
        sin6->sin6_family, &sin6->sin6_addr, buf.data, (socklen_t)buf.size
    );
    if(!ret) return E_OS;
    buf.len = strlen(ret);

    return fmt_dstr_append_quiet(out, &buf);
}


derr_type_t fmthook_ntop(dstr_t* out, const void* arg){
    const struct sockaddr *sa = arg;

    if(sa->sa_family == AF_INET){
        return fmthook_ntop4(out, (const struct sockaddr_in*)sa);
    }else if(sa->sa_family == AF_INET6){
        return fmthook_ntop4(out, (const struct sockaddr_in6*)sa);
    }
    return E_PARAM;
}

uint16_t addr_port(const struct sockaddr *sa){
    if(sa->sa_family == AF_INET){
        return ntohs(((const struct sockaddr_in*)sa)->sin_port);
    }else if(sa->sa_family == AF_INET6){
        return ntohs(((const struct sockaddr_in6*)sa)->sin6_port);
    }
    return 0;
}

uint16_t addrs_port(const struct sockaddr_storage *ss){
    return addr_port((const struct sockaddr*)ss);
}

derr_t read_addr(struct sockaddr_storage *ss, const char *addr, uint16_t port){
    derr_t e = E_OK;

    *ss = (struct sockaddr_storage){0};

    // detect ipv4 vs ipv6
    sa_family_t family = AF_INET;
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
            TRACE(&e, "unable to parse address: %x\n", FE(&errno));
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
            TRACE(&e, "unable to parse address: %x\n", FE(&errno));
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
        struct sockaddr_in *ain = (struct sockaddr_in*)a;
        struct sockaddr_in *bin = (struct sockaddr_in*)b;
        if(ain->sin_port != bin->sin_port) return false;
        return memcmp(
            &ain->sin_addr, &bin->sin_addr, sizeof(ain->sin_addr)
        ) == 0;
    }
    if(a->sa_family == AF_INET6){
        struct sockaddr_in6 *ain6 = (struct sockaddr_in6*)a;
        struct sockaddr_in6 *bin6 = (struct sockaddr_in6*)b;
        if(ain6->sin6_port != bin6->sin6_port) return false;
        return memcmp(
            &ain6->sin6_addr, &bin6->sin6_addr, sizeof(ain6->sin6_addr)
        ) == 0;
    }
    LOG_FATAL("unhandled addr family %x\n", FI(a->sa_family));
    return false;
}

bool addrs_eq(
    const struct sockaddr_storage *a, const struct sockaddr_storage *b
){
    return addr_eq((struct sockaddr*)a, (struct sockaddr*)b);
}
