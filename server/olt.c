#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "libdstr/libdstr.h"

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
static LIST(dstr_t) uid_cache;
static LIST(uid_t) uid_results;
static LIST(dstr_t) gid_cache;
static LIST(gid_t) gid_results;


static derr_t get_uid(const dstr_t* name, uid_t* uid){
    derr_t e = E_OK;
    // first check if the name is in the cache
    size_t cached_result;
    if(in_list(name, &uid_cache, &cached_result)){
        *uid = uid_results.data[cached_result];
        return e;
    }

    // get c-string for the name
    DSTR_VAR(cname, 128);
    PROP(&e, FMT(&cname, "%x", FD(name)) );

    // need a buffer for re-entrant UID/GID functions
    char buffer[1024];

    // now get the UID for that name
    struct passwd pwd;
    struct passwd *pwdret;
    int ret = getpwnam_r(cname.data, &pwd, buffer, sizeof(buffer), &pwdret);
    if(ret != 0){
        TRACE(&e, "getpwnam_r(%x): %x\n", FD(name), FE(&errno));
        ORIG(&e, E_OS, "unable to get UID from username");
    }
    if(pwdret == NULL){
        TRACE(&e, "user %x not found\n", FD(name));
        ORIG(&e, E_VALUE, "user not found");
    }
    *uid = pwdret->pw_uid;

    // cache the result
    PROP(&e, LIST_APPEND(dstr_t, &uid_cache, *name) );
    PROP(&e, LIST_APPEND(uid_t, &uid_results, pwdret->pw_uid) );

    return e;
}


static derr_t get_gid(const dstr_t* name, gid_t* gid){
    derr_t e = E_OK;
    // first check if the name is in the cache
    size_t cached_result;
    if(in_list(name, &gid_cache, &cached_result)){
        *gid = gid_results.data[cached_result];
        return e;
    }

    // get c-string for the name
    DSTR_VAR(cname, 128);
    PROP(&e, FMT(&cname, "%x", FD(name)) );

    // need a buffer for re-entrant UID/GID functions
    char buffer[1024];

    // now get the GID for that name
    struct group grp;
    struct group *grpret;
    int ret = getgrnam_r(cname.data, &grp, buffer, sizeof(buffer), &grpret);
    if(ret != 0){
        TRACE(&e, "getpwnam_r(%x): %x\n", FD(name), FE(&errno));
        ORIG(&e, E_OS, "unable to get GID from groupname");
    }
    if(grpret == NULL){
        TRACE(&e, "group %x not found\n", FD(name));
        ORIG(&e, E_VALUE, "group not found");
    }
    *gid = grpret->gr_gid;

    // cache the result
    PROP(&e, LIST_APPEND(dstr_t, &gid_cache, *name) );
    PROP(&e, LIST_APPEND(gid_t, &gid_results, grpret->gr_gid) );

    return e;
}


static derr_t directory_new(directory* dir, const dstr_t* name){
    derr_t e = E_OK;
    dir->name = *name;
    PROP(&e, LIST_NEW(directory, &dir->dirs, 8) );
    PROP_GO(&e, LIST_NEW(uid_t, &dir->dir_uids, 8), fail_dirs);
    PROP_GO(&e, LIST_NEW(gid_t, &dir->dir_gids, 8), fail_dir_uids);
    PROP_GO(&e, LIST_NEW(mode_t, &dir->dir_modes, 8), fail_dir_gids);
    PROP_GO(&e, LIST_NEW(dstr_t, &dir->files, 8), fail_dir_modes);
    PROP_GO(&e, LIST_NEW(uid_t, &dir->file_uids, 8), fail_files);
    PROP_GO(&e, LIST_NEW(gid_t, &dir->file_gids, 8), fail_file_uids);
    PROP_GO(&e, LIST_NEW(mode_t, &dir->file_modes, 8), fail_file_gids);
    return e;

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
    return e;
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
    derr_t e = E_OK;
    // first grow all of the lists
    PROP(&e, LIST_GROW(dstr_t, &dir->files, dir->files.len + 1) );
    PROP(&e, LIST_GROW(uid_t, &dir->file_uids, dir->file_uids.len + 1) );
    PROP(&e, LIST_GROW(gid_t, &dir->file_gids, dir->file_gids.len + 1) );
    PROP(&e, LIST_GROW(mode_t, &dir->file_modes, dir->file_modes.len + 1) );
    // then append to all of the lists
    LIST_APPEND(dstr_t, &dir->files, *file);
    LIST_APPEND(uid_t, &dir->file_uids, uid);
    LIST_APPEND(gid_t, &dir->file_gids, gid);
    LIST_APPEND(mode_t, &dir->file_modes, mode);
    return e;
}


