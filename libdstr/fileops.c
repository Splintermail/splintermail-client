#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>

#include "libdstr.h"

fileops_harness_t fileops_harness = {
    .mkdir = compat_mkdir,
};

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
        ret = fileops_harness.mkdir(path, 0770);
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
    ret = compat_access(path, flags);
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
    ret = compat_access(path, flags);
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

derr_t dstat(const char *path, struct stat *s, bool *exists){
    derr_t e = E_OK;
    if(exists) *exists = false;
    *s = (struct stat){0};

    int ret = stat(path, s);
    if(ret != 0){
        if(errno == ENOMEM){
            ORIG(&e, E_NOMEM, "nomem");
        }

        // catch non-existent file errors only if exists != NULL
        if(exists && (errno == ENOENT || errno == ENOTDIR)){
            *exists = false;
            return e;
        }

        TRACE(&e, "%x: %x\n", FS(path), FE(&errno));
        ORIG(&e, E_OS, "failed in stat");
    }

    *exists = true;
    return e;
}

derr_t dstat_path(const string_builder_t *sb, struct stat *s, bool *exists){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    PROP_GO(&e, dstat(path->data, s, exists), cu);

cu:
    dstr_free(&heap);
    return e;
}

derr_t is_file(const char *path, bool *res){
    derr_t e = E_OK;
    *res = false;

    bool exists;
    struct stat s;
    PROP(&e, dstat(path, &s, &exists) );

    *res = exists && !S_ISDIR(s.st_mode);

    return e;
}

derr_t is_file_path(const string_builder_t *sb, bool *res){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    PROP_GO(&e, is_file(path->data, res), cu);

cu:
    dstr_free(&heap);
    return e;
}

derr_t is_dir(const char *path, bool *res){
    derr_t e = E_OK;
    *res = false;

    bool exists;
    struct stat s;
    PROP(&e, dstat(path, &s, &exists) );

    *res = exists && S_ISDIR(s.st_mode);

    return e;
}

derr_t is_dir_path(const string_builder_t *sb, bool *res){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    PROP_GO(&e, is_dir(path->data, res), cu);

cu:
    dstr_free(&heap);
    return e;
}

// filepath utilities

static bool _is_sep(char c){
    #ifndef _WIN32 // UNIX
    return c == '/';
    #else // WINDOWS
    return c == '/' || c == '\\';
    #endif
}

// returns the number of trailing seps at the end of path, excluding tail
static size_t _get_trailing_sep(const dstr_t path, size_t tail){
    size_t out = 0;
    for(; out + tail < path.len; out++){
        if(!_is_sep(path.data[path.len - tail - 1 - out])) return out;
    }
    return out;
}

// returns the number of trailing non-seps at the end of path, excluding tail
static size_t _get_trailing_nonsep(const dstr_t path, size_t tail){
    size_t out = 0;
    for(; out + tail < path.len; out++){
        if(_is_sep(path.data[path.len - tail - 1 - out])) return out;
    }
    return out;
}

#ifdef _WIN32 // WINDOWS

// returns number of seps at start of path, exlcuding start
static size_t _get_sep(const dstr_t path, size_t start){
    size_t out = 0;
    for(; start + out < path.len; out++){
        if(!_is_sep(path.data[start + out])) return out;
    }
    return out;
}

// returns number of non-seps at start of path, exlcuding start
static size_t _get_non_sep(const dstr_t path, size_t start){
    size_t out = 0;
    for(; start + out < path.len; out++){
        if(_is_sep(path.data[start + out])) return out;
    }
    return out;
}

