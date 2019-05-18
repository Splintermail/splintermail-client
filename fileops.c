#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#include "fileops.h"
#include "logger.h"

#include "win_compat.h"

static char* dot = ".";
static char* dotdot = "..";
DSTR_STATIC(slash, "/");

static inline bool do_dir_access(const char* path, bool create, int flags){
    // check if path is a valid directory
    struct stat s;
    int ret = stat(path, &s);
    if(ret != 0){
        LOG_DEBUG("%x: %x\n", FS(path), FE(&errno));
        // if stat failed and we don't want to create the directory, fail
        if(!create){
            return false;
        }
        // we might be able to create the directory
        ret = mkdir(path, 0770);
        if(ret != 0){
            LOG_DEBUG("%x: %x\n", FS(path), FE(&errno));
            LOG_DEBUG("%x: unable to stat or create directory\n", FS(path));
            return false;
        }else{
            // if we just created the directory, no need to check for access
            return true;
        }
    }

    // if stat() succeeded, make sure the path is actually a directory
    if(!S_ISDIR(s.st_mode)){
        LOG_DEBUG("%x: not a directory\n", FS(path));
        return false;
    }

    // lastly, make sure we have access
    ret = access(path, flags);
    if(ret != 0){
        LOG_DEBUG("%x: %x\n", FS(path), FE(&errno));
        LOG_DEBUG("%x: incorrect permissions\n", FS(path));
        return false;
    }
    return true;
}

bool dir_r_access(const char* path, bool create){
    return do_dir_access(path, create, X_OK | R_OK);
}
bool dir_w_access(const char* path, bool create){
    return do_dir_access(path, create, X_OK | W_OK);
}
bool dir_rw_access(const char* path, bool create){
    return do_dir_access(path, create, X_OK | R_OK | W_OK);
}

static inline bool do_file_access(const char* path, int flags){
    // check if path is a valid file
    struct stat s;
    int ret = stat(path, &s);
    if(ret != 0){
        LOG_DEBUG("%x: %x\n", FS(path), FE(&errno));
        // if we can't stat it, that is the end of the test
        return false;
    }
    ret = access(path, flags);
    if(ret != 0){
        LOG_DEBUG("%x: %x\n", FS(path), FE(&errno));
        // could be that it doesn't exist, could be bad permissions
        return false;
    }
    return true;
}

bool file_r_access(const char* path){
    return do_file_access(path, R_OK);
}
bool file_w_access(const char* path){
    return do_file_access(path, W_OK);
}
bool file_rw_access(const char* path){
    return do_file_access(path, R_OK | W_OK);
}

bool exists(const char* path){
    struct stat s;
    int ret = stat(path, &s);
    return ret == 0;
}

