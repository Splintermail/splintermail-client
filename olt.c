#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>

#include "common.h"
#include "logger.h"
#include "fileops.h"
#include "opt_parse.h"

static char* dot = ".";
static char* dotdot = "..";
DSTR_STATIC(slash, "/");

// foward-delcaration of struct directory, for use in LIST macros
typedef struct directory directory;
typedef directory* directory_p;

LIST_HEADERS(directory)
LIST_HEADERS(directory_p)
LIST_HEADERS(uid_t)
LIST_HEADERS(gid_t)
LIST_HEADERS(mode_t)

// in-memory layout of file system
struct directory {
    dstr_t name;
    LIST(directory) dirs;
    LIST(uid_t) dir_uids;
    LIST(gid_t) dir_gids;
    LIST(mode_t) dir_modes;
    LIST(dstr_t) files;
    LIST(uid_t) file_uids;
    LIST(gid_t) file_gids;
    LIST(mode_t) file_modes;
};


LIST_FUNCTIONS(directory)
LIST_FUNCTIONS(directory_p)
LIST_FUNCTIONS(uid_t)
LIST_FUNCTIONS(gid_t)
LIST_FUNCTIONS(mode_t)


// cache UID and GID lookups
LIST(dstr_t) uid_cache;
LIST(uid_t) uid_results;
LIST(dstr_t) gid_cache;
LIST(gid_t) gid_results;


static derr_t get_uid(const dstr_t* name, uid_t* uid){
    // first check if the name is in the cache
    size_t cached_result;
    if(in_list(name, &uid_cache, &cached_result)){
        *uid = uid_results.data[cached_result];
        return E_OK;
    }

    // get c-string for the name
    DSTR_VAR(cname, 128);
    PROP( FMT(&cname, "%x", FD(name)) );

    // need a buffer for re-entrant UID/GID functions
    char buffer[1024];

    // now get the UID for that name
    struct passwd pwd;
    struct passwd *pwdret;
    int ret = getpwnam_r(cname.data, &pwd, buffer, sizeof(buffer), &pwdret);
    if(ret != 0){
        LOG_ERROR("getpwnam_r(%x): %x\n", FD(name), FE(&errno));
        ORIG(E_OS, "unable to get UID from username");
    }
    if(pwdret == NULL){
        LOG_ERROR("user %x not found\n", FD(name));
        ORIG(E_VALUE, "user not found");
    }
    *uid = pwdret->pw_uid;

    // cache the result
    PROP( LIST_APPEND(dstr_t, &uid_cache, *name) );
    PROP( LIST_APPEND(uid_t, &uid_results, pwdret->pw_uid) );

    return E_OK;
}


static derr_t get_gid(const dstr_t* name, gid_t* gid){
    // first check if the name is in the cache
    size_t cached_result;
    if(in_list(name, &gid_cache, &cached_result)){
        *gid = gid_results.data[cached_result];
        return E_OK;
    }

    // get c-string for the name
    DSTR_VAR(cname, 128);
    PROP( FMT(&cname, "%x", FD(name)) );

    // need a buffer for re-entrant UID/GID functions
    char buffer[1024];

    // now get the GID for that name
    struct group grp;
    struct group *grpret;
    int ret = getgrnam_r(cname.data, &grp, buffer, sizeof(buffer), &grpret);
    if(ret != 0){
        LOG_ERROR("getpwnam_r(%x): %x\n", FD(name), FE(&errno));
        ORIG(E_OS, "unable to get GID from groupname");
    }
    if(grpret == NULL){
        LOG_ERROR("group %x not found\n", FD(name));
        ORIG(E_VALUE, "group not found");
    }
    *gid = grpret->gr_gid;

    // cache the result
    PROP( LIST_APPEND(dstr_t, &gid_cache, *name) );
    PROP( LIST_APPEND(gid_t, &gid_results, grpret->gr_gid) );

    return E_OK;
}