static derr_t directory_add_dir(directory* dir, const dstr_t* name,
                                uid_t uid, gid_t gid, mode_t mode){
    derr_t e = E_OK;
    // first grow all of the lists
    PROP(&e, LIST_GROW(directory, &dir->dirs, dir->dirs.len + 1) );
    PROP(&e, LIST_GROW(uid_t, &dir->dir_uids, dir->dir_uids.len + 1) );
    PROP(&e, LIST_GROW(gid_t, &dir->dir_gids, dir->dir_gids.len + 1) );
    PROP(&e, LIST_GROW(mode_t, &dir->dir_modes, dir->dir_modes.len + 1) );
    // the allocate the directory (operate on the list.data value)
    PROP(&e, directory_new(&dir->dirs.data[dir->dirs.len], name) );
    dir->dirs.len += 1;
    // then append to all of the lists
    LIST_APPEND(uid_t, &dir->dir_uids, uid);
    LIST_APPEND(gid_t, &dir->dir_gids, gid);
    LIST_APPEND(mode_t, &dir->dir_modes, mode);
    return e;
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
    derr_t e = E_OK;
    // split file into lines
    LIST(dstr_t) lines;
    DSTR_STATIC(newline, "\n");
    PROP(&e, LIST_NEW(dstr_t, &lines, dstr_count(perms, &newline)) );
    PROP_GO(&e, dstr_split(perms, &newline, &lines), cu_lines);
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
            TRACE(&e, "bad indent (%x): %x\n", FU(indent), FD(&lines.data[i]));
            ORIG_GO(&e, E_INTERNAL, "bad indent", cu_lines);
        }
        // check if indent has increased
        if(indent > last_indent){
            // make sure the last line was a directory
            if(last_line_dir == NULL){
                TRACE(&e, "can't increase indent here: %x\n", FD(&lines.data[i]));
                ORIG_GO(&e, E_VALUE, "can't increase indent here", cu_lines);
            }
            // make sure the indentation increased by exactly 4
            if(indent - last_indent != 4){
                TRACE(&e, "indent jump: %x\n", FD(&lines.data[i]));
                ORIG_GO(&e, E_VALUE, "indent jump", cu_lines);
            }
            // now push the last dir onto our dpstack
            PROP_GO(&e, LIST_APPEND(directory_p, &dpstack, last_line_dir), cu_lines);
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
        PROP_GO(&e, dstr_split(&txt, &DSTR_LIT(":"), &entries), cu_lines);
        // make sure we got all three entries
        if(entries.len != 4){
            TRACE(&e, "Unparsable line: %x\n", FD(&lines.data[i]));
            ORIG_GO(&e, E_INTERNAL, "unparsable line", cu_lines);
        }

        // get UID
        uid_t uid = 0;
        PROP_GO(&e, get_uid(&entries.data[1], &uid), cu_lines);

        // get GID
        gid_t gid = 0;
        PROP_GO(&e, get_gid(&entries.data[2], &gid), cu_lines);

        // get mode
        if(entries.data[3].data[0] != '0'){
            TRACE(&e, "mode %x on file %x not in octal format\n");
            ORIG_GO(&e, E_VALUE, "Invalid mode in permissions file", cu_lines);
        }
        mode_t mode;
        PROP_GO(&e, dstr_tou(&entries.data[3], &mode, 8), cu_lines);

        // decide if it is a file or a directory
        size_t namelen = entries.data[0].len;
        if(entries.data[0].data[namelen - 1] == '/'){
            // directory
            dstr_t dirname = dstr_sub(&entries.data[0], 0, namelen - 1);
            PROP_GO(&e, directory_add_dir(parent, &dirname,
                                       uid, gid, mode), cu_lines);
            // save the directory we just pushed as last_line_dir
            last_line_dir = &parent->dirs.data[parent->dirs.len-1];
        }else{
            // file
            PROP_GO(&e, directory_add_file(parent, &entries.data[0],
                                        uid, gid, mode), cu_lines);
            last_line_dir = NULL;
        }
    }

cu_lines:
    LIST_FREE(dstr_t, &lines);
    return e;
}


