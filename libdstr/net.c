#include "libdstr.h"

derr_type_t fmthook_ntop4(dstr_t* out, const void* arg){
    const struct sockaddr_in *sin = arg;

    DSTR_VAR(buf, INET_ADDRSTRLEN);
    const char *ret = inet_ntop(
        sin->sin_family, &sin->sin_addr, buf.data, (socklen_t)buf.size
    );
    if(!ret) return E_OS;

    return fmt_dstr_append_quiet(out, &buf);
}


derr_type_t fmthook_ntop6(dstr_t* out, const void* arg){
    const struct sockaddr_in6 *sin6 = arg;

    DSTR_VAR(buf, INET6_ADDRSTRLEN);
    const char *ret = inet_ntop(
        sin6->sin6_family, &sin6->sin6_addr, buf.data, (socklen_t)buf.size
    );
    if(!ret) return E_OS;

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
