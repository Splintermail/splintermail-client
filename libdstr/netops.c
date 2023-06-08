#include <sys/types.h>
#include <errno.h>

#include "libdstr.h"

#ifndef _WIN32
#include <sys/un.h>

derr_t dsock(int domain, int type, int protocol, int *fd){
    derr_t e = E_OK;

    *fd = socket(domain, type, protocol);
    if(*fd < 0){
        TRACE(&e, "socket: %x\n", FE(errno));
        ORIG(&e, E_OS, "socket failed");
    }

    return e;
}

static derr_t dsockaddr_un(
    const dstr_t *path,
    struct sockaddr_storage *storage,
    struct sockaddr **sa
){
    derr_t e = E_OK;

    // zeroize the backing memory
    *storage = (struct sockaddr_storage){0};

    // reference backing memory as unix-type sockaddr
    struct sockaddr_un *sun = (struct sockaddr_un*)storage;

    sun->sun_family = AF_UNIX;

    dstr_t sockpath;
    DSTR_WRAP_ARRAY(sockpath, sun->sun_path);
    PROP(&e, FMT(&sockpath, "%x", FD(*path)) );

    // return backing memory as sockaddr
    *sa = (struct sockaddr*)storage;

    return e;
}

derr_t dconnect_unix(int sockfd, const dstr_t *path){
    derr_t e = E_OK;

    struct sockaddr_storage storage;
    struct sockaddr *sa;
    PROP(&e, dsockaddr_un(path, &storage, &sa) );

    int ret = connect(sockfd, sa, sizeof(storage));
    if(ret){
        TRACE(&e, "connect(%x): %x\n", FD(*path), FE(errno));
        ORIG(&e, E_OS, "connect failed");
    }

    return e;
}

#endif // _WIN32
