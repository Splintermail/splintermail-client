#ifdef _WIN32
    // I'm not making this up.
    #define WIN32_LEAN_AND_MEAN
    // no point in seeing MSVC warnings in MS's own damn code
    #pragma warning(push, 0)
    #include <windows.h>
    #include <direct.h>
    #include <io.h>
    #include <winsock2.h>
    #include <ws2ipdef.h>
    #include <ws2tcpip.h>
    #pragma warning(pop)

    #define S_ISDIR(mode)  (((mode) & S_IFMT) == S_IFDIR)

    typedef unsigned int mode_t;

    // for compat_access()
#ifndef X_OK
    #define X_OK 00
    #define R_OK 02
    #define W_OK 04
#endif

    // same behavior as libuv, to allow including in either order:
    #ifndef _SSIZE_T_
    #ifndef _SSIZE_T_DEFINED
    typedef intptr_t ssize_t;
    # define SSIZE_MAX INTPTR_MAX
    #endif
    #endif

    void win_perror(void);

    derr_type_t _fmt_win_error(const fmt_i *iface, writer_i *out);
    #define FWINERR (&(fmt_i){ _fmt_win_error })

    ssize_t compat_read(int fd, void *buf, size_t count);
    ssize_t compat_write(int fd, const void *buf, size_t count);
    int compat_open(const char* pathname, int flags, ...);
    int compat_chmod(const char* pathname, int mode);
    int compat_close(int fd);
    FILE* compat_fopen(const char* pathname, const char* mode);
    int compat_access(const char *path, int amode);
    int compat_gethostname(char* name, size_t len);
    int compat_mkdir(const char* name, mode_t mode);
    // compat_remove works on directories the way remove() works in unix
    int compat_remove(const char* name);
    int compat_strerror_r(int errnum, char* buf, size_t buflen);
    int compat_pipe(int* pfds);
    int compat_getpid(void);
    #define compat_fstat _fstat64
    #define compat_stat_t struct _stat64
    #define compat_fputc_unlocked _fputc_nolock
    #define compat_fwrite_unlocked _fwrite_nolock
    #define compat_flockfile _lock_file
    #define compat_funlockfile _unlock_file

    // use the "new" functions... with same prototypes and behaviors
    #define compat_rmdir _rmdir
    #define compat_unlink _unlink
    #define compat_lseek _lseek
    #define compat_dup _dup
    #define compat_fsync _commit
    #define compat_fileno _fileno
    #define compat_tzset _tzset
    #define compat_strdup _strdup
    extern long timezone;

    #define compat_environ _environ

    // what sort of weird-ass magic is MSVC doing that this is required?
    /* it's like some sort of lazy-include, where if these appear as types
       in a function parameter, they become types declared inside partheses */
    extern const struct sockaddr_in6 _ext_sockaddr_in6;
    extern const struct sockaddr_in _ext_sockaddr_in;
    extern const struct sockaddr_storage _ext_sockaddr_storage;
    extern const struct stat _ext_stat;
    extern const struct _stat64 _ext_stat_64;

#else // not _WIN32

    #include <unistd.h>
    #include <libgen.h>
    #include <arpa/inet.h>
    #include <sys/socket.h>

    #define compat_read read
    #define compat_write write
    #define compat_open open
    #define compat_chmod chmod
    #define compat_close close
    #define compat_fopen fopen
    #define compat_fileno fileno
    #define compat_access access
    #define compat_gethostname gethostname
    #define compat_mkdir mkdir
    #define compat_remove remove
    #define compat_unlink unlink
    #define compat_strerror_r strerror_r
    #define compat_pipe pipe
    #define compat_rmdir rmdir
    #define compat_lseek lseek
    #define compat_fstat fstat
    #define compat_fputc_unlocked fputc_unlocked
    #define compat_fwrite_unlocked fwrite_unlocked
    #define compat_flockfile flockfile
    #define compat_funlockfile funlockfile
    #define compat_stat_t struct stat
    #define compat_dup dup
    #define compat_getpid getpid
    #define compat_fsync fsync
    #define compat_tzset tzset
    #define compat_strdup strdup

    #define compat_environ environ

    extern char **compat_environ;

#endif // _WIN32