derr_t for_each_file_in_dir(const char* path, for_each_file_hook_t hook, void* userdata){
    derr_t e = E_OK;

#ifdef _WIN32

    WIN32_FIND_DATA ffd;
    DSTR_VAR(search, 4096);
    e = FMT(&search, "%x/*", FS(path));
    CATCH(e, E_FIXEDSIZE){
        TRACE(e, "path too long\n");
        RETHROW(e, E_FS);
    }else PROP(e, e);

    HANDLE hFind = FindFirstFile(search.data, &ffd);

    if(hFind == INVALID_HANDLE_VALUE){
        win_perror();
        ORIG(e, E_FS, "FindFirstFile() failed");
    }

    do{
        bool isdir = (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        if(isdir){
            // make sure it is not . or ..
            if(strncmp(ffd.cFileName, dot, 2) == 0) continue;
            if(strncmp(ffd.cFileName, dotdot, 3) == 0) continue;
        }
        // get dstr_t from the filename, it must be null-terminated
        dstr_t dfile;
        DSTR_WRAP(dfile, ffd.cFileName, strlen(ffd.cFileName), true);
        PROP_GO(e, hook(path, &dfile, isdir, userdata), cleanup);
    } while(FindNextFile(hFind, &ffd) != 0);

cleanup:
    FindClose(hFind);
    return e;

#else // not _WIN32

    DIR* d = opendir(path);
    if(!d){
        TRACE(e, "%x: %x\n", FS(path), FE(&errno));
        if(errno == ENOMEM){
            ORIG(e, E_NOMEM, "unable to opendir()");
        }else if(errno == EMFILE || errno == ENFILE){
            ORIG(e, E_OS, "too many file descriptors open");
        }else{
            ORIG(e, E_FS, "unable to open directory");
        }
    }

    while(1){
        errno = 0;
        struct dirent* entry = readdir(d);
        if(!entry){
            if(errno){
                // this can only throw EBADF, which should never happen
                TRACE(e, "%x: %x\n", FS("readdir"), FE(&errno));
                ORIG_GO(e, E_INTERNAL, "unable to readdir()", cleanup);
            }else{
                break;
            }
        }
        bool isdir = (entry->d_type == DT_DIR);
        if(isdir){
            // make sure it is not . or ..
            if(strncmp(entry->d_name, dot, 2) == 0) continue;
            if(strncmp(entry->d_name, dotdot, 3) == 0) continue;
        }
        // get dstr_t from the filename, it must be null-terminated
        dstr_t dfile;
        DSTR_WRAP(dfile, entry->d_name, strlen(entry->d_name), true);
        PROP_GO(e, hook(path, &dfile, isdir, userdata), cleanup);
    }
cleanup:
    closedir(d);
    return e;

#endif

}

static derr_t rm_rf_hook(const char* base, const dstr_t* file,
                                bool isdir, void* userdata){
    (void) userdata;
    DSTR_VAR(path, 4096);
    derr_t e = FMT(&path, "%x/%x", FS(base), FD(file));
    CATCH(e, E_FIXEDSIZE){
        TRACE(e, "path too long\n");
        RETHROW(e, E_FS);
    }else PROP(e, e);

    // base/file is a directory?
    if(isdir){
        // recursively delete everything in the directory
        PROP(e, for_each_file_in_dir(path.data, rm_rf_hook, NULL) );
        // now delete the directory itself
        int ret = rmdir(path.data);
        if(ret != 0){
            TRACE(e, "%x: %x\n", FS(path.data), FE(&errno));
            ORIG(e, E_OS, "failed to remove directory");
        }
    }else{
        // make sure we have write permissions to delete the file
        int ret = chmod(path.data, 0600);
        if(ret != 0){
            TRACE(e, "%x: %x\n", FS(path.data), FE(&errno));
            ORIG(e, E_OS, "failed to remove file");
        }
        // now delete the file
        ret = remove(path.data);
        if(ret != 0){
            TRACE(e, "%x: %x\n", FS(path.data), FE(&errno));
            ORIG(e, E_OS, "failed to remove file");
        }
    }

    return E_OK;
}

derr_t rm_rf(const char* path){
    derr_t e = E_OK;
    // check if this item is a file
    struct stat s;
    int ret = stat(path, &s);
    if(ret != 0){
        TRACE(e, "%x: %x\n", FS(path), FE(&errno));
        ORIG(e, E_OS, "stat call failed");
    }
    // is path a directory?
    if(S_ISDIR(s.st_mode)){
        // if so delete it recursivley
        PROP(e, for_each_file_in_dir(path, rm_rf_hook, NULL) );
        // now delete the directory itself
        ret = rmdir(path);
        if(ret != 0){
            TRACE(e, "%x: %x\n", FS(path), FE(&errno));
            ORIG(e, E_OS, "failed to remove directory");
        }
    }else{
        // otherwise delete just this file
        ret = remove(path);
        if(ret != 0){
            TRACE(e, "%x: %x\n", FS(path), FE(&errno));
            ORIG(e, E_OS, "failed to remove file");
        }
    }
    return E_OK;
}


// the string-builder-based version of for_each_file
derr_t for_each_file_in_dir2(const string_builder_t* path,
                             for_each_file_hook2_t hook, void* userdata){
    derr_t e = E_OK;
    DIR* d;
    PROP(e, opendir_path(path, &d) );

    while(1){
        errno = 0;
        struct dirent* entry = readdir(d);
        if(!entry){
            if(errno){
                // this can only throw EBADF, which should never happen
                TRACE(e, "%x: %x\n", FS("readdir"), FE(&errno));
                ORIG_GO(e, E_INTERNAL, "unable to readdir()", cleanup);
            }else{
                break;
            }
        }
        bool isdir = (entry->d_type == DT_DIR);
        if(isdir){
            // make sure it is not . or ..
            if(strncmp(entry->d_name, dot, 2) == 0) continue;
            if(strncmp(entry->d_name, dotdot, 3) == 0) continue;
        }
        dstr_t dfile;
        DSTR_WRAP(dfile, entry->d_name, strlen(entry->d_name), true);
        PROP_GO(e, hook(path, &dfile, isdir, userdata), cleanup);
    }
cleanup:
    closedir(d);
    return e;
}


// String-Builder-enabled libc wrappers below //

#ifndef _WIN32
static inline derr_t do_chown_path(const string_builder_t* sb, uid_t uid,
                                   gid_t gid, bool l){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(e, sb_expand(sb, &slash, &stack, &heap, &path) );

    int ret;
    if(l){
        ret = lchown(path->data, uid, gid);
    }else{
        ret = chown(path->data, uid, gid);
    }
    if(ret != 0){
        if(errno == ENOMEM){
            ORIG_GO(e, E_NOMEM, "No memory for chown", cu);
        }else{
            TRACE(e, "chown %x: %x\n", FD(path), FE(&errno));
            ORIG_GO(e, E_OS, "chown failed\n", cu);
        }
    }
cu:
    dstr_free(&heap);
    return e;
}
derr_t chown_path(const string_builder_t* sb, uid_t uid, gid_t gid){
    derr_t e = E_OK;
    PROP(e, do_chown_path(sb, uid, gid, false) );
    return E_OK;
}
derr_t lchown_path(const string_builder_t* sb, uid_t uid, gid_t gid){
    derr_t e = E_OK;
    PROP(e, do_chown_path(sb, uid, gid, true) );
    return E_OK;
}

derr_t opendir_path(const string_builder_t* sb, DIR** out){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(e, sb_expand(sb, &slash, &stack, &heap, &path) );

    DIR* d = opendir(path->data);
    if(!d){
        TRACE(e, "opendir(%x): %x\n", FS(path->data), FE(&errno));
        if(errno == ENOMEM){
            ORIG_GO(e, E_NOMEM, "unable to opendir()", cu);
        }else if(errno == EMFILE || errno == ENFILE){
            ORIG_GO(e, E_OS, "too many file descriptors open", cu);
        }else{
            ORIG_GO(e, E_FS, "unable to open directory", cu);
        }
    }
    *out = d;

cu:
    dstr_free(&heap);
    return e;
}

derr_t readlink_path(const string_builder_t* sb, dstr_t* stack_out,
                     dstr_t* heap_out, dstr_t** out){
    // first expand the sb, so we know which link to read
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(e, sb_expand(sb, &slash, &stack, &heap, &path) );

    // now try and read
    ssize_t len = readlink(path->data, stack_out->data, stack_out->size);
    if(len < 0){
        if(errno == ENOMEM){
            ORIG_GO(e, E_NOMEM, "no memory for readlink", cu_path);
        }else{
            TRACE(e, "readlink %x: %x\n", FD(path), FE(&errno));
            ORIG_GO(e, E_FS, "unable to readlink", cu_path);
        }
    }
    // if link wasn't too long, we are done
    if((size_t)len < stack_out->size){
        stack_out->len = (size_t)len;
        // this should never fail now
        PROP_GO(e, dstr_null_terminate(stack_out), cu_path);
        *out = stack_out;
    }else{
        // the stack_out wasn't big enough
        PROP_GO(e, dstr_new(heap_out, stack_out->size * 2), cu_path);
        while(true){
            len = readlink(path->data, heap_out->data, heap_out->size);
            if(len < 0){
                if(errno == ENOMEM){
                    ORIG_GO(e, E_NOMEM, "no memory for readlink", cu_heap);
                }else{
                    TRACE(e, "readlink %x: %x\n", FD(path), FE(&errno));
                    ORIG_GO(e, E_FS, "unable to readlink", cu_heap);
                }
            }
            // check if the heap_out was big enough
            if((size_t)len < heap_out->size){
                break;
            }
            // otherwise grow it and try again
            PROP_GO(e, dstr_grow(heap_out, heap_out->size * 2), cu_heap);
        }
        PROP_GO(e, dstr_null_terminate(heap_out), cu_path);
        *out = heap_out;
    }

cu_heap:
    if(e.type) dstr_free(heap_out);
cu_path:
    dstr_free(&heap);
    return e;
}

derr_t symlink_path(const string_builder_t* to, const string_builder_t* at){
    derr_t e = E_OK;
    DSTR_VAR(stack_at, 256);
    dstr_t heap_at = {0};
    dstr_t* at_out;
    PROP(e, sb_expand(at, &slash, &stack_at, &heap_at, &at_out) );

    DSTR_VAR(stack_to, 256);
    dstr_t heap_to = {0};
    dstr_t* to_out;
    PROP_GO(e, sb_expand(to, &slash, &stack_to, &heap_to,
                       &to_out), cu_at);

    int ret = symlink(to_out->data, at_out->data);
    if(ret != 0){
        if(errno == ENOMEM){
            ORIG_GO(e, E_NOMEM, "no memory for symlink", cu_to);
        }else{
            TRACE(e, "symlink %x -> %x: %x\n",
                      FD(at_out), FD(to_out), FE(&errno));
            ORIG_GO(e, E_FS, "unable to symlink", cu_to);
        }
    }

cu_to:
    dstr_free(&heap_to);
cu_at:
    dstr_free(&heap_at);
    return e;
}
#endif // _WIN32

derr_t chmod_path(const string_builder_t* sb, mode_t mode){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(e, sb_expand(sb, &slash, &stack, &heap, &path) );

    int ret = chmod(path->data, mode);
    if(ret != 0){
        if(errno == ENOMEM){
            ORIG_GO(e, E_NOMEM, "No memory for chmod", cu);
        }else{
            TRACE(e, "chmod %x: %x\n", FD(path), FE(&errno));
            ORIG_GO(e, E_OS, "chmod failed\n", cu);
        }
    }

cu:
    dstr_free(&heap);
    return e;
}

derr_t dstr_fread_path(const string_builder_t* sb, dstr_t* buffer){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(e, sb_expand(sb, &slash, &stack, &heap, &path) );

    PROP_GO(e, dstr_fread_file(path->data, buffer), cu);

cu:
    dstr_free(&heap);
    return e;
}

static inline derr_t do_stat_path(const string_builder_t* sb, struct stat* out,
                                  int* eno, bool l){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(e, sb_expand(sb, &slash, &stack, &heap, &path) );

    errno = 0;
    int ret;
    if(l){
        ret = lstat(path->data, out);
    }else{
        ret = stat(path->data, out);
    }
    if(eno != NULL) *eno = errno;
    if(ret != 0){
        if(errno == ENOMEM){
            ORIG_GO(e, E_NOMEM, "no memory for stat", cu);
        // only throw E_FS if calling function doesn't check *eno
        }else if(eno == NULL){
            TRACE(e, "unable to stat %x: %x\n", FD(path), FE(&errno));
            ORIG_GO(e, E_FS, "unable to stat", cu);
        }
    }

cu:
    dstr_free(&heap);
    return e;
}
derr_t stat_path(const string_builder_t* sb, struct stat* out, int* eno){
    derr_t e = E_OK;
    PROP(e, do_stat_path(sb, out, eno, false) );
    return E_OK;
}
derr_t lstat_path(const string_builder_t* sb, struct stat* out, int* eno){
    derr_t e = E_OK;
    PROP(e, do_stat_path(sb, out, eno, true) );
    return E_OK;
}

derr_t mkdir_path(const string_builder_t* sb, mode_t mode, bool soft){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(e, sb_expand(sb, &slash, &stack, &heap, &path) );

    int ret = mkdir(path->data, mode);
    if(ret != 0){
        if(errno == ENOMEM){
            ORIG_GO(e, E_NOMEM, "no memory for mkdir", cu);
        }else if(soft && errno == EEXIST){
            // we might ignore this error, if what exists is a directory
            struct stat s;
            int ret = stat(path->data, &s);
            if(ret == 0 && S_ISDIR(s.st_mode)){
                // it's a directory!  we are fine.
                goto cu;
            }else if(errno == ENOMEM){
                ORIG_GO(e, E_NOMEM, "no memory for stat", cu);
            }else{
                TRACE(e, "mkdir %x failed: file already exists\n", FD(path));
                ORIG_GO(e, E_FS, "unable to mkdir", cu);
            }
        }else{
            TRACE(e, "unable to mkdir %x: %x\n", FD(path), FE(&errno));
            ORIG_GO(e, E_FS, "unable to mkdir", cu);
        }
    }

cu:
    dstr_free(&heap);
    return e;
}

static inline derr_t do_dir_access_path(const string_builder_t* sb, bool create,
                                        bool* ret, int flags){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(e, sb_expand(sb, &slash, &stack, &heap, &path) );

    *ret = do_dir_access(path->data, create, flags);

    dstr_free(&heap);
    return E_OK;
}
derr_t dir_r_access_path(const string_builder_t* sb, bool create, bool* ret){
    derr_t e = E_OK;
    PROP(e, do_dir_access_path(sb, create, ret, X_OK | R_OK) );
    return E_OK;
}
derr_t dir_w_access_path(const string_builder_t* sb, bool create, bool* ret){
    derr_t e = E_OK;
    PROP(e, do_dir_access_path(sb, create, ret, X_OK | W_OK) );
    return E_OK;
}
derr_t dir_rw_access_path(const string_builder_t* sb, bool create, bool* ret){
    derr_t e = E_OK;
    PROP(e, do_dir_access_path(sb, create, ret, X_OK | R_OK | W_OK) );
    return E_OK;
}

static inline derr_t do_file_access_path(const string_builder_t* sb, bool* ret, int flags){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(e, sb_expand(sb, &slash, &stack, &heap, &path) );

    *ret = do_file_access(path->data, flags);

    dstr_free(&heap);
    return E_OK;
}
derr_t file_r_access_path(const string_builder_t* sb, bool* ret){
    derr_t e = E_OK;
    PROP(e, do_file_access_path(sb, ret, R_OK) );
    return E_OK;
}
derr_t file_w_access_path(const string_builder_t* sb, bool* ret){
    derr_t e = E_OK;
    PROP(e, do_file_access_path(sb, ret, W_OK) );
    return E_OK;
}
derr_t file_rw_access_path(const string_builder_t* sb, bool* ret){
    derr_t e = E_OK;
    PROP(e, do_file_access_path(sb, ret, R_OK | W_OK) );
    return E_OK;
}

derr_t exists_path(const string_builder_t* sb, bool* ret){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(e, sb_expand(sb, &slash, &stack, &heap, &path) );

    *ret = exists(path->data);

    dstr_free(&heap);
    return E_OK;
}

derr_t remove_path(const string_builder_t* sb){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(e, sb_expand(sb, &slash, &stack, &heap, &path) );

    int ret = remove(path->data);
    if(ret != 0){
        if(errno == ENOMEM){
            ORIG_GO(e, E_NOMEM, "no memory for remove", cu);
        }else{
            TRACE(e, "unable to remove %x: %x\n", FD(path), FE(&errno));
            ORIG_GO(e, E_FS, "unable to remove", cu);
        }
    }

cu:
    dstr_free(&heap);
    return e;
}

derr_t file_copy(const char* from, const char* to, mode_t mode){
    derr_t e = E_OK;
    DSTR_VAR(buffer, 4096);

    int fdin = open(from, O_RDONLY);
    if(fdin < 0){
        if(errno == ENOMEM){
            ORIG(e, E_NOMEM, "no memory for open");
        }else{
            TRACE(e, "%x: %x\n", FS(from), FE(&errno));
            ORIG(e, E_OPEN, "unable to open file");
        }
    }

    int fdout = open(to, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if(fdout < 0){
        if(errno == ENOMEM){
            ORIG_GO(e, E_NOMEM, "no memory for open", cu_fdin);
        }else{
            TRACE(e, "%x: %x\n", FS(to), FE(&errno));
            ORIG_GO(e, E_OPEN, "unable to open file", cu_fdin);
        }
    }

    // read and write chunks until we run out
    while(true){
        size_t amnt_read;
        buffer.len = 0;
        PROP_GO(e, dstr_read(fdin, &buffer, 0, &amnt_read), cu_fdout);
        // break on a no-read
        if(amnt_read == 0) break;
        PROP_GO(e, dstr_write(fdout, &buffer), cu_fdout);
    }

cu_fdout:
    close(fdout);
cu_fdin:
    close(fdin);
    return e;
}

derr_t file_copy_path(const string_builder_t* sb_from,
                      const string_builder_t* sb_to, mode_t mode){
    derr_t e = E_OK;
    DSTR_VAR(stack_from, 256);
    dstr_t heap_from = {0};
    dstr_t* path_from;
    PROP(e, sb_expand(sb_from, &slash, &stack_from, &heap_from, &path_from) );

    DSTR_VAR(stack_to, 256);
    dstr_t heap_to = {0};
    dstr_t* path_to;
    PROP_GO(e, sb_expand(sb_to, &slash, &stack_to, &heap_to, &path_to), cu_from);

    PROP_GO(e, file_copy(path_from->data, path_to->data, mode), cu_to);

cu_to:
    dstr_free(&heap_to);
cu_from:
    dstr_free(&heap_from);
    return e;
}

derr_t touch(const char* path){
    derr_t e = E_OK;
    // create the file if necessary
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if(fd < 0){
        if(errno == ENOMEM){
            ORIG(e, E_NOMEM, "no memory for open");
        }else{
            TRACE(e, "%x: %x\n", FS(path), FE(&errno));
            ORIG(e, E_OPEN, "unable to open file");
        }
    }
    close(fd);
    // use uitme to update the file's timestamp
    int ret = utime(path, NULL);
    if(ret != 0){
        TRACE(e, "%x: %x\n", FS(path), FE(&errno));
        ORIG(e, E_FS, "utime failed");
    }
    return E_OK;
}

derr_t touch_path(const string_builder_t* sb){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(e, sb_expand(sb, &slash, &stack, &heap, &path) );

    PROP_GO(e, touch(path->data), cu);

cu:
    dstr_free(&heap);
    return e;
}