// like common.c's in_list() but with an additional level of dereference
static inline bool in_dir_list(
    const dstr_t* val, const LIST(directory)* list, size_t* idx
){
    if(idx) *idx = 0;
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
    derr_t e = E_OK;
    // dereference userdata
    check_tree_data_t* data = (check_tree_data_t*)userdata;
    // append filename to base
    string_builder_t fullpath = sb_append(base, FD(file));
    // make sure regular files are 1:1
    if(!isdir){
        size_t idx = 0;
        if(!in_list(file, &data->perm_dir->files, &idx)){
           LOG_ERROR("%x: not in permissions file\n",
                     FSB(&fullpath, &slash));
           *data->ok = false;
        }else{
            // make sure links have valid permissions (0777)
            struct stat s;
            PROP(&e, lstat_path(&fullpath, &s, NULL) );
            if(S_ISLNK(s.st_mode)){
                if(data->perm_dir->file_modes.data[idx] != 0777){
                    LOG_ERROR("%x: link needs 0777 permissions\n",
                              FSB(&fullpath, &slash));
                    *data->ok = false;
                }
            }
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
            PROP(&e, check_tree(&fullpath, thisdir, &thispath, data->ok) );
        }
    }
    return e;
}

static derr_t check_tree(const string_builder_t* path,
                         const directory* perm_dir,
                         const string_builder_t* perm_path,
                         bool* ok){
    derr_t e = E_OK;
    // init data structure
    check_tree_data_t data = {.perm_dir = perm_dir,
                              .perm_path = perm_path,
                              .ok = ok};
    // allocate boolean lists
    PROP(&e, LIST_NEW(bool, &data.dirs_found, perm_dir->dirs.len) );
    PROP_GO(&e, LIST_NEW(bool, &data.files_found, perm_dir->files.len), cu_df);

    // mark all dirs and files as not found yet
    for(size_t i = 0; i < perm_dir->dirs.len; i++){
        PROP_GO(&e, LIST_APPEND(bool, &data.dirs_found, false), cu_df);
    }
    for(size_t i = 0; i < perm_dir->files.len; i++){
        PROP_GO(&e, LIST_APPEND(bool, &data.files_found, false), cu_df);
    }

    // make sure everything in the tree is in the permissions file
    PROP_GO(&e, for_each_file_in_dir2(path, check_tree_hook, (void*)&data), cu_ff);

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
    return e;
}


static derr_t read_perms_file(const string_builder_t* permsfile,
                              dstr_t* perms, directory* perm_dir){
    derr_t e = E_OK;
    // read permissions file
    PROP(&e, dstr_new(perms, 4096) );
    PROP_GO(&e, dstr_read_path(permsfile, perms), fail_perms);
    // ignore the trailing newline
    if(perms->data[perms->len - 1] == '\n') perms->len -= 1;
    // base of the perm_dir; a nameless directory
    PROP_GO(&e, directory_new(perm_dir, &DSTR_LIT("")), fail_perms);
    // parse perms file
    PROP_GO(&e, parse_perms(perms, perm_dir), fail_perm_dir);
    return e;

fail_perm_dir:
    directory_free(perm_dir);
fail_perms:
    dstr_free(perms);
    return e;
}


static derr_t dir_is_empty(const string_builder_t* path, bool* dir_empty){
    derr_t e = E_OK;
    // we are going to open the directory, and check if it has even one file
    *dir_empty = true;
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
        if(strncmp(entry->d_name, dot, 2) == 0) continue;
        if(strncmp(entry->d_name, dotdot, 3) == 0) continue;
        *dir_empty = false;
        break;
    }
cleanup:
    closedir(d);
    return e;
}


static derr_t delete_from_perms(const directory* del, const directory* keep,
                                const string_builder_t* dest, bool* update){
    derr_t e = E_OK;
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
        PROP(&e, exists_path(&fpath, &fexists) );
        // if it exists, remove it
        if(fexists){
            LOG_DEBUG("deleting file %x\n", FSB(&fpath, &slash));
            *update = true;
            PROP(&e, remove_path(&fpath) );
        }
    }
    // recurse into each sub directory, and possibly delete it
    for(size_t i = 0; i < del->dirs.len; i++){
        directory* d = &del->dirs.data[i];
        string_builder_t dpath = sb_append(dest, FD(&d->name));
        // if the directory doesn't exist, just skip this
        // Preformance note: always need a syscall, so don't check memory first
        bool dexists;
        PROP(&e, exists_path(&dpath, &dexists) );
        if(!dexists) continue;
        // skip files in "keep", if keep is defined
        size_t idx = 0;
        // in_keep is false if keep == NULL
        bool in_keep = keep ? in_dir_list(&d->name, &keep->dirs, &idx) : false;
        if(in_keep){
            delete_from_perms(d, &keep->dirs.data[idx], &dpath, update);
        }else{
            // if subdir not in keep, delete everything in it
            delete_from_perms(d, NULL, &dpath, update);
            // then trim empty directories
            bool dir_empty;
            PROP(&e, dir_is_empty(&dpath, &dir_empty) );
            if(dir_empty){
                LOG_DEBUG("triming empty dir %x\n", FSB(&dpath, &slash));
                *update = true;
                PROP(&e, remove_path(&dpath) );
            }
        }
    }
    return e;
}

