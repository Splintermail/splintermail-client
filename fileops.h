#ifndef FILESOPS_H
#define FILESOPS_H

#include "common.h"

// if (create == true), these functions will attempt to create the directory
bool dir_r_access(const char* path, bool create);
bool dir_w_access(const char* path, bool create);
bool dir_rw_access(const char* path, bool create);

bool file_r_access(const char* path);
bool file_w_access(const char* path);
bool file_rw_access(const char* path);

bool exists(const char* path);

// when for_each_file_in_dir() calls a hook, it always hands a null-terminated dstr_t
typedef derr_t (*for_each_file_hook_t)(const char*, const dstr_t*, bool, void*);
derr_t for_each_file_in_dir(const char* path, for_each_file_hook_t hook, void* userdata);
/* throws: E_FS (path too long, from FindFirstFile() or from opendir())
           E_NOMEM (from opendir())
           E_OS (from opendir())
           E_INTERNAL
           <whatever the hook can throw> */

derr_t rm_rf(const char* root_path);

#endif // FILEOPS_H
