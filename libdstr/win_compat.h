#ifndef WIN_COMPAT_H
#define WIN_COMPAT_H


#ifdef _WIN32
    // no point in seeing MSVC warnings in other MS's own damn code
    #pragma warning(push, 0)
    #include <windows.h>
    #include <direct.h>
    #include <io.h>
    #pragma warning(pop)

    #define S_ISDIR(mode)  (((mode) & S_IFMT) == S_IFDIR)

    #define read compat_read
    #define write compat_write
    #define open compat_open
    #define chmod compat_chmod
    #define close compat_close
    #define fopen compat_fopen
    #define gethostname compat_gethostname
    #define mkdir compat_mkdir
    #define strerror_r compat_strerror_r
    #define pipe compat_pipe
    // use the "new" functions... with same prototypes and behaviors
    #define rmdir _rmdir
    #define lseek _lseek
    #define dup _dup
    // for access()
    #define access compat_access
    #define X_OK 00
    #define R_OK 02
    #define W_OK 04
    #define ssize_t int

    void win_perror(void);
    int compat_read(int fd, void *buf, size_t count);
    int compat_write(int fd, const void *buf, size_t count);
    int compat_open(const char* pathname, int flags, ...);
    int compat_chmod(const char* pathname, int mode);
    int compat_close(int fd);
    FILE* compat_fopen(const char* pathname, const char* mode);
    int compat_access(const char *path, int amode);
    int compat_gethostname(char* name, size_t len);
    int compat_mkdir(const char* name, int mode);
    int compat_strerror_r(int errnum, char* buf, size_t buflen);
    int compat_pipe(int* pfds);


#else // not _WIN32

    #include <unistd.h>
    #include <libgen.h>

#endif // _WIN32

#endif // WIN_COMPAT_H