static derr_t directory_new(directory* dir, const dstr_t* name){
    derr_t error;
    dir->name = *name;
    PROP( LIST_NEW(directory, &dir->dirs, 8) );
    PROP_GO( LIST_NEW(uid_t, &dir->dir_uids, 8), fail_dirs);
    PROP_GO( LIST_NEW(gid_t, &dir->dir_gids, 8), fail_dir_uids);
    PROP_GO( LIST_NEW(mode_t, &dir->dir_modes, 8), fail_dir_gids);
    PROP_GO( LIST_NEW(dstr_t, &dir->files, 8), fail_dir_modes);
    PROP_GO( LIST_NEW(uid_t, &dir->file_uids, 8), fail_files);
    PROP_GO( LIST_NEW(gid_t, &dir->file_gids, 8), fail_file_uids);
    PROP_GO( LIST_NEW(mode_t, &dir->file_modes, 8), fail_file_gids);
    return E_OK;

fail_file_gids:
    LIST_FREE(gid_t, &dir->file_gids);
fail_file_uids:
    LIST_FREE(uid_t, &dir->file_uids);
fail_files:
    LIST_FREE(dstr_t, &dir->files);
fail_dir_modes:
    LIST_FREE(mode_t, &dir->dir_modes);
fail_dir_gids:
    LIST_FREE(gid_t, &dir->dir_gids);
fail_dir_uids:
    LIST_FREE(uid_t, &dir->dir_uids);
fail_dirs:
    LIST_FREE(directory, &dir->dirs);
    return error;
}


static void directory_free(directory* dir){
    // first free all subdirs
    for(size_t i = 0; i < dir->dirs.len; i++){
        directory_free(&dir->dirs.data[i]);
    }
    LIST_FREE(mode_t, &dir->file_modes);
    LIST_FREE(gid_t, &dir->file_gids);
    LIST_FREE(uid_t, &dir->file_uids);
    LIST_FREE(dstr_t, &dir->files);
    LIST_FREE(mode_t, &dir->dir_modes);
    LIST_FREE(gid_t, &dir->dir_gids);
    LIST_FREE(uid_t, &dir->dir_uids);
    LIST_FREE(directory, &dir->dirs);
}


static derr_t directory_add_file(directory* dir, const dstr_t* file,
                                 uid_t uid, gid_t gid, mode_t mode){
    // first grow all of the lists
    PROP( LIST_GROW(dstr_t, &dir->files, dir->files.len + 1) );
    PROP( LIST_GROW(uid_t, &dir->file_uids, dir->file_uids.len + 1) );
    PROP( LIST_GROW(gid_t, &dir->file_gids, dir->file_gids.len + 1) );
    PROP( LIST_GROW(mode_t, &dir->file_modes, dir->file_modes.len + 1) );
    // then append to all of the lists
    LIST_APPEND(dstr_t, &dir->files, *file);
    LIST_APPEND(uid_t, &dir->file_uids, uid);
    LIST_APPEND(gid_t, &dir->file_gids, gid);
    LIST_APPEND(mode_t, &dir->file_modes, mode);
    return E_OK;
}


static derr_t directory_add_dir(directory* dir, const dstr_t* name,
                                uid_t uid, gid_t gid, mode_t mode){
    // first grow all of the lists
    PROP( LIST_GROW(directory, &dir->dirs, dir->dirs.len + 1) );
    PROP( LIST_GROW(uid_t, &dir->dir_uids, dir->dir_uids.len + 1) );
    PROP( LIST_GROW(gid_t, &dir->dir_gids, dir->dir_gids.len + 1) );
    PROP( LIST_GROW(mode_t, &dir->dir_modes, dir->dir_modes.len + 1) );
    // the allocate the directory (operate on the list.data value)
    PROP( directory_new(&dir->dirs.data[dir->dirs.len], name) );
    dir->dirs.len += 1;
    // then append to all of the lists
    LIST_APPEND(uid_t, &dir->dir_uids, uid);
    LIST_APPEND(gid_t, &dir->dir_gids, gid);
    LIST_APPEND(mode_t, &dir->dir_modes, mode);
    return E_OK;
}


// static void print_directory(directory* dir, size_t indent){
//     DSTR_PRESET(pre, "                                                   ");
//     pre.len = indent;
//     // first print this directory, taking the fmt_t out of the string_builder_t
//     PFMT("%x%x:\n", FD(&pre), FD(&dir->name));
//     // increase indent
//     indent += 2;
//     pre.len = indent;
//     // now print each file
//     for(size_t i = 0; i < dir->files.len; i++){
//         PFMT("%x%x\n", FD(&pre), FD(&dir->files.data[i]));
//     }
//     // now print each directory
//     for(size_t i = 0; i < dir->dirs.len; i++){
//         print_directory(&dir->dirs.data[i], indent);
//     }
// }


