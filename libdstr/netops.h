// derr_t-wrapped system calls

/* mostly meant to be useful in unix; not meant to be a cross-platform
   networking library; those already exist */

derr_t dsock(int domain, int type, int protocol, int *fd);

#ifndef _WIN32

derr_t dconnect_unix(int sockfd, const dstr_t *path);

#endif // _WIN32