// returns 0 if not found, 2 for relative, 3 for absolute
static size_t _get_letter_drive(
    const dstr_t path, size_t start, bool colon, bool include_sep
){
    if(start > path.len || path.len - start < 2) return 0;
    char c = path.data[start];
    if(!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return 0;
    if(path.data[start + 1] != (colon ? ':' : '$')) return 0;
    if(include_sep && path.len - start > 2 && _is_sep(path.data[start + 2])){
        return 3;
    }
    return 2;
}

// returns 0 if not found, else (3 + one or more trailing slashes)
static size_t _get_dos_device_indicator(const dstr_t path){
    if(path.len < 4) return 0;
    if(!_is_sep(path.data[0])) return 0;
    if(!_is_sep(path.data[1])) return 0;
    char c = path.data[2];
    if(c != '.' && c != '?') return 0;
    size_t seps = _get_sep(path, 3);
    if(!seps) return 0;
    return 3 + seps;
}

// returns 0 if not found, else 2
static size_t _get_unc_indicator(const dstr_t path){
    size_t sep = _get_sep(path, 0);
    return sep == 2 ? 2 : 0;
}

// return 0 or (3 + one or more trailing slashes)
static size_t _get_dos_unc_indicator(const dstr_t path, size_t start){
    dstr_t unc = dstr_sub2(path, start, start + 3);
    if(dstr_icmp2(unc, DSTR_LIT("unc")) != 0) return 0;
    size_t seps = _get_sep(path, start + 3);
    if(!seps) return 0;
    return 3 + seps;
}

// returns 0 if not found or the text including server\share or server\c$[\]
static size_t _get_unc(const dstr_t path, size_t start){
    size_t server = _get_non_sep(path, start);
    if(!server) return 0;
    size_t sep = _get_sep(path, start + server);
    if(!sep) return 0;
    // check for the server\c$ case
    size_t drive = _get_letter_drive(path, start + server + sep, false, false);
    if(drive) return (start + server + sep);
    // otherwise expect the server\share case
    size_t share = _get_non_sep(path, start + server + sep);
    if(!share) return 0;
    return server + sep + share;
}

#endif

/* read the atomic part of a path string, the part which is unmodified by both
   the dirname and the basename.  In Unix, that's just a leading '/'.  In
   Windows, it takes many forms:
    - C:                    drive letter (relative path form)
    - C:/                   drive letter (absolute path form)

    - \\server\share        a UNC path to a shared directory
    - \\server\C$           a UNC path to a drive
                              (the docs reference some sort of relative
                               version, but that seems like a myth; I can't
                               `ls` any relative version in powershell)

    - \\.\VOL               a DOS device path (VOL could be C: or Volume{UUID})
    - \\?\VOL               another form of DOS device path
    - \\.\UNC\server\share  a DOS device to a UNC path to a shared directory
    - \\.\UNC\server\C$     a DOS device to a UNC path to a drive

   Based on: docs.microsoft.com/en-us/dotnet/standard/io/file-path-formats */
static size_t _get_volume(const dstr_t path){
#ifdef _WIN32 // WINDOWS
    // letter-drive case: C:
    size_t letter_drive = _get_letter_drive(path, 0, true, true);
    if(letter_drive){
        return letter_drive;
    }

    // DOS device case: \\.\UNC or \\.\DRIVE
    size_t dos_dev = _get_dos_device_indicator(path);
    if(dos_dev){
        // DOS-UNC case
        size_t dos_unc_indicator = _get_dos_unc_indicator(path, dos_dev);
        if(dos_unc_indicator){
            size_t unc = _get_unc(path, dos_dev + dos_unc_indicator);
            if(!unc) return 0;
            return dos_dev + dos_unc_indicator + unc;
        }
        // DOS-VOLUME case, don't bother checking the volume
        size_t volume = _get_non_sep(path, dos_dev);
        if(!volume) return 0;
        return dos_dev + volume;
    }

    // UNC case: \\server\share
    size_t unc_indicator = _get_unc_indicator(path);
    if(unc_indicator){
        size_t unc = _get_unc(path, unc_indicator);
        if(!unc) return 0;
        return unc_indicator + unc;
    }
#endif

    // absolute path
    if(path.len && _is_sep(path.data[0])) return 1;
    return 0;
}

static dstr_t _get_path_part(const dstr_t path, bool wantdir){
    // special case: empty string
    if(path.len == 0) return DSTR_LIT(".");
    // special case: "."
    if(dstr_cmp2(path, DSTR_LIT(".")) == 0) return path;

    // we never break down the volume and we never include it
    size_t volume = _get_volume(path);
    dstr_t nonvol = dstr_sub2(path, volume, path.len);

    // we never consider a trailing sep
    size_t tsep = _get_trailing_sep(nonvol, 0);

    // special case: only the volume [+trailing sep]
    if(tsep == nonvol.len) return dstr_sub2(path, 0, volume);

    // drop the base and the joiner for the base (if any)
    size_t base = _get_trailing_nonsep(nonvol, tsep);
    size_t joiner = _get_trailing_sep(nonvol, tsep + base);
    size_t dir = nonvol.len - tsep - base - joiner;
    if(wantdir){
        // special case: just a basename
        if(volume + dir == 0){
            return DSTR_LIT(".");
        }
        return dstr_sub2(path, 0, volume + dir);
    }else{
        return dstr_sub2(
            path, volume + dir + joiner, volume + dir + joiner + base
        );
    }
}

dstr_t ddirname(const dstr_t path){
    return _get_path_part(path, true);
}

dstr_t dbasename(const dstr_t path){
    return _get_path_part(path, false);
}

derr_t for_each_file_in_dir(const char* path, for_each_file_hook_t hook, void* userdata){
    derr_t e = E_OK;

#ifdef _WIN32

    WIN32_FIND_DATA ffd;
    DSTR_VAR(search, 4096);
    derr_t e2 = FMT(&search, "%x/*", FS(path));
    CATCH(e2, E_FIXEDSIZE){
        TRACE(&e2, "path too long\n");
        RETHROW(&e, &e2, E_FS);
    }else PROP(&e, e2);

    HANDLE hFind = FindFirstFile(search.data, &ffd);

    if(hFind == INVALID_HANDLE_VALUE){
        win_perror();
        ORIG(&e, E_FS, "FindFirstFile() failed");
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
        PROP_GO(&e, hook(path, &dfile, isdir, userdata), cleanup);
    } while(FindNextFile(hFind, &ffd) != 0);

cleanup:
    FindClose(hFind);
    return e;

#else // not _WIN32

    DIR* d = opendir(path);
    if(!d){
        TRACE(&e, "%x: %x\n", FS(path), FE(&errno));
        if(errno == ENOMEM){
            ORIG(&e, E_NOMEM, "unable to opendir()");
        }else if(errno == EMFILE || errno == ENFILE){
            ORIG(&e, E_OS, "too many file descriptors open");
        }else{
            ORIG(&e, E_FS, "unable to open directory");
        }
    }

    while(1){
        errno = 0;
        struct dirent* entry = readdir(d);
        if(!entry){
            if(errno){
                // this can only throw EBADF, which should never happen
                TRACE(&e, "%x: %x\n", FS("readdir"), FE(&errno));
                ORIG_GO(&e, E_INTERNAL, "unable to readdir()", cleanup);
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
        PROP_GO(&e, hook(path, &dfile, isdir, userdata), cleanup);
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
    derr_t e = E_OK;
    derr_t e2;
    e2 = FMT(&path, "%x/%x", FS(base), FD(file));
    CATCH(e2, E_FIXEDSIZE){
        TRACE(&e2, "path too long\n");
        RETHROW(&e, &e2, E_FS);
    }else PROP(&e, e2);

    // base/file is a directory?
    if(isdir){
        // recursively delete everything in the directory
        PROP(&e, for_each_file_in_dir(path.data, rm_rf_hook, NULL) );
        // now delete the directory itself
        int ret = compat_rmdir(path.data);
        if(ret != 0){
            TRACE(&e, "%x: %x\n", FS(path.data), FE(&errno));
            ORIG(&e, E_OS, "failed to remove directory");
        }
    }else{
        // make sure we have write permissions to delete the file
        int ret = compat_chmod(path.data, 0600);
        if(ret != 0){
            TRACE(&e, "%x: %x\n", FS(path.data), FE(&errno));
            ORIG(&e, E_OS, "failed to remove file");
        }
        // now delete the file
        ret = compat_unlink(path.data);
        if(ret != 0){
            TRACE(&e, "%x: %x\n", FS(path.data), FE(&errno));
            ORIG(&e, E_OS, "failed to remove file");
        }
    }

    return e;
}

derr_t rm_rf(const char* path){
    derr_t e = E_OK;
    // check if this item is a file
    struct stat s;
    int ret = stat(path, &s);
    if(ret != 0){
        TRACE(&e, "%x: %x\n", FS(path), FE(&errno));
        ORIG(&e, E_OS, "stat call failed");
    }
    // is path a directory?
    if(S_ISDIR(s.st_mode)){
        // if so delete it recursively
        PROP(&e, for_each_file_in_dir(path, rm_rf_hook, NULL) );
        // now delete the directory itself
        ret = compat_rmdir(path);
        if(ret != 0){
            TRACE(&e, "%x: %x\n", FS(path), FE(&errno));
            ORIG(&e, E_OS, "failed to remove directory");
        }
    }else{
        // otherwise delete just this file
        ret = compat_unlink(path);
        if(ret != 0){
            TRACE(&e, "%x: %x\n", FS(path), FE(&errno));
            ORIG(&e, E_OS, "failed to remove file");
        }
    }
    return e;
}


derr_t rm_rf_path(const string_builder_t *sb){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    PROP_GO(&e, rm_rf(path->data), cu);

cu:
    dstr_free(&heap);
    return e;
}


// like rm_rf_path but it leaves the top-level directory untouched
derr_t empty_dir(const string_builder_t *sb){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    PROP_GO(&e, for_each_file_in_dir(path->data, rm_rf_hook, NULL), cu);

cu:
    dstr_free(&heap);
    return e;
}


// the string-builder-based version of for_each_file
derr_t for_each_file_in_dir2(const string_builder_t* path,
                             for_each_file_hook2_t hook, void* userdata){
#ifdef _WIN32
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};

    derr_t e = E_OK;

    // search spec is "path/*"
    string_builder_t search_spec = sb_append(path, FS("*"));
    dstr_t *search = NULL;
    PROP(&e, sb_expand(&search_spec, &slash, &stack, &heap, &search) );

    WIN32_FIND_DATA ffd;
    HANDLE hFind = FindFirstFile(search->data, &ffd);
    if(hFind == INVALID_HANDLE_VALUE){
        win_perror();
        ORIG_GO(&e, E_FS, "FindFirstFile() failed", cu_heap);
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
        PROP_GO(&e, hook(path, &dfile, isdir, userdata), cu_hfind);
    } while(FindNextFile(hFind, &ffd) != 0);

cu_hfind:
    FindClose(hFind);
cu_heap:
    dstr_free(&heap);
    return e;

#else // not _WIN32
    derr_t e = E_OK;

    DIR* d;
    PROP(&e, opendir_path(path, &d) );

    while(1){
        errno = 0;
        struct dirent* entry = readdir(d);
        if(!entry){
            if(errno){
                // this can only throw EBADF, which should never happen
                TRACE(&e, "%x: %x\n", FS("readdir"), FE(&errno));
                ORIG_GO(&e, E_INTERNAL, "unable to readdir()", cleanup);
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
        PROP_GO(&e, hook(path, &dfile, isdir, userdata), cleanup);
    }
cleanup:
    closedir(d);
    return e;
#endif
}


// String-Builder-enabled libc wrappers below //

#ifndef _WIN32
static inline derr_t do_chown_path(const string_builder_t* sb, uid_t uid,
                                   gid_t gid, bool l){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    int ret;
    if(l){
        ret = lchown(path->data, uid, gid);
    }else{
        ret = chown(path->data, uid, gid);
    }
    if(ret != 0){
        if(errno == ENOMEM){
            ORIG_GO(&e, E_NOMEM, "No memory for chown", cu);
        }else{
            TRACE(&e, "chown %x: %x\n", FD(path), FE(&errno));
            ORIG_GO(&e, E_OS, "chown failed\n", cu);
        }
    }
cu:
    dstr_free(&heap);
    return e;
}
derr_t chown_path(const string_builder_t* sb, uid_t uid, gid_t gid){
    derr_t e = E_OK;
    PROP(&e, do_chown_path(sb, uid, gid, false) );
    return e;
}
derr_t lchown_path(const string_builder_t* sb, uid_t uid, gid_t gid){
    derr_t e = E_OK;
    PROP(&e, do_chown_path(sb, uid, gid, true) );
    return e;
}

derr_t opendir_path(const string_builder_t* sb, DIR** out){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    DIR* d = opendir(path->data);
    if(!d){
        TRACE(&e, "opendir(%x): %x\n", FS(path->data), FE(&errno));
        if(errno == ENOMEM){
            ORIG_GO(&e, E_NOMEM, "unable to opendir()", cu);
        }else if(errno == EMFILE || errno == ENFILE){
            ORIG_GO(&e, E_OS, "too many file descriptors open", cu);
        }else{
            ORIG_GO(&e, E_FS, "unable to open directory", cu);
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
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    // now try and read
    ssize_t len = readlink(path->data, stack_out->data, stack_out->size);
    if(len < 0){
        if(errno == ENOMEM){
            ORIG_GO(&e, E_NOMEM, "no memory for readlink", cu_path);
        }else{
            TRACE(&e, "readlink %x: %x\n", FD(path), FE(&errno));
            ORIG_GO(&e, E_FS, "unable to readlink", cu_path);
        }
    }
    // if link wasn't too long, we are done
    if((size_t)len < stack_out->size){
        stack_out->len = (size_t)len;
        // this should never fail now
        PROP_GO(&e, dstr_null_terminate(stack_out), cu_path);
        *out = stack_out;
    }else{
        // the stack_out wasn't big enough
        PROP_GO(&e, dstr_new(heap_out, stack_out->size * 2), cu_path);
        while(true){
            len = readlink(path->data, heap_out->data, heap_out->size);
            if(len < 0){
                if(errno == ENOMEM){
                    ORIG_GO(&e, E_NOMEM, "no memory for readlink", cu_heap);
                }else{
                    TRACE(&e, "readlink %x: %x\n", FD(path), FE(&errno));
                    ORIG_GO(&e, E_FS, "unable to readlink", cu_heap);
                }
            }
            // check if the heap_out was big enough
            if((size_t)len < heap_out->size){
                break;
            }
            // otherwise grow it and try again
            PROP_GO(&e, dstr_grow(heap_out, heap_out->size * 2), cu_heap);
        }
        PROP_GO(&e, dstr_null_terminate(heap_out), cu_path);
        *out = heap_out;
    }

cu_heap:
    if(is_error(e)) dstr_free(heap_out);
cu_path:
    dstr_free(&heap);
    return e;
}

derr_t symlink_path(const string_builder_t* to, const string_builder_t* at){
    derr_t e = E_OK;
    DSTR_VAR(stack_at, 256);
    dstr_t heap_at = {0};
    dstr_t* at_out;
    PROP(&e, sb_expand(at, &slash, &stack_at, &heap_at, &at_out) );

    DSTR_VAR(stack_to, 256);
    dstr_t heap_to = {0};
    dstr_t* to_out;
    PROP_GO(&e, sb_expand(to, &slash, &stack_to, &heap_to,
                       &to_out), cu_at);

    int ret = symlink(to_out->data, at_out->data);
    if(ret != 0){
        if(errno == ENOMEM){
            ORIG_GO(&e, E_NOMEM, "no memory for symlink", cu_to);
        }else{
            TRACE(&e, "symlink %x -> %x: %x\n",
                      FD(at_out), FD(to_out), FE(&errno));
            ORIG_GO(&e, E_FS, "unable to symlink", cu_to);
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
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    int ret = compat_chmod(path->data, mode);
    if(ret != 0){
        if(errno == ENOMEM){
            ORIG_GO(&e, E_NOMEM, "No memory for chmod", cu);
        }else{
            TRACE(&e, "chmod %x: %x\n", FD(path), FE(&errno));
            ORIG_GO(&e, E_OS, "chmod failed\n", cu);
        }
    }

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
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    errno = 0;
    int ret;
    if(l){
        #ifdef _WIN32
        ORIG_GO(&e, E_INTERNAL, "windows does not have lstat", cu);
        # else
        ret = lstat(path->data, out);
        #endif
    }else{
        ret = stat(path->data, out);
    }
    if(eno != NULL) *eno = errno;
    if(ret != 0){
        if(errno == ENOMEM){
            ORIG_GO(&e, E_NOMEM, "no memory for stat", cu);
        // only throw E_FS if calling function doesn't check *eno
        }else if(eno == NULL){
            TRACE(&e, "unable to stat %x: %x\n", FD(path), FE(&errno));
            ORIG_GO(&e, E_FS, "unable to stat", cu);
        }
    }

cu:
    dstr_free(&heap);
    return e;
}
derr_t stat_path(const string_builder_t* sb, struct stat* out, int* eno){
    derr_t e = E_OK;
    PROP(&e, do_stat_path(sb, out, eno, false) );
    return e;
}
derr_t lstat_path(const string_builder_t* sb, struct stat* out, int* eno){
    derr_t e = E_OK;
    PROP(&e, do_stat_path(sb, out, eno, true) );
    return e;
}

derr_t dmkdir(const char *path, mode_t mode, bool soft){
    derr_t e = E_OK;

    int ret = fileops_harness.mkdir(path, mode);
    if(ret != 0){
        if(errno == ENOMEM){
            ORIG(&e, E_NOMEM, "no memory for mkdir");
        }else if(soft && errno == EEXIST){
            // we might ignore this error, if what exists is a directory
            struct stat s;
            int ret = stat(path, &s);
            if(ret == 0 && S_ISDIR(s.st_mode)){
                // it's a directory!  we are fine.
                return e;
            }else if(errno == ENOMEM){
                ORIG(&e, E_NOMEM, "no memory for stat");
            }else{
                TRACE(&e, "mkdir %x failed: file already exists\n", FS(path));
                ORIG(&e, E_FS, "unable to mkdir");
            }
        }else{
            TRACE(&e, "unable to mkdir %x: %x\n", FS(path), FE(&errno));
            ORIG(&e, E_FS, "unable to mkdir");
        }
    }

    return e;
}

derr_t mkdir_path(const string_builder_t* sb, mode_t mode, bool soft){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    PROP_GO(&e, dmkdir(path->data, mode, soft), cu);

cu:
    dstr_free(&heap);
    return e;
}

derr_t mkdirs_path(const string_builder_t* sb, mode_t mode){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t *path;
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    // if the directory already exists, do nothing
    if(exists(path->data)){
        goto cu;
    }

    // count how many parent directories to make by repeated calls to dirname
    dstr_t tpath = *path;
    size_t tpath_len = tpath.len;
    int nparents = 0;
    while(true){
        tpath = ddirname(tpath);
        bool ok;
        PROP_GO(&e, exists_path(&SB(FD(&tpath)), &ok), cu);
        if(ok){
            // no need to make this parent
            break;
        }
        if(tpath.len == tpath_len){
            // we've arrived at the most root path, either / or .
            nparents--;
            break;
        }
        tpath_len = tpath.len;
        nparents++;
    }

    int ncreated = 0;

    for(int i = nparents; i >= 0; i--){
        // repair the path
        tpath = *path;
        for(int j = 0; j < i; j++){
            tpath = ddirname(tpath);
        }
        // create the ith parent
        PROP_GO(&e, mkdir_path(&SB(FD(&tpath)), mode, true), fail);
        ncreated++;
    }

    // the 0th parent was the full path, so we are done
cu:
    dstr_free(&heap);
    return e;

fail:
    // attempt to delete any folders we created
    for(int i = ncreated - 1; i >= 0; i--){
        // repair the path
        tpath = *path;
        for(int j = 0; j < (nparents - i); j++){
            tpath = ddirname(tpath);
        }
        DROP_CMD( drmdir_path(&SB(FD(&tpath))) );
    }
    return e;
}

static inline derr_t do_dir_access_path(const string_builder_t* sb, bool create,
                                        bool* ret, int flags){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    *ret = do_dir_access(path->data, create, flags);

    dstr_free(&heap);
    return e;
}
derr_t dir_r_access_path(const string_builder_t* sb, bool create, bool* ret){
    derr_t e = E_OK;
    PROP(&e, do_dir_access_path(sb, create, ret, X_OK | R_OK) );
    return e;
}
derr_t dir_w_access_path(const string_builder_t* sb, bool create, bool* ret){
    derr_t e = E_OK;
    PROP(&e, do_dir_access_path(sb, create, ret, X_OK | W_OK) );
    return e;
}
derr_t dir_rw_access_path(const string_builder_t* sb, bool create, bool* ret){
    derr_t e = E_OK;
    PROP(&e, do_dir_access_path(sb, create, ret, X_OK | R_OK | W_OK) );
    return e;
}

static inline derr_t do_file_access_path(const string_builder_t* sb, bool* ret, int flags){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    *ret = do_file_access(path->data, flags);

    dstr_free(&heap);
    return e;
}
derr_t file_r_access_path(const string_builder_t* sb, bool* ret){
    derr_t e = E_OK;
    PROP(&e, do_file_access_path(sb, ret, R_OK) );
    return e;
}
derr_t file_w_access_path(const string_builder_t* sb, bool* ret){
    derr_t e = E_OK;
    PROP(&e, do_file_access_path(sb, ret, W_OK) );
    return e;
}
derr_t file_rw_access_path(const string_builder_t* sb, bool* ret){
    derr_t e = E_OK;
    PROP(&e, do_file_access_path(sb, ret, R_OK | W_OK) );
    return e;
}

derr_t dexists(const char *path, bool *exists){
    derr_t e = E_OK;

    struct stat s;
    PROP(&e, dstat(path, &s, exists) );

    return e;
}

derr_t exists_path(const string_builder_t* sb, bool* ret){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    *ret = exists(path->data);

    dstr_free(&heap);
    return e;
}

derr_t dremove(const char* path){
    derr_t e = E_OK;

    int ret = compat_remove(path);
    if(ret != 0){
        if(errno == ENOMEM){
            ORIG(&e, E_NOMEM, "no memory for remove");
        }else{
            TRACE(&e, "remove(%x): %x\n", FS(path), FE(&errno));
            ORIG(&e, E_FS, "unable to remove");
        }
    }

    return e;
}

derr_t remove_path(const string_builder_t* sb){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    PROP_GO(&e, dremove(path->data), cu);

cu:
    dstr_free(&heap);
    return e;
}

derr_t dunlink(const char *path){
    derr_t e = E_OK;

    int ret = compat_unlink(path);
    if(ret != 0){
        if(errno == ENOMEM){
            ORIG(&e, E_NOMEM, "no memory for unlink");
        }else{
            TRACE(&e, "unlink(%x): %x\n", FS(path), FE(&errno));
            ORIG(&e, E_FS, "unable to unlink");
        }
    }

    return e;
}

derr_t dunlink_path(const string_builder_t* sb){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    PROP_GO(&e, dunlink(path->data), cu);

cu:
    dstr_free(&heap);
    return e;
}

derr_t drmdir(const char *path){
    derr_t e = E_OK;

    int ret = compat_rmdir(path);
    if(ret != 0){
        if(errno == ENOMEM){
            ORIG(&e, E_NOMEM, "no memory for rmdir");
        }else{
            TRACE(&e, "rmdir(%x): %x\n", FS(path), FE(&errno));
            ORIG(&e, E_FS, "unable to rmdir");
        }
    }

    return e;
}

derr_t drmdir_path(const string_builder_t* sb){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    PROP_GO(&e, drmdir(path->data), cu);

cu:
    dstr_free(&heap);
    return e;
}


derr_t file_copy(const char* from, const char* to, mode_t mode){
    derr_t e = E_OK;
    DSTR_VAR(buffer, 4096);

    int fdin = compat_open(from, O_RDONLY);
    if(fdin < 0){
        if(errno == ENOMEM){
            ORIG(&e, E_NOMEM, "no memory for open");
        }else{
            TRACE(&e, "%x: %x\n", FS(from), FE(&errno));
            ORIG(&e, E_OPEN, "unable to open file");
        }
    }

    int fdout = compat_open(to, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if(fdout < 0){
        if(errno == ENOMEM){
            ORIG_GO(&e, E_NOMEM, "no memory for open", cu_fdin);
        }else{
            TRACE(&e, "%x: %x\n", FS(to), FE(&errno));
            ORIG_GO(&e, E_OPEN, "unable to open file", cu_fdin);
        }
    }

    // read and write chunks until we run out
    while(true){
        size_t amnt_read;
        buffer.len = 0;
        PROP_GO(&e, dstr_read(fdin, &buffer, 0, &amnt_read), cu_fdout);
        // break on a no-read
        if(amnt_read == 0) break;
        PROP_GO(&e, dstr_write(fdout, &buffer), cu_fdout);
    }

cu_fdout:
    compat_close(fdout);
cu_fdin:
    compat_close(fdin);
    return e;
}

derr_t file_copy_path(const string_builder_t* sb_from,
                      const string_builder_t* sb_to, mode_t mode){
    derr_t e = E_OK;
    DSTR_VAR(stack_from, 256);
    dstr_t heap_from = {0};
    dstr_t* path_from;
    PROP(&e, sb_expand(sb_from, &slash, &stack_from, &heap_from, &path_from) );

    DSTR_VAR(stack_to, 256);
    dstr_t heap_to = {0};
    dstr_t* path_to;
    PROP_GO(&e, sb_expand(sb_to, &slash, &stack_to, &heap_to, &path_to), cu_from);

    PROP_GO(&e, file_copy(path_from->data, path_to->data, mode), cu_to);

cu_to:
    dstr_free(&heap_to);
cu_from:
    dstr_free(&heap_from);
    return e;
}

derr_t touch(const char* path){
    derr_t e = E_OK;
    // create the file if necessary
    int fd = compat_open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if(fd < 0){
        if(errno == ENOMEM){
            ORIG(&e, E_NOMEM, "no memory for open");
        }else{
            TRACE(&e, "%x: %x\n", FS(path), FE(&errno));
            ORIG(&e, E_OPEN, "unable to open file");
        }
    }
    compat_close(fd);
    // use uitme to update the file's timestamp
    int ret = utime(path, NULL);
    if(ret != 0){
        TRACE(&e, "%x: %x\n", FS(path), FE(&errno));
        ORIG(&e, E_FS, "utime failed");
    }
    return e;
}

derr_t touch_path(const string_builder_t* sb){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    PROP_GO(&e, touch(path->data), cu);

cu:
    dstr_free(&heap);
    return e;
}

derr_t drename(const char *src, const char *dst){
    derr_t e = E_OK;

    int ret = rename(src, dst);
    if(ret != 0){
        TRACE(&e, "rename(%x, %x): %x\n", FS(src), FS(dst), FE(&errno));
        ORIG(&e, errno == ENOMEM ? E_NOMEM : E_OS, "unable to rename file");
    }

    return e;
}

derr_t drename_path(const string_builder_t *src, const string_builder_t *dst){
    derr_t e = E_OK;
    DSTR_VAR(stack_src, 256);
    dstr_t heap_src = {0};
    dstr_t* path_src;
    PROP(&e, sb_expand(src, &slash, &stack_src, &heap_src, &path_src) );

    DSTR_VAR(stack_dst, 256);
    dstr_t heap_dst = {0};
    dstr_t* path_dst;
    PROP_GO(&e, sb_expand(dst, &slash, &stack_dst, &heap_dst, &path_dst),
            cu_src);

    PROP_GO(&e, drename(path_src->data, path_dst->data), cu_dst);

cu_dst:
    dstr_free(&heap_dst);
cu_src:
    dstr_free(&heap_src);
    return e;
}

derr_t drename_atomic(const char *src, const char *dst){
#ifndef _WIN32 // UNIX
    derr_t e = E_OK;
    PROP(&e, drename(src, dst) );
    return e;
#else // WINDOWS
    derr_t e = E_OK;

    /* when the file doesn't exist, just use rename.  ReplaceFile() fails if
       the file doesn't exist yet */
    bool ok;
    PROP(&e, dexists(dst, &ok) );
    if(!ok){
        PROP(&e, drename(src, dst) );
        return e;
    }

    /* Windows has a few APIs that folks peddle as atomic (see
       https://stackoverflow.com/q/167414) but MS itself suggests using the
       ReplaceFile() API. */
    // https://docs.microsoft.com/en-us/windows/win32/fileio/deprecation-of-txf
    // no backup or flags, and two (!) NULL reserved values
    int ret = ReplaceFileA(dst, src, NULL, 0, NULL, NULL);
    if(ret == 0){
        TRACE(&e, "ReplaceFileA(%x -> %x): %x\n", FS(src), FS(dst), FWINERR());
        ORIG(&e, E_OS, "ReplaceFileA failed");
    }
    return e;
#endif
}

derr_t drename_atomic_path(
    const string_builder_t *src, const string_builder_t *dst
){
    DSTR_VAR(stack_src, 256);
    dstr_t heap_src = {0};
    dstr_t* path_src = NULL;
    DSTR_VAR(stack_dst, 256);
    dstr_t heap_dst = {0};
    dstr_t* path_dst = NULL;

    derr_t e = E_OK;

    PROP_GO(&e, sb_expand(src, &slash, &stack_src, &heap_src, &path_src), cu);
    PROP_GO(&e, sb_expand(dst, &slash, &stack_dst, &heap_dst, &path_dst), cu);
    PROP_GO(&e, drename_atomic(path_src->data, path_dst->data), cu);

cu:
    dstr_free(&heap_dst);
    dstr_free(&heap_src);
    return e;
}

derr_t dfopen(const char *path, const char *mode, FILE **out){
    derr_t e = E_OK;

    *out = compat_fopen(path, mode);
    if(!out){
        TRACE(&e, "fopen(%x): %x\n", FS(path), FE(&errno));
        ORIG(&e, errno == ENOMEM ? E_NOMEM : E_OPEN, "unable to open file");
    }

    return e;
}

derr_t dfopen_path(const string_builder_t *sb, const char *mode, FILE **out){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;

    *out = NULL;

    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    PROP_GO(&e, dfopen(path->data, mode, out), cu);

cu:
    dstr_free(&heap);
    return e;
}

derr_t dopen(const char *path, int flags, int mode, int *fd){
    derr_t e = E_OK;

    *fd = compat_open(path, flags, mode);
    if(*fd < 0){
        if(errno == ENOMEM){
            ORIG(&e, E_NOMEM, "no memory for open");
        }
        TRACE(&e, "open(%x): %x\n", FS(path), FE(&errno));
        ORIG(&e, E_OPEN, "unable to open file");
    }

    return e;
}

derr_t dopen_path(const string_builder_t *sb, int flags, int mode, int *fd){
    derr_t e = E_OK;

    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    PROP_GO(&e, dopen(path->data, flags, mode, fd), cu);

cu:
    dstr_free(&heap);
    return e;
}

derr_t dfsync(int fd){
    derr_t e = E_OK;

    int ret = compat_fsync(fd);
    if(ret != 0){
        TRACE(&e, "fsync: %x\n", FE(&errno));
        ORIG(&e, E_OPEN, "fsync failed");
    }

    return e;
}

derr_t dffsync(FILE *f){
    derr_t e = E_OK;

    int ret = fflush(f);
    if(ret != 0){
        TRACE(&e, "fflush: %x\n", FE(&errno));
        ORIG(&e, E_OPEN, "fflush failed");
    }

    #ifndef _WIN32
    // windows fflush() already does this
    ret = compat_fsync(fileno(f));
    if(ret != 0){
        TRACE(&e, "fsync: %x\n", FE(&errno));
        ORIG(&e, E_OPEN, "fsync failed");
    }
    #endif

    return e;
}

derr_t dfseek(FILE *f, long offset, int whence){
    derr_t e = E_OK;

    int ret = fseek(f, offset, whence);
    if(ret != 0){
        TRACE(&e, "fseek: %x\n", FE(&errno));
        ORIG(&e, E_OS, "fseek failed");
    }

    return e;
}

derr_t dfgetc(FILE *f, int *c){
    derr_t e = E_OK;

    *c = fgetc(f);
    if(*c == EOF){
        int error = ferror(f);
        if(error){
            TRACE(&e, "fgetc: %x\n", FE(&error));
            ORIG(&e, E_OS, "fgetc failed");
        }
    }

    return e;
}

derr_t dfputc(FILE *f, int c){
    derr_t e = E_OK;

    int ret = fputc(c, f);
    if(ret == EOF){
        int error = ferror(f);
        TRACE(&e, "fputc: %x\n", FE(&error));
        ORIG(&e, E_OS, "fputc failed");
    }

    return e;
}

derr_t dclose(int fd){
    derr_t e = E_OK;

    int ret = compat_close(fd);
    if(ret){
        TRACE(&e, "close: %x\n", FE(&errno));
        ORIG(&e, E_OPEN, "close failed");
    }

    return e;
}

derr_t dfclose(FILE *f){
    derr_t e = E_OK;

    int ret = fclose(f);
    if(ret){
        TRACE(&e, "fclose: %x\n", FE(&errno));
        ORIG(&e, E_OPEN, "fclose failed");
    }

    return e;
}

derr_t dstr_read_file(const char* filename, dstr_t* buffer){
    derr_t e = E_OK;

    int fd = compat_open(filename, O_RDONLY);
    if(fd < 0){
        TRACE(&e, "%x: %x\n", FS(filename), FE(&errno));
        ORIG(&e, E_OPEN, "unable to open file");
    }
    // if the buffer is intially full, grow it before we start dstr_read()
    if(buffer->len == buffer->size){
        PROP_GO(&e, dstr_grow(buffer, buffer->len + 4096), cleanup);
    }
    while(true){
        size_t amnt_read;
        PROP_GO(&e, dstr_read(fd, buffer, 0, &amnt_read), cleanup);
        // if we read nothing then we're done
        if(amnt_read == 0){
            break;
        }
        /* if we filled the buffer with text we should force the buffer to
           reallocate and try again */
        if(buffer->len == buffer->size){
            PROP_GO(&e, dstr_grow(buffer, buffer->size * 2), cleanup);
        }
    }
cleanup:
    compat_close(fd);
    return e;
}

derr_t dstr_read_path(const string_builder_t* sb, dstr_t* buffer){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    PROP_GO(&e, dstr_read_file(path->data, buffer), cu);

cu:
    dstr_free(&heap);
    return e;
}

derr_t dstr_write_file(const char* filename, const dstr_t* buffer){
    derr_t e = E_OK;

    int fd = compat_open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if(fd < 0){
        TRACE(&e, "%x: %x\n", FS(filename), FE(&errno));
        ORIG(&e, E_OPEN, "unable to open file");
    }
    PROP_GO(&e, dstr_write(fd, buffer), cleanup);

    // ensure things are written to disk
    PROP_GO(&e, dfsync(fd), cleanup);

    int ret;
cleanup:
    ret = compat_close(fd);
    // check for closing error
    if(ret != 0 && !is_error(e)){
        TRACE(&e, "compat_close(%x): %x\n", FS(filename), FE(&errno));
        TRACE_ORIG(&e, E_OS, "failed to write file");
    }
    if(is_error(e)){
        // make a feeble attempt to delete the file
        ret = compat_unlink(filename);
        LOG_ERROR(
            "failed to remove failed file after failed write: %x\n", FE(&errno)
        );
    }
    return e;
}

derr_t dstr_write_path(const string_builder_t *sb, const dstr_t* buffer){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    PROP_GO(&e, dstr_write_file(path->data, buffer), cu);

cu:
    dstr_free(&heap);
    return e;
}