static size_t get_indent(const dstr_t* dstr){
    size_t i;
    for(i = 0; i < dstr->len; i++){
        if(dstr->data[i] != ' ') break;
    }
    return i;
}


static derr_t parse_perms(const dstr_t* perms, directory* root){
    derr_t error;
    // split file into lines
    LIST(dstr_t) lines;
    DSTR_STATIC(newline, "\n");
    PROP( LIST_NEW(dstr_t, &lines, dstr_count(perms, &newline)) );
    PROP_GO( dstr_split(perms, &newline, &lines), cu_lines);
    size_t last_indent = 0;
    // we can push/pop to this, as we build our filesystem tree
    LIST_VAR(directory_p, dpstack, 16);
    LIST_APPEND(directory_p, &dpstack, root);
    // parent directory is always the end of dpstack
    directory_p parent = dpstack.data[dpstack.len - 1];
    directory_p last_line_dir = NULL;
    // now look at each line
    for(size_t i = 0; i < lines.len; i++){
        // get indent
        size_t indent = get_indent(&lines.data[i]);
        // indent should be divisible by 4
        if(indent % 4 != 0){
            LOG_ERROR("bad indent (%x): %x\n", FU(indent), FD(&lines.data[i]));
            ORIG_GO(E_INTERNAL, "bad indent", cu_lines);
        }
        // check if indent has increased
        if(indent > last_indent){
            // make sure the last line was a directory
            if(last_line_dir == NULL){
                LOG_ERROR("can't increase indent here: %x\n", FD(&lines.data[i]));
                ORIG_GO(E_INTERNAL, "can't increase indent here", cu_lines);
            }
            // make sure the indentation increased by exactly 4
            if(indent - last_indent != 4){
                LOG_ERROR("indent jump: %x\n", FD(&lines.data[i]));
                ORIG_GO(E_INTERNAL, "indent jump", cu_lines);
            }
            // now push the last dir onto our dpstack
            PROP_GO( LIST_APPEND(directory_p, &dpstack, last_line_dir), cu_lines);
            // parent directory is always the end of dpstack
            parent = dpstack.data[dpstack.len - 1];
        }
        // otherwise check if indent has decreased
        else if(last_indent > indent){
            // pop from the dpstack
            dpstack.len -= (last_indent - indent)/4;
            // parent directory is always the end of dpstack
            parent = dpstack.data[dpstack.len - 1];
        }
        // save indent as last_indent for next iteration
        last_indent = indent;

        // parse the entries from the line
        dstr_t txt = dstr_sub(&lines.data[i], indent, 0);
        LIST_VAR(dstr_t, entries, 4);
        PROP_GO( dstr_split(&txt, &DSTR_LIT(":"), &entries), cu_lines);
        // make sure we got all three entries
        if(entries.len != 4){
            LOG_ERROR("Unparsable line: %x\n", FD(&lines.data[i]));
            ORIG_GO(E_INTERNAL, "unparsable line", cu_lines);
        }

        // get UID
        uid_t uid;
        PROP_GO( get_uid(&entries.data[1], &uid), cu_lines);

        // get GID
        gid_t gid;
        PROP_GO( get_gid(&entries.data[2], &gid), cu_lines);

        // get mode
        if(entries.data[3].data[0] != '0'){
            LOG_ERROR("mode %x on file %x not in octal format\n");
            ORIG_GO(E_VALUE, "Invalid mode in permissions file", cu_lines);
        }
        mode_t mode;
        PROP_GO( dstr_tou(&entries.data[3], &mode), cu_lines);

        // decide if it is a file or a directory
        size_t namelen = entries.data[0].len;
        if(entries.data[0].data[namelen - 1] == '/'){
            // directory
            dstr_t dirname = dstr_sub(&entries.data[0], 0, namelen - 1);
            PROP_GO( directory_add_dir(parent, &dirname,
                                       uid, gid, mode), cu_lines);
            // save the directory we just pushed as last_line_dir
            last_line_dir = &parent->dirs.data[parent->dirs.len-1];
        }else{
            // file
            PROP_GO( directory_add_file(parent, &entries.data[0],
                                        uid, gid, mode), cu_lines);
            last_line_dir = NULL;
        }
    }

cu_lines:
    LIST_FREE(dstr_t, &lines);
    return error;
}


