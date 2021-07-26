#ifdef _WIN32
    typedef unsigned int mode_t;
    #include <sys/utime.h>
#else
    #include <dirent.h>
    #include <sys/stat.h>
    #include <utime.h>
#endif

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

// GNU semantics, not POSIX semantics
dstr_t dstr_basename(const dstr_t *path);

// when for_each_file_in_dir() calls a hook, it always hands a null-terminated dstr_t
// arguments are: (base, file, isdir, userdata)
typedef derr_t (*for_each_file_hook_t)(const char*, const dstr_t*, bool, void*);
derr_t for_each_file_in_dir(const char* path, for_each_file_hook_t hook, void* userdata);
/* throws: E_FS (path too long, from FindFirstFile() or from opendir())
           E_NOMEM (from opendir())
           E_OS (from opendir())
           E_INTERNAL
           <whatever the hook can throw> */

typedef derr_t (*for_each_file_hook2_t)(const string_builder_t* base,
        const dstr_t* file, bool isdir, void* userdata);
derr_t for_each_file_in_dir2(const string_builder_t* path,
                             for_each_file_hook2_t hook, void* userdata);

derr_t rm_rf(const char* root_path);
derr_t rm_rf_path(const string_builder_t *sb);
// like rm_rf_path but it leaves the top-level directory untouched
derr_t empty_dir(const string_builder_t *sb);

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
derr_t lstat_path(const string_builder_t* sb, struct stat* out, int* eno);

derr_t mkdir_path(const string_builder_t* sb, mode_t mode, bool soft);
derr_t mkdirs_path(const string_builder_t* sb, mode_t mode);

derr_t dir_r_access_path(const string_builder_t* sb, bool create, bool* ret);
derr_t dir_w_access_path(const string_builder_t* sb, bool create, bool* ret);
derr_t dir_rw_access_path(const string_builder_t* sb, bool create, bool* ret);

derr_t file_r_access_path(const string_builder_t* sb, bool* ret);
derr_t file_w_access_path(const string_builder_t* sb, bool* ret);
derr_t file_rw_access_path(const string_builder_t* sb, bool* ret);

derr_t exists_path(const string_builder_t* path, bool* ret);
derr_t dremove(const char *path);
derr_t remove_path(const string_builder_t* sb);

/* mode is unused if the file already exists. Use chmod if you really need to
   set the mode.  Also mode will be modified by umask via compat_open() */
derr_t file_copy(const char* from, const char* to, mode_t mode);
derr_t file_copy_path(const string_builder_t* sb_from,
                      const string_builder_t* sb_to, mode_t mode);

derr_t touch(const char* path);
derr_t touch_path(const string_builder_t* sb);

derr_t drename(const char *src, const char *dst);
derr_t drename_path(const string_builder_t *src, const string_builder_t *dst);

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
