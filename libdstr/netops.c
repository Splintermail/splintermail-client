#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>

#include "libdstr.h"

derr_t dsock(int domain, int type, int protocol, int *fd){
    derr_t e = E_OK;

    *fd = socket(domain, type, protocol);
    if(*fd < 0){
        TRACE(&e, "socket: %x\n", FE(&errno));
        ORIG(&e, E_OS, "socket failed");
    }

    return e;
}

#ifdef _WIN32

static derr_t dsockaddr_un(
    const dstr_t *path,
    struct sockaddr_storage storage,
    struct sockaddr **sa
){
    derr_t e = E_OK;

    *storage = (struct sockaddr_storage){0};

    *sa = &storage;

    sa.sun_family = AF_UNIX;

    DSTR_WRAP(sockpath, (*sa)->sun_path);
    PROP(&e, FMT(&sockpath, "%x", FD(&path)) );

    return e;
}

derr_t dconnect_unix(int sockfd, const dstr_t *path){
    derr_t e = E_OK;

    struct sockaddr_storage storage;
    struct sockaddr_un *sa;
    PROP(&e, d_sockaddr_un(path, &storage, &sa) );

    int ret = connect(sockfd, sa, sizeof(storage));
    if(ret){
        TRACE(&e, "connect(%x): %x\n", FS(path), FE(&errno));
        ORIG(&e, E_OS, "connect failed");
    }

    return e;
}

#endif // _WIN32