// static derr_t print_tree_hook(const string_builder_t* base, const dstr_t* file,
//                             bool isdir, void* userdata){
//     // dereference userdata
//     size_t level = *((size_t*)userdata);
//     // append filename to base
//     string_builder_t fullpath = sb_append(base, FD(file));
//     // stat for the existing permissions
//     struct stat s;
//     PROP( stat_path(&fullpath, &s) );
//
//     // prepare for printing indent
//     DSTR_PRESET(pre, "                                                   ");
//     pre.len = level * 2;
//     // handle regular files
//     if(!isdir){
//         PFMT("%x%x\n", FD(&pre), FD(file));
//     }
//     // handle directories
//     else{
//         PFMT("%x%x:\n", FD(&pre), FD(file));
//         // recurse into this directory
//         size_t newdata = level + 1;
//         PROP( for_each_file_in_dir2(&fullpath, print_tree_hook, (void*)&newdata) );
//     }
//     return E_OK;
// }
//
// static derr_t print_tree(const string_builder_t* path){
//     size_t data = 0;
//     PROP( for_each_file_in_dir2(path, print_tree_hook, (void*)&data) );
//     return E_OK;
// }
//
// static derr_t do_print_fs_tree(const dstr_t* base){
//
//     string_builder_t sb_base = sb_append(NULL, FD(base));
//     PROP( print_tree(&sb_base) );
//
//     return E_OK;
// }


// like common.c's in_list() but with an additional level of dereference
static inline bool in_dir_list(const dstr_t* val, const LIST(directory)* list,
                               size_t* idx){
    for(size_t i = 0; i < list->len; i++){
        int result = dstr_cmp(val, &list->data[i].name);
        if(result == 0){
            if(idx) *idx = i;
            return true;
        }
    }
    if(idx) *idx = (size_t) -1;
    return false;
}

// forward declaration
static derr_t check_tree(const string_builder_t* path,
                         const directory* perm_dir,
                         const string_builder_t* perm_path,
                         bool* ok);

typedef struct {
    const directory* perm_dir;
    const string_builder_t* perm_path;
    LIST(bool) dirs_found;
    LIST(bool) files_found;
    bool* ok;
} check_tree_data_t;

static derr_t check_tree_hook(const string_builder_t* base, const dstr_t* file,
                            bool isdir, void* userdata){
    // dereference userdata
    check_tree_data_t* data = (check_tree_data_t*)userdata;
    // append filename to base
    string_builder_t fullpath = sb_append(base, FD(file));
    // make sure regular files are 1:1
    if(!isdir){
        size_t idx;
        if(!in_list(file, &data->perm_dir->files, &idx)){
           LOG_ERROR("%x: not in permissions file\n",
                     FSB(&fullpath, &slash));
           *data->ok = false;
        }else{
            // mark file as found
            data->files_found.data[idx] = true;
        }
    }
    // make sure directories are 1:1
    else{
        size_t idx;
        if(!in_dir_list(file, &data->perm_dir->dirs, &idx)){
            LOG_ERROR("%x/: not found in permissions file\n",
                      FSB(&fullpath, &slash));
           *data->ok = false;
        }else{
            // mark dir as found
            data->dirs_found.data[idx] = true;
            // recurse into this directory
            directory* thisdir = &data->perm_dir->dirs.data[idx];
            string_builder_t thispath = sb_append(data->perm_path,
                                                  FD(&thisdir->name));
            PROP( check_tree(&fullpath, thisdir, &thispath, data->ok) );
        }
    }
    return E_OK;
}

