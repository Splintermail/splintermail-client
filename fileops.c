#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>

#include "fileops.h"
#include "logger.h"

#include "win_compat.h"

#ifndef _WIN32
    #include <dirent.h>
#endif

static char* dot = ".";
static char* dotdot = "..";


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

#ifdef _WIN32

    derr_t error;
    WIN32_FIND_DATA ffd;
    DSTR_VAR(search, 4096);
    error = FMT(&search, "%x/*", FS(path));
    CATCH(E_FIXEDSIZE){
        LOG_ERROR("path too long\n");
        RETHROW(E_FS);
    }else PROP(error);

    HANDLE hFind = FindFirstFile(search.data, &ffd);

    if(hFind == INVALID_HANDLE_VALUE){
        win_perror();
        ORIG(E_FS, "FindFirstFile() failed");
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
        PROP_GO( hook(path, &dfile, isdir, userdata), cleanup);
    } while(FindNextFile(hFind, &ffd) != 0);

cleanup:
    FindClose(hFind);
    return error;;

#else // not _WIN32

    derr_t error = E_OK;
    DIR* d = opendir(path);
    if(!d){
        LOG_ERROR("%x: %x\n", FS(path), FE(&errno));
        if(errno == ENOMEM){
            ORIG(E_NOMEM, "unable to opendir()");
        }else if(errno == EMFILE || errno == ENFILE){
            ORIG(E_OS, "too many file descriptors open");
        }else{
            ORIG(E_FS, "unable to open directory");
        }
    }

    while(1){
        errno = 0;
        struct dirent* entry = readdir(d);
        if(!entry){
            if(errno){
                // this can only throw EBADF, which should never happen
                LOG_ERROR("%x: %x\n", FS("readdir"), FE(&errno));
                ORIG_GO(E_INTERNAL, "unable to readdir()", cleanup);
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
        PROP_GO( hook(path, &dfile, isdir, userdata), cleanup);
    }
cleanup:
    closedir(d);
    return error;

#endif

}

static derr_t rm_rf_hook(const char* base, const dstr_t* file,
                                bool isdir, void* userdata){
    (void) userdata;
    DSTR_VAR(path, 4096);
    derr_t error = FMT(&path, "%x/%x", FS(base), FD(file));
    CATCH(E_FIXEDSIZE){
        LOG_ERROR("path too long\n");
        RETHROW(E_FS);
    }else PROP(error);

    // base/file is a directory?
    if(isdir){
        // recursively delete everything in the directory
        PROP( for_each_file_in_dir(path.data, rm_rf_hook, NULL) );
        // now delete the directory itself
        int ret = rmdir(path.data);
        if(ret != 0){
            LOG_ERROR("%x: %x\n", FS(path.data), FE(&errno));
            ORIG(E_OS, "failed to remove directory");
        }
    }else{
        // make sure we have write permissions to delete the file
        int ret = chmod(path.data, 0600);
        if(ret != 0){
            LOG_ERROR("%x: %x\n", FS(path.data), FE(&errno));
            ORIG(E_OS, "failed to remove file");
        }
        // now delete the file
        ret = remove(path.data);
        if(ret != 0){
            LOG_ERROR("%x: %x\n", FS(path.data), FE(&errno));
            ORIG(E_OS, "failed to remove file");
        }
    }

    return E_OK;
}

derr_t rm_rf(const char* path){

    // check if this item is a file
    struct stat s;
    int ret = stat(path, &s);
    if(ret != 0){
        LOG_ERROR("%x: %x\n", FS(path), FE(&errno));
        ORIG(E_OS, "stat call failed");
    }
    // is path a directory?
    if(S_ISDIR(s.st_mode)){
        // if so delete it recursivley
        PROP( for_each_file_in_dir(path, rm_rf_hook, NULL) );
        // now delete the directory itself
        ret = rmdir(path);
        if(ret != 0){
            LOG_ERROR("%x: %x\n", FS(path), FE(&errno));
            ORIG(E_OS, "failed to remove directory");
        }
    }else{
        // otherwise delete just this file
        ret = remove(path);
        if(ret != 0){
            LOG_ERROR("%x: %x\n", FS(path), FE(&errno));
            ORIG(E_OS, "failed to remove file");
        }
    }
    return E_OK;
}
