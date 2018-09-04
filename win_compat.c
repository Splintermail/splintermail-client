#ifdef _WIN32

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdarg.h>

#include <sys/stat.h>

// no point in seeing MSVC warnings in other MS's own damn code
#pragma warning(push, 0)
#include <windows.h>
#include <io.h>
#include <share.h>
#include <direct.h>
#pragma warning(pop)

#include "common.h"
#include "logger.h"

void win_perror(void){
    char buf[256];
    DWORD winerr = GetLastError();
    winerr = FormatMessageA(
                FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                buf, (sizeof(buf) / sizeof(*buf)), NULL);
    buf[sizeof(buf) - 1] = '\0';
    LOG_ERROR("%x", FS(buf));
}

FILE* compat_fopen(const char* pathname, const char* mode);

int compat_read(int fd, void *buf, size_t count){
    unsigned int uicount = (unsigned int)MIN(UINT_MAX, count);
    return _read(fd, buf, uicount);
}

int compat_write(int fd, const void *buf, size_t count){
    unsigned int uicount = (unsigned int)MIN(UINT_MAX, count);
    return _write(fd, buf, uicount);
}

int compat_open(const char* pathname, int flags, ...){
    // https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/sopen-s-wsopen-s

    // get the optional mode argument, or 0
    va_list ap;
    va_start(ap, flags);
    int mode = 0;
    if(flags & _O_CREAT){
        mode = va_arg(ap, int);
    }
    va_end(ap);

    // if we call posix open() we always want binary mode
    int oflag = _O_BINARY | flags;

    // no sharing of files
    int shflag = _SH_DENYRW;

    // windows-translated read/write permissions
    int pmode = 0;
    if(mode & 0444) pmode |= _S_IREAD;
    if(mode & 0222) pmode |= _S_IWRITE;

    int fdout = -1;

    // the global errno is also set, so we can ignore the return errno
    _sopen_s(&fdout, pathname, oflag, shflag, pmode);

    return fdout;
}

int compat_chmod(const char* pathname, int mode){
    // windows-translated read/write permissions
    int pmode = 0;
    if(mode & 0444) pmode |= _S_IREAD;
    if(mode & 0222) pmode |= _S_IWRITE;

    return _chmod(pathname, pmode);
}

int compat_close(int fd){
    // https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/close
    return _close(fd);
}

FILE* compat_fopen(const char* pathname, const char* mode){
    // https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/fopen-s-wfopen-s

    // in error conditions, fpout is not modified
    FILE* fpout = NULL;

    // the global errno is also set, so we can ignore the return errno
    fopen_s(&fpout, pathname, mode);

    return fpout;
}

int compat_access(const char* path, int amode){
    // https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/access-waccess

    // the global errno is also set, so we can ignore the return errno
    return (_access_s(path, amode) == 0) ? 0 : -1;
}

int compat_gethostname(char* name, size_t len){
    // https://docs.microsoft.com/en-us/windows/desktop/api/winbase/nf-winbase-getcomputernamea
    /* this will silently discard the len that is output by GetComputerName.
       sorry, GetComputerName */
    // there's no reason why a hostname would be longer than 4GB anyways:
    DWORD dw = MIN(4294967295, (DWORD)len);
    BOOL ret = GetComputerNameA(name, &dw);
    if(ret == 0){
        win_perror();
    }

    return (ret != 0) ? 0 : -1;
}

int compat_mkdir(const char* name, int mode){
    (void)mode;
    return _mkdir(name);
}

int compat_strerror_r(int errnum, char* buf, size_t buflen){
    // https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/strerror-s-strerror-s-wcserror-s-wcserror-s
    return (int) strerror_s(buf, buflen, errnum);
}

int compat_pipe(int* pfds){
    // https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/pipe
    return _pipe(pfds, 2, _O_BINARY);
}

#else // not _WIN32
typedef int make_iso_compilers_happy;
#endif // _WIN32