static derr_t check_tree(const string_builder_t* path,
                         const directory* perm_dir,
                         const string_builder_t* perm_path,
                         bool* ok){
    derr_t error;
    // init data structure
    check_tree_data_t data = {.perm_dir = perm_dir,
                              .perm_path = perm_path,
                              .ok = ok};
    // allocate boolean lists
    PROP( LIST_NEW(bool, &data.dirs_found, perm_dir->dirs.len) );
    PROP_GO( LIST_NEW(bool, &data.files_found, perm_dir->files.len), cu_df);

    // mark all dirs and files as not found yet
    for(size_t i = 0; i < perm_dir->dirs.len; i++){
        PROP_GO( LIST_APPEND(bool, &data.dirs_found, false), cu_df);
    }
    for(size_t i = 0; i < perm_dir->files.len; i++){
        PROP_GO( LIST_APPEND(bool, &data.files_found, false), cu_df);
    }

    // make sure everything in the tree is in the permissions file
    PROP_GO( for_each_file_in_dir2(path, check_tree_hook, (void*)&data), cu_ff);

    // make sure everything in the permissions file was found in the tree
    for(size_t i = 0; i < data.dirs_found.len; i++){
        if(data.dirs_found.data[i] == false){
            string_builder_t tmp = sb_append(perm_path,
                                             FD(&perm_dir->dirs.data[i].name));
            LOG_ERROR("%x/: not found in overlay\n", FSB(&tmp, &slash));
            *ok = false;
        }
    }
    for(size_t i = 0; i < data.files_found.len; i++){
        if(data.files_found.data[i] == false){
            string_builder_t tmp = sb_append(perm_path,
                                             FD(&perm_dir->files.data[i]));
            LOG_ERROR("%x: not found in overlay\n", FSB(&tmp, &slash));
            *ok = false;
        }
    }

cu_ff:
    LIST_FREE(bool, &data.files_found);
cu_df:
    LIST_FREE(bool, &data.dirs_found);
    return error;
}


// static derr_t mkdir_recurse(const directory* dir,
//                             const string_builder_t* dest){
//     // make sure all subdirectories exist
//     for(size_t i = 0; i < dir->dirs.len; i++){
//         directory* subdir = &dir->dirs.data[i];
//         string_builder_t subpath = sb_append(dest, FD(&subdir->name));
//         PROP( mkdir_path(&subpath, dir->dir_modes.data[i], true) );
//         // then recurse into every subdirectory
//         PROP( mkdir_recurse(subdir, &subpath) );
//     }
//     return E_OK;
// }


static derr_t read_perms_file(const string_builder_t* permsfile,
                              dstr_t* perms, directory* perm_dir){
    derr_t error;
    // read permissions file
    PROP( dstr_new(perms, 4096) );
    PROP_GO( dstr_fread_path(permsfile, perms), fail_perms);
    // ignore the trailing newline
    if(perms->data[perms->len - 1] == '\n') perms->len -= 1;
    // base of the perm_dir; a nameless directory
    PROP_GO( directory_new(perm_dir, &DSTR_LIT("")), fail_perms);
    // parse perms file
    PROP_GO( parse_perms(perms, perm_dir), fail_perm_dir);
    return E_OK;

fail_perm_dir:
    directory_free(perm_dir);
fail_perms:
    dstr_free(perms);
    return error;
}


// static derr_t olt_preinstall(const char* permsfile, const char* overlay,
//                              const char* dest, int* retval){
//     derr_t error;
//     // allocate memory and read/parse permissions file
//     dstr_t perms;
//     directory perm_dir;
//     PROP( read_perms_file(&SB(FS(permsfile)), &perms, &perm_dir) );
//
//     // check the perms file against the actual file overlay
//     bool ok = true;
//     string_builder_t overlay_sb = sb_append(NULL, FS(overlay));
//     PROP_GO( check_tree(&overlay_sb, &perm_dir, NULL, &ok), cu);
//
//     // make sure that we are OK up to here
//     if(!ok){
//         LOG_ERROR("RESULT: permissions/overlay mismatch\n");
//         *retval = 2;
//         goto cu;
//     }
//
//     // then create the skeleton of directories, for the installer
//     string_builder_t dest_sb = sb_append(NULL, FS(dest));
//     PROP_GO( mkdir_recurse(&perm_dir, &dest_sb), cu);
//
// cu:
//     directory_free(&perm_dir);
//     dstr_free(&perms);
//     return error;
// }


static derr_t dir_is_empty(const string_builder_t* path, bool* dir_empty){
    // we are going to open the directory, and check if it has even one file
    derr_t error = E_OK;
    *dir_empty = true;
    DIR* d;
    PROP( opendir_path(path, &d) );
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
        if(strncmp(entry->d_name, dot, 2) == 0) continue;
        if(strncmp(entry->d_name, dotdot, 3) == 0) continue;
        *dir_empty = false;
        break;
    }
cleanup:
    closedir(d);
    return error;
}


