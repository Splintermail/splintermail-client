#ifdef _WIN32
    // no point in seeing MSVC warnings in other MS's own damn code
    #pragma warning(push, 0)
    #include <windows.h>
    #include <direct.h>
    #include <io.h>
    #pragma warning(pop)

    #define S_ISDIR(mode)  (((mode) & S_IFMT) == S_IFDIR)

    // for compat_access()
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

    // use the "new" functions... with same prototypes and behaviors
    #define compat_rmdir _rmdir
    #define compat_lseek _lseek
    #define compat_dup _dup

    #define compat_environ _environ
    #define compat_putenv _putenv

#else // not _WIN32

    #include <unistd.h>
    #include <libgen.h>

    #define compat_read read
    #define compat_write write
    #define compat_open open
    #define compat_chmod chmod
    #define compat_close close
    #define compat_fopen fopen
    #define compat_access access
    #define compat_gethostname gethostname
    #define compat_mkdir mkdir
    #define compat_strerror_r strerror_r
    #define compat_pipe pipe
    #define compat_rmdir rmdir
    #define compat_lseek lseek
    #define compat_dup dup
    #define compat_putenv putenv

    #define compat_environ environ

#endif // _WIN32
