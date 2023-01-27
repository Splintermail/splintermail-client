#ifdef _WIN32
    #include <sys/utime.h>
#else
    #include <dirent.h>
    #include <sys/stat.h>
    #include <utime.h>
#endif

// certain filesystem calls are interceptable, for the purpose of testing.
typedef struct {
    int (*mkdir)(const char *path, mode_t mode);
} fileops_harness_t;

extern fileops_harness_t fileops_harness;

// if (create == true), these functions will attempt to create the directory
bool dir_r_access(const char* path, bool create);
bool dir_w_access(const char* path, bool create);
bool dir_rw_access(const char* path, bool create);

bool file_r_access(const char* path);
bool file_w_access(const char* path);
bool file_rw_access(const char* path);

bool exists(const char* path);

// if exists == NULL, non-existing files will throw E_OS
derr_t dstat(const char *path, struct stat *s, bool *exists);
/* throws: E_NOMEM
           E_OS */

derr_t dfstat(int fd, compat_stat_t *s);

derr_t dffstat(FILE *f, compat_stat_t *s);

derr_t dstat_path(const string_builder_t *sb, struct stat *s, bool *exists);
/* throws: E_NOMEM
           E_OS */

// non-existing files do not throw an error, but inaccessible files do
derr_t is_file(const char *path, bool *res);
derr_t is_file_path(const string_builder_t *sb, bool *res);
/* throws: E_NOMEM
           E_OS */

// non-existing dirs do not throw an error, but inaccessible dirs do
derr_t is_dir(const char *path, bool *res);
derr_t is_dir_path(const string_builder_t *sb, bool *res);
/* throws: E_NOMEM
           E_OS */

/* More POSIXy semantics than GNUy semantics.

   Components of a file path:

    C:/   path/to   /     file      /
    ^^^   ^^^^^^^   ^     ^^^^      ^
    vol   dir     joiner  base   trailing separator

    (p="present")
    vol  dir  base | dirname() | basename()
    -------------------------------------------------
    p    p    p    | vol + dir | base
    p    -    p    | vol       | base
    p    -    -    | vol       | vol
    -    p    p    | dir       | base
    -    -    p    | '.'       | base
    -    -    -    | '.'       | '.'

   In unix, the only possible volume is '/'.  In Windows, there are many
   possible volumes.  Windows also honors '\\' as a separator.
*/
dstr_t ddirname(const dstr_t path);
dstr_t dbasename(const dstr_t path);

typedef derr_t (*for_each_file_hook_t)(
    const string_builder_t *base,
    const dstr_t *file,
    bool isdir,
    void *userdata
);
derr_t for_each_file_in_dir(
    const string_builder_t* path, for_each_file_hook_t hook, void* userdata
);
/* throws: E_FS (from FindFirstFile() or from opendir())
           E_NOMEM (from opendir(), sb_expand())
           E_OS (from opendir())
           E_INTERNAL
           <whatever the hook can throw> */

derr_t rm_rf(const char* root_path);
derr_t rm_rf_path(const string_builder_t *path);
// like rm_rf_path but it leaves the top-level directory untouched
derr_t empty_dir(const string_builder_t *path);

// String-Builder-enabled libc wrappers below //

#ifndef _WIN32
derr_t chown_path(const string_builder_t* sb, uid_t uid, gid_t gid);
derr_t lchown_path(const string_builder_t* sb, uid_t uid, gid_t gid);
derr_t opendir_path(const string_builder_t* sb, DIR** out);

// usage is similar to sb_expand().  This will always null-terminate *out.
derr_t readlink_path(const string_builder_t* sb, dstr_t* stack_out,
                     dstr_t* heap_out, dstr_t** out);
derr_t symlink_path(const string_builder_t* to, const string_builder_t* at);
#endif // _WIN32

derr_t chmod_path(const string_builder_t* sb, mode_t mode);

/* If *eno!=NULL then stat_path can only throw E_NOMEM */
derr_t stat_path(const string_builder_t* sb, struct stat* out, int* eno);
#ifndef _WIN32
derr_t lstat_path(const string_builder_t* sb, struct stat* out, int* eno);
#endif // _WIN32

derr_t dmkdir(const char *path, mode_t mode, bool soft);
derr_t mkdir_path(const string_builder_t* sb, mode_t mode, bool soft);
derr_t mkdirs_path(const string_builder_t* sb, mode_t mode);

derr_t dir_r_access_path(const string_builder_t* sb, bool create, bool* ret);
derr_t dir_w_access_path(const string_builder_t* sb, bool create, bool* ret);
derr_t dir_rw_access_path(const string_builder_t* sb, bool create, bool* ret);

derr_t file_r_access_path(const string_builder_t* sb, bool* ret);
derr_t file_w_access_path(const string_builder_t* sb, bool* ret);
derr_t file_rw_access_path(const string_builder_t* sb, bool* ret);

derr_t dexists(const char *path, bool *exists);
derr_t exists_path(const string_builder_t* path, bool* ret);
derr_t dremove(const char *path);
derr_t remove_path(const string_builder_t* sb);
derr_t dunlink(const char *path);
derr_t dunlink_path(const string_builder_t* sb);
derr_t drmdir(const char *path);
derr_t drmdir_path(const string_builder_t* sb);

/* mode is unused if the file already exists. Use chmod if you really need to
   set the mode.  Also mode will be modified by umask via compat_open() */
derr_t file_copy(const char* from, const char* to, mode_t mode);
derr_t file_copy_path(const string_builder_t* sb_from,
                      const string_builder_t* sb_to, mode_t mode);

derr_t touch(const char* path);
derr_t touch_path(const string_builder_t* sb);

// drename() wraps rename(), which will not overwrite in windows
derr_t drename(const char *src, const char *dst);
derr_t drename_path(const string_builder_t *src, const string_builder_t *dst);

// this is just rename() in unix, but windows requires a special API
derr_t drename_atomic(const char *src, const char *dst);
derr_t drename_atomic_path(
    const string_builder_t *src, const string_builder_t *dst
);

derr_t dfopen(const char *path, const char *mode, FILE **out);
derr_t dfopen_path(const string_builder_t *sb, const char *mode, FILE **out);

// avoid the vararg in libc's open()
derr_t dopen(const char *path, int flags, int mode, int *fd);
derr_t dopen_path(const string_builder_t *sb, int flags, int mode, int *out);

derr_t dfsync(int fd);
// combines fflush and fsync
derr_t dffsync(FILE *f);

derr_t dfseek(FILE *f, long offset, int whence);

// checks for errors, passes EOF untouched
derr_t dfgetc(FILE *f, int *c);
derr_t dfputc(FILE *f, int c);

// for when you were going to check the error on close
derr_t dclose(int fd);
derr_t dfclose(FILE *f);

// read an entire file to memory
derr_t dstr_read_file(const char* filename, dstr_t* buffer);
/*  throws : E_NOMEM (reading into *buffer)
             E_FIXEDSIZE (reading into *buffer)
             E_OS (reading)
             E_OPEN */

derr_t dstr_read_path(const string_builder_t* sb, dstr_t* buffer);

// write an entire file from memory
derr_t dstr_write_file(const char* filename, const dstr_t* buffer);
/*  throws : E_OS
             E_OPEN */

derr_t dstr_write_path(const string_builder_t* sb, const dstr_t* buffer);