static derr_t delete_from_perms(const directory* del, const directory* keep,
                                const string_builder_t* dest){
    // delete all files, unless they are in the keep directory
    for(size_t i = 0; i < del->files.len; i++){
        dstr_t* f = &del->files.data[i];
        string_builder_t fpath = sb_append(dest, FD(f));
        // skip files in "keep", if keep is defined
        // Preformance note: check memory before system call
        if(keep != NULL && in_list(f, &keep->files, NULL))
            continue;
        // check if the file exists in the destination
        bool fexists;
        PROP( exists_path(&fpath, &fexists) );
        // if it exists, remove it
        if(fexists){
            LOG_DEBUG("deleting file %x\n", FSB(&fpath, &slash));
            PROP( remove_path(&fpath) );
        }
    }
    // recurse into each sub directory, and possibly delete it
    for(size_t i = 0; i < del->dirs.len; i++){
        directory* d = &del->dirs.data[i];
        string_builder_t dpath = sb_append(dest, FD(&d->name));
        // if the directory doesn't exist, just skip this
        // Preformance note: always need a syscall, so don't check memory first
        bool dexists;
        PROP( exists_path(&dpath, &dexists) );
        if(!dexists) continue;
        // skip files in "keep", if keep is defined
        size_t idx;
        // in_keep is false if keep == NULL
        bool in_keep = keep ? in_dir_list(&d->name, &keep->dirs, &idx) : false;
        if(in_keep){
            delete_from_perms(d, &keep->dirs.data[idx], &dpath);
        }else{
            // if subdir not in keep, delete everything in it
            delete_from_perms(d, NULL, &dpath);
            // then trim empty directories
            bool dir_empty;
            PROP( dir_is_empty(&dpath, &dir_empty) );
            if(dir_empty){
                LOG_DEBUG("triming empty dir %x\n", FSB(&dpath, &slash));
                PROP( remove_path(&dpath) );
            }
        }
    }
    return E_OK;
}


static derr_t handle_deletions(const char* permsfile, const directory* perms,
                               const string_builder_t* dest){
    derr_t error;
    // get the name of the old permissions file
    dstr_t oldpf;
    PROP( dstr_new(&oldpf, strlen(permsfile) + 5) );
    PROP_GO( FMT(&oldpf, "%x.old", FS(permsfile)), cu_oldpf);

    // if old file exists, proceed with deleteions
    if(exists(oldpf.data)){
        // make sure we can read/write the file
        if(!file_rw_access(oldpf.data)){
            ORIG_GO(E_FS, "no access to old permissions file", cu_oldpf);
        }
        // read/parse the old permissions file
        dstr_t old_perms;
        directory old_perm_dir;
        PROP_GO( read_perms_file(&SB(FD(&oldpf)), &old_perms, &old_perm_dir), cu_oldpf);

        // delete from old_perms where not in perms, recursivley
        PROP_GO( delete_from_perms(&old_perm_dir, perms, dest), cu_oldperms);

    cu_oldperms:
        directory_free(&old_perm_dir);
        dstr_free(&old_perms);
        if(error) goto cu_oldpf;
    }

cu_oldpf:
    dstr_free(&oldpf);
    return error;
}


static inline bool is_newer(struct stat* a, struct stat* b){
    // first compare seconds
    if(a->st_mtim.tv_sec > b->st_mtim.tv_sec) return true;
    // if seconds are equal, compare nanoseconds
    if(a->st_mtim.tv_sec == b->st_mtim.tv_sec)
        return a->st_mtim.tv_nsec > b->st_mtim.tv_nsec;
    // otherwise, b is newer
    return false;
}