static derr_t get_old_permissions_file(const char* permsfile, dstr_t* out){
    derr_t e = E_OK;
    // first append .old to the name
    out->len = 0;
    PROP(&e, FMT(out, "%x.old", FS(permsfile)) );
    // then convert all "/" to "_"
    for(size_t i = 0; i < out->len; i++){
        if(out->data[i] == '/'){
            out->data[i] = '_';
        }
    }
    return e;
}

static derr_t handle_deletions(const char* permsfile, const directory* perms,
                               const string_builder_t* dest, bool* update){
    derr_t e = E_OK;
    // get the name of the old permissions file
    dstr_t oldpf;
    PROP(&e, dstr_new(&oldpf, strlen(permsfile) + 5) );
    PROP_GO(&e, get_old_permissions_file(permsfile, &oldpf), cu_oldpf);

    // if old file exists, proceed with deleteions
    if(exists(oldpf.data)){
        // make sure we can read/write the file
        if(!file_rw_access(oldpf.data)){
            ORIG_GO(&e, E_FS, "no access to old permissions file", cu_oldpf);
        }
        // read/parse the old permissions file
        dstr_t old_perms;
        directory old_perm_dir;
        PROP_GO(&e, read_perms_file(&SB(FD(&oldpf)), &old_perms, &old_perm_dir), cu_oldpf);

        // delete from old_perms where not in perms, recursivley
        PROP_GO(&e, delete_from_perms(&old_perm_dir, perms, dest, update), cu_oldperms);

    cu_oldperms:
        directory_free(&old_perm_dir);
        dstr_free(&old_perms);
        if(is_error(e)) goto cu_oldpf;
    }

cu_oldpf:
    dstr_free(&oldpf);
    return e;
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


// separate function due to obnoxiousness of readlink() behavior
static inline derr_t install_symlink(const string_builder_t* readlinkfrom,
                                     const string_builder_t* link,
                                     bool rm_first){
    derr_t e = E_OK;
    if(rm_first){
        PROP(&e, remove_path(link) );
    }
    // now get the linkto from the original link, no matter how long it is
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* linkto;
    PROP(&e, readlink_path(readlinkfrom, &stack, &heap, &linkto) );

    // now install the new readlink
    LOG_DEBUG("    %x -> %x\n", FSB(link, &slash), FD(linkto));
    PROP_GO(&e, symlink_path(&SB(FD(linkto)), link), cu_heap);

cu_heap:
    dstr_free(&heap);
    return e;
}


static derr_t handle_install(const directory* perms,
                             const string_builder_t* overlay,
                             const string_builder_t* dest,
                             bool* update,
                             bool force_install){
    derr_t e = E_OK;
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
        PROP(&e, lstat_path(&path, &s, &eno) );
        // we will need info about the overlay file as well
        struct stat overlay_s;
        PROP(&e, lstat_path(&overlay_path, &overlay_s, NULL) );
        if(eno == 0){
            // someting exists, make sure it's not a directory
            if(S_ISDIR(s.st_mode)){
                /* if it is not a regular file, since we didn't delete it
                   during the deletion phase, just stop. */
                TRACE(&e, "a directory exists at %x, can't install file\n",
                          FSB(&path, &slash));
                ORIG(&e, E_FS, "a directory exists, can't install file");
            }
            // make sure file contents are up-to-date
            if(force_install || is_newer(&overlay_s, &s)){
                *update = true;
                if(S_ISREG(overlay_s.st_mode)){
                    LOG_DEBUG("install %x from %x\n",
                              FSB(&path, &slash), FSB(&overlay_path, &slash));
                    // file copy (mode doesn't matter, file already exists)
                    PROP(&e, file_copy_path(&overlay_path, &path, 0000) );
                }else if(S_ISLNK(overlay_s.st_mode)){
                    LOG_DEBUG("update %x based on %x\n",
                              FSB(&path, &slash), FSB(&overlay_path, &slash));
                    PROP(&e, install_symlink(&overlay_path, &path, true) );
                }else{
                    ORIG(&e, E_INTERNAL, "invalid file type");
                }
            }
            // make sure owner is correct
            if(s.st_uid != uid || s.st_gid != gid){
                LOG_DEBUG("chown %x\n", FSB(&path, &slash));
                *update = true;
                PROP(&e, lchown_path(&path, uid, gid) );
            }
            // make sure mode is correct (except for links)
            if(!S_ISLNK(overlay_s.st_mode) && (s.st_mode & 0777) != mode){
                LOG_DEBUG("chmod %x from %x to %x\n", FSB(&path, &slash),
                          FU(s.st_mode & 0777), FU(mode));
                *update = true;
                PROP(&e, chmod_path(&path, mode) );
            }
        }else if(eno == ENOENT){
            *update = true;
            // installing a file that hasn't been there before
            if(S_ISREG(overlay_s.st_mode)){
                // file does not exist, create it (with restrictive permissions)
                LOG_DEBUG("new file: %x\n", FSB(&path, &slash));
                PROP(&e, file_copy_path(&overlay_path, &path, 0000) );
            }else if(S_ISLNK(overlay_s.st_mode)){
                LOG_DEBUG("new link: %x\n", FSB(&path, &slash));
                PROP(&e, install_symlink(&overlay_path, &path, false) );
            }else{
                ORIG(&e, E_INTERNAL, "invalid file type");
            }
            // set mode with chmod (open() in file_copy() applies a umask)
            // (except chmod() doesn't work on symlinks)
            if(!S_ISLNK(overlay_s.st_mode)){
                PROP(&e, chmod_path(&path, mode) );
            }
            // set owner
            PROP(&e, lchown_path(&path, uid, gid) );
        }else{
            // any other error from stat():
            TRACE(&e, "failed to stat %x: %x\n", FSB(&path, &slash), FE(&eno));
            ORIG(&e, E_FS, "stat failed");
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
        PROP(&e, lstat_path(&path, &s, &eno) );
        if(eno == 0){
            // someting exists, make sure it's a directory
            if(!S_ISDIR(s.st_mode)){
                /* if it is not a directory, and we didn't delete in during
                   the deletion phase, just stop. */
                TRACE(&e, "a regular file exists at %x, can't mkdir\n",
                          FSB(&path, &slash));
                ORIG(&e, E_FS, "a regular file exists, can't mkdir");
            }
            // make sure owner is correct
            if(s.st_uid != uid || s.st_gid != gid){
                LOG_DEBUG("chown %x/\n", FSB(&path, &slash));
                *update = true;
                PROP(&e, chown_path(&path, uid, gid) );
            }
            // make sure mode is correct
            if((s.st_mode & 0777) != mode){
                LOG_DEBUG("chmod %x/\n", FSB(&path, &slash));
                *update = true;
                PROP(&e, chmod_path(&path, mode) );
            }
        }else if(eno == ENOENT){
            *update = true;
            LOG_DEBUG("new directory: %x/\n", FSB(&path, &slash));
            // directory does not exist, create it
            PROP(&e, mkdir_path(&path, mode, false) );
            // set mode with chmod (mkdir applies a umask)
            PROP(&e, chmod_path(&path, mode) );
            // set owner
            PROP(&e, chown_path(&path, uid, gid) );
        }else{
            // any other error from stat():
            TRACE(&e, "failed to stat %x: %x\n", FSB(&path, &slash), FE(&eno));
            ORIG(&e, E_FS, "stat failed");
        }
        // now, recurse into the directory
        string_builder_t overlay_sub = sb_append(overlay, FD(&d->name));
        string_builder_t dest_sub = sb_append(dest, FD(&d->name));
        PROP(&e, handle_install(d, &overlay_sub, &dest_sub, update, force_install) );
    }
    return e;
}


static derr_t save_permissions(const char* permsfile){
    derr_t e = E_OK;
    // get the name of the old permissions file (which we will save to)
    dstr_t oldpf;
    PROP(&e, dstr_new(&oldpf, strlen(permsfile) + 5) );
    PROP_GO(&e, get_old_permissions_file(permsfile, &oldpf), cu_oldpf);

    // copy the permissions file to its new location
    PROP_GO(&e, file_copy(permsfile, oldpf.data, 0644), cu_oldpf);

cu_oldpf:
    dstr_free(&oldpf);
    return e;
}


static derr_t do_olt(const char* permsfile, const char* overlay,
                     const char* dest, const char* stamp, bool check_mode,
                     int* retval){
    derr_t e = E_OK;
    // allocate memory and read/parse permissions file
    dstr_t perms;
    directory perm_dir;
    PROP(&e, read_perms_file(&SB(FS(permsfile)), &perms, &perm_dir) );

    // check the perms file against the actual file overlay
    bool ok = true;
    string_builder_t overlay_sb = sb_append(NULL, FS(overlay));
    PROP_GO(&e, check_tree(&overlay_sb, &perm_dir, NULL, &ok), cu);

    // make sure that we are OK up to here
    if(!ok){
        LOG_ERROR("RESULT: permissions/overlay mismatch\n");
        *retval = 2;
        goto cu;
    }

    if(check_mode) goto cu;

    // prepare for installation steps
    bool update = false;
    string_builder_t dest_sb = sb_append(NULL, FS(dest));

    // handle deletions
    PROP_GO(&e, handle_deletions(permsfile, &perm_dir, &dest_sb, &update), cu);

    // if a stamp was provided, but doesn't exist, install everything
    bool force_install = false;
    if(stamp && !exists(stamp)){
        force_install = true;
    }

    // handle the install
    PROP_GO(&e, handle_install(&perm_dir, &overlay_sb, &dest_sb,
                            &update, force_install), cu);

    // save the current permissions file so we can detect deletions next time
    PROP_GO(&e, save_permissions(permsfile), cu);

    // update the stamp, if necessary
    if(stamp && update){
        LOG_DEBUG("updating stamp\n");
        PROP_GO(&e, touch(stamp), cu);
    }

cu:
    directory_free(&perm_dir);
    dstr_free(&perms);
    return e;
}

static derr_t setup_uid_gid_cache(void){
    derr_t e = E_OK;
    PROP(&e, LIST_NEW(dstr_t, &uid_cache, 32) );
    PROP_GO(&e, LIST_NEW(uid_t, &uid_results, 32), cu_uid_cache);
    PROP_GO(&e, LIST_NEW(dstr_t, &gid_cache, 32), cu_uid_results);
    PROP_GO(&e, LIST_NEW(gid_t, &gid_results, 32), cu_gid_cache);
    return e;

cu_gid_cache:
    LIST_FREE(dstr_t, &gid_cache);
cu_uid_results:
    LIST_FREE(uid_t, &uid_results);
cu_uid_cache:
    LIST_FREE(dstr_t, &uid_cache);
    return e;
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
    LOG_ERROR("usage: olt [-d|--debug] PERMS OVERLAY DEST [STAMP] "
                                            "# install mode\n");
}

int main(int argc, char** argv){
    derr_t e = E_OK;

    // check for debug option on command line
    opt_spec_t o_debug = {'d', "debug", false, OPT_RETURN_INIT};
    opt_spec_t* spec[] = {&o_debug};
    size_t speclen = sizeof(spec) / sizeof(*spec);
    int newargc;
    IF_PROP(&e, opt_parse(argc, argv, spec, speclen, &newargc)){
        logger_add_fileptr(LOG_LVL_DEBUG, stderr);
        DUMP(e);
        DROP_VAR(&e);
        print_help();
        return 1;
    }

    logger_add_fileptr(o_debug.found ? LOG_LVL_DEBUG : LOG_LVL_INFO, stderr);

    const char* perms;
    const char* overlay;
    const char* dest;
    const char* stamp;
    bool check_mode;

    int retval = 0;

    if(newargc == 3){
        // check mode
        perms = argv[1];
        overlay = argv[2];
        dest = NULL;
        stamp = NULL;
        check_mode = true;
    }else if(newargc == 4 || newargc == 5){
        // install mode
        perms = argv[1];
        overlay = argv[2];
        dest = argv[3];
        stamp = (newargc == 5) ? argv[4] : NULL;
        check_mode = false;
    }else{
        print_help();
        DROP_VAR(&e);
        return 1;
    }

    // setup global cache
    PROP_GO(&e, setup_uid_gid_cache(), exit);

    // take action
    PROP_GO(&e, do_olt(perms, overlay, dest, stamp, check_mode, &retval), cu);

cu:
    free_uid_gid_cache();
exit:
    retval = is_error(e) ? 255 : retval;
    DUMP(e);
    DROP_VAR(&e);
    return retval;
}
