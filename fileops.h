#ifndef FILESOPS_H
#define FILESOPS_H

#include "common.h"

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

// when for_each_file_in_dir() calls a hook, it always hands a null-terminated dstr_t
// arguments are: (base, file, isdir, userdata)
typedef derr_t (*for_each_file_hook_t)(const char*, const dstr_t*, bool, void*);
derr_t for_each_file_in_dir(const char* path, for_each_file_hook_t hook, void* userdata);
/* throws: E_FS (path too long, from FindFirstFile() or from opendir())
           E_NOMEM (from opendir())
           E_OS (from opendir())
           E_INTERNAL
           <whatever the hook can throw> */

// arguments are: (base, file, isdir, userdata)
typedef derr_t (*for_each_file_hook2_t)(const string_builder_t*, const dstr_t*, bool, void*);
derr_t for_each_file_in_dir2(const string_builder_t* path,
                             for_each_file_hook2_t hook, void* userdata);

derr_t rm_rf(const char* root_path);

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
derr_t dstr_fread_path(const string_builder_t* sb, dstr_t* buffer);

/* If *eno!=NULL then stat_path can only throw E_NOMEM */
derr_t stat_path(const string_builder_t* sb, struct stat* out, int* eno);
derr_t lstat_path(const string_builder_t* sb, struct stat* out, int* eno);

derr_t mkdir_path(const string_builder_t* sb, mode_t mode, bool soft);

derr_t dir_r_access_path(const string_builder_t* sb, bool create, bool* ret);
derr_t dir_w_access_path(const string_builder_t* sb, bool create, bool* ret);
derr_t dir_rw_access_path(const string_builder_t* sb, bool create, bool* ret);

derr_t file_r_access_path(const string_builder_t* sb, bool* ret);
derr_t file_w_access_path(const string_builder_t* sb, bool* ret);
derr_t file_rw_access_path(const string_builder_t* sb, bool* ret);

derr_t exists_path(const string_builder_t* path, bool* ret);
derr_t remove_path(const string_builder_t* sb);

/* mode is unused if the file already exists. Use chmod if you really need to
   set the mode.  Also mode will be modified by umask via open() */
derr_t file_copy(const char* from, const char* to, mode_t mode);
derr_t file_copy_path(const string_builder_t* sb_from,
                      const string_builder_t* sb_to, mode_t mode);

derr_t touch(const char* path);
derr_t touch_path(const string_builder_t* sb);

#endif // FILEOPS_H