static derr_t handle_install(const directory* perms,
                             const string_builder_t* overlay,
                             const string_builder_t* dest){
    for(size_t i = 0; i < perms->files.len; i++){
        // get the information we need for this file
        dstr_t* f = &perms->files.data[i];
        string_builder_t path = sb_append(dest, FD(f));
        uid_t uid = perms->file_uids.data[i];
        gid_t gid = perms->file_gids.data[i];
        mode_t mode = perms->file_modes.data[i];
        // also get the full path of the corresponding overlay file
        string_builder_t overlay_path = sb_append(overlay, FD(f));
        // check if file already exists
        struct stat s;
        int eno;
        PROP( stat_path(&path, &s, &eno) );
        if(eno == 0){
            // someting exists, make sure it's a regular file
            if(S_ISDIR(s.st_mode)){
                /* if it is not a regular file, and we didn't delete in during
                   the deletion phase, just stop. */
                LOG_ERROR("a directory exists at %x, can't install file\n",
                          FSB(&path, &slash));
                ORIG(E_FS, "a directory exists, can't install file");
            }
            // make sure file contents are up-to-date
            struct stat overlay_s;
            PROP( stat_path(&overlay_path, &overlay_s, NULL) );
            if(is_newer(&overlay_s, &s)){
                // file copy (mode doesn't matter, file already exists)
                LOG_DEBUG("instl %x from %x\n",
                          FSB(&path, &slash), FSB(&overlay_path, &slash));
                PROP( file_copy_path(&overlay_path, &path, 0000) );
            }
            // make sure owner is correct
            if(s.st_uid != uid || s.st_gid != gid){
                LOG_DEBUG("chown %x\n", FSB(&path, &slash));
                PROP( chown_path(&path, uid, gid) );
            }
            // make sure mode is correct
            if((s.st_mode & 0777) != mode){
                LOG_DEBUG("chmod %x from %x to %x\n", FSB(&path, &slash),
                          FI(s.st_mode), FI(mode));
                PROP( chmod_path(&path, mode) );
            }
        }else if(eno == ENOENT){
            // installing a file that hasn't been there before
            LOG_DEBUG("new file: %x\n", FSB(&path, &slash));
            // file does not exist, create it (with restrictive permissions)
            PROP( file_copy_path(&overlay_path, &path, 0000) );
            // set mode with chmod (open() in file_copy() applies a umask)
            PROP( chmod_path(&path, mode) );
            // set owner
            PROP( chown_path(&path, uid, gid) );
        }else{
            // any other error from stat():
            LOG_ERROR("failed to stat %x: %x\n", FSB(&path, &slash), FE(&eno));
            ORIG(E_FS, "stat failed");
        }
    }
    for(size_t i = 0; i < perms->dirs.len; i++){
        // get the information we need for this subdirectory
        directory* d = &perms->dirs.data[i];
        string_builder_t path = sb_append(dest, FD(&d->name));
        uid_t uid = perms->dir_uids.data[i];
        gid_t gid = perms->dir_gids.data[i];
        mode_t mode = perms->dir_modes.data[i];
        // make sure the subdirectory exists and is a directory
        struct stat s;
        int eno;
        PROP( stat_path(&path, &s, &eno) );
        if(eno == 0){
            // someting exists, make sure it's a directory
            if(!S_ISDIR(s.st_mode)){
                /* if it is not a directory, and we didn't delete in during
                   the deletion phase, just stop. */
                LOG_ERROR("a regular file exists at %x, can't mkdir\n",
                          FSB(&path, &slash));
                ORIG(E_FS, "a regular file exists, can't mkdir");
            }
            // make sure owner is correct
            if(s.st_uid != uid || s.st_gid != gid){
                LOG_DEBUG("chown %x/\n", FSB(&path, &slash));
                PROP( chown_path(&path, uid, gid) );
            }
            // make sure mode is correct
            if((s.st_mode & 0777) != mode){
                LOG_DEBUG("chmod %x/\n", FSB(&path, &slash));
                PROP( chmod_path(&path, mode) );
            }
        }else if(eno == ENOENT){
            LOG_DEBUG("new directory: %x/\n", FSB(&path, &slash));
            // directory does not exist, create it
            PROP( mkdir_path(&path, mode, false) );
            // set mode with chmod (mkdir applies a umask)
            PROP( chmod_path(&path, mode) );
            // set owner
            PROP( chown_path(&path, uid, gid) );
        }else{
            // any other error from stat():
            LOG_ERROR("failed to stat %x: %x\n", FSB(&path, &slash), FE(&eno));
            ORIG(E_FS, "stat failed");
        }
        // now, recurse into the directory
        string_builder_t overlay_sub = sb_append(overlay, FD(&d->name));
        string_builder_t dest_sub = sb_append(dest, FD(&d->name));
        PROP( handle_install(d, &overlay_sub, &dest_sub) );
    }
    return E_OK;
}


static derr_t save_permissions(const char* permsfile){
    derr_t error;
    // get the name of the old permissions file (which we will save to)
    dstr_t oldpf;
    PROP( dstr_new(&oldpf, strlen(permsfile) + 5) );
    PROP_GO( FMT(&oldpf, "%x.old", FS(permsfile)), cu_oldpf);

    // copy the permissions file to its new location
    PROP_GO( file_copy(permsfile, oldpf.data, 0644), cu_oldpf);

cu_oldpf:
    dstr_free(&oldpf);
    return error;
}


static derr_t do_olt(const char* permsfile, const char* overlay,
                     const char* dest, bool check_mode, int* retval){
    derr_t error;
    // allocate memory and read/parse permissions file
    dstr_t perms;
    directory perm_dir;
    PROP( read_perms_file(&SB(FS(permsfile)), &perms, &perm_dir) );

    // check the perms file against the actual file overlay
    bool ok = true;
    string_builder_t overlay_sb = sb_append(NULL, FS(overlay));
    PROP_GO( check_tree(&overlay_sb, &perm_dir, NULL, &ok), cu);

    // make sure that we are OK up to here
    if(!ok){
        LOG_ERROR("RESULT: permissions/overlay mismatch\n");
        *retval = 2;
        goto cu;
    }

    if(check_mode) goto cu;

    // make string_builder from destination
    string_builder_t dest_sb = sb_append(NULL, FS(dest));

    // handle deletions
    PROP_GO( handle_deletions(permsfile, &perm_dir, &dest_sb), cu);

    // handle the install
    PROP_GO( handle_install(&perm_dir, &overlay_sb, &dest_sb), cu);

    // save the current permissions file so we can detect deletions next time
    PROP_GO( save_permissions(permsfile), cu);

cu:
    directory_free(&perm_dir);
    dstr_free(&perms);
    return error;
}

static derr_t setup_uid_gid_cache(void){
    derr_t error;
    PROP( LIST_NEW(dstr_t, &uid_cache, 32) );
    PROP_GO( LIST_NEW(uid_t, &uid_results, 32), cu_uid_cache);
    PROP_GO( LIST_NEW(dstr_t, &gid_cache, 32), cu_uid_results);
    PROP_GO( LIST_NEW(gid_t, &gid_results, 32), cu_gid_cache);
    return E_OK;

cu_gid_cache:
    LIST_FREE(dstr_t, &gid_cache);
cu_uid_results:
    LIST_FREE(uid_t, &uid_results);
cu_uid_cache:
    LIST_FREE(dstr_t, &uid_cache);
    return error;
}

static void free_uid_gid_cache(void){
    LIST_FREE(gid_t, &gid_results);
    LIST_FREE(dstr_t, &gid_cache);
    LIST_FREE(uid_t, &uid_results);
    LIST_FREE(dstr_t, &uid_cache);
}

static void print_help(void){
    LOG_ERROR("usage: olt [-d|--debug] PERMS OVERLAY      "
                                            "# check mode; takes no action\n");
    LOG_ERROR("usage: olt [-d|--debug] PERMS OVERLAY DEST # install mode\n");
}

int main(int argc, char** argv){
    derr_t error;

    // check for debug option on command line
    opt_spec_t o_debug = {'d', "debug", false, OPT_RETURN_INIT};
    opt_spec_t* spec[] = {&o_debug};
    size_t speclen = sizeof(spec) / sizeof(*spec);
    int newargc;
    if(opt_parse(argc, argv, spec, speclen, &newargc)){
        print_help();
        return 1;
    }

    logger_add_fileptr(o_debug.found ? LOG_LVL_DEBUG : LOG_LVL_INFO, stderr);

    const char* permsarg;
    const char* overlayarg;
    const char* destarg;
    bool check_mode;

    int retval = 0;

    if(newargc == 3){
        // check mode
        permsarg = argv[1];
        overlayarg = argv[2];
        destarg = NULL;
        check_mode = true;
    }else if(newargc == 4){
        // install mode
        permsarg = argv[1];
        overlayarg = argv[2];
        destarg = argv[3];
        check_mode = false;
    }else{
        print_help();
        return 1;
    }

    // setup global cache
    PROP_GO( setup_uid_gid_cache(), exit);

    // take action
    PROP_GO( do_olt(permsarg, overlayarg, destarg, check_mode, &retval), cu);

cu:
    free_uid_gid_cache();
exit:
    return error == E_OK ? retval : 255;
}
