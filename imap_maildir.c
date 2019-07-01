#include "imap_maildir.h"
#include "logger.h"
#include "fileops.h"

// check for "cur" and "new" and "tmp" subdirs, "true" means "all present"
static derr_t ctn_check(const string_builder_t *path, bool *ret){
    derr_t e = E_OK;
    char *ctn[3] = {"cur", "tmp", "new"};
    *ret = true;
    for(size_t i = 0; i < 3; i++){
        string_builder_t subdir_path = sb_append(path, FS(ctn[i]));
        bool temp;
        PROP(&e, dir_rw_access_path(&subdir_path, false, &temp) );
        *ret &= temp;
    }
    return e;
}

// check each child directory
static derr_t find_children(const string_builder_t *base, const dstr_t *name,
                            bool isdir, void *data){
    derr_t e = E_OK;
    // don't care about any non-directories here
    if(!isdir) return e;
    // also ignore subdirs named "cur", "tmp", and "new"
    if(dstr_cmp(name, &DSTR_LIT("cur")) == 0) return e;
    if(dstr_cmp(name, &DSTR_LIT("tmp")) == 0) return e;
    if(dstr_cmp(name, &DSTR_LIT("new")) == 0) return e;
    // dereference argument, the parent maildir
    imaildir_t *parent = data;
    // use parent->path explicitly, not via *base
    (void)base;
    // mark parent as having inferiors
    parent->mflags.noinferiors = false;
    // open the maildir
    imaildir_t *m;
    PROP_GO(&e, imaildir_new(&m, &parent->path, name), fail_malloc);
    // put this child into the parent's hashmap
    PROP_GO(&e, hashmap_puts(&parent->children, &m->name, &m->h), fail_open);

    return e;

fail_open:
    imaildir_free(m);
fail_malloc:
    free(m);
    return e;
}

derr_t imaildir_new(imaildir_t **maildir, const string_builder_t *path,
                    const dstr_t *name){
    derr_t e = E_OK;
    // allocate the pointer
    imaildir_t *m = malloc(sizeof(*m));
    *maildir = m;
    if(!m) ORIG(&e, E_NOMEM, "unable to malloc");
    // set self-references
    m->h.data = m;
    // copy the name
    m->name = (dstr_t){0};
    PROP_GO(&e, dstr_copy(name, &m->name), fail_malloc);
    // build path with name
    m->path = sb_append(path, FD(&m->name));
    // check for cur/new/tmp folders, and assign /NOSELECT accordingly
    bool ctn_present;
    PROP_GO(&e, ctn_check(&m->path, &ctn_present), fail_name);
    m->mflags = (ie_mflag_list_t){.noselect = !ctn_present};
    // init hashmap of messages (we will populate later, after SELECT command)
    PROP_GO(&e, hashmap_init(&m->msgs), fail_name);
    // init hashmap of children
    PROP_GO(&e, hashmap_init(&m->children), fail_msgs);
    // search for child folders
    m->mflags.noinferiors = true;
    PROP_GO(&e, for_each_file_in_dir2(&m->path, find_children, m), fail_chld);
    // set other initial values
    m->refs = 0;
    m->del_plan = IMDP_DONT_DELETE;
    // TODO: UID_VALIDITY
    // m->uid_validity = ???

    return e;

    hashmap_iter_t i;
fail_chld:
    // close all the child maildirs
    for(i = hashmap_pop_first(&m->children); i.more; hashmap_pop_next(&i)){
        imaildir_free((imaildir_t*)i.data);
    }
    // then free the hashmap
    hashmap_free(&m->children);
fail_msgs:
    hashmap_free(&m->msgs);
fail_name:
    dstr_free(&m->name);
fail_malloc:
    free(m);
    *maildir = NULL;
    return e;
}

void imaildir_free(imaildir_t *m){
    // first free all the children (yay!! free the children!)
    hashmap_iter_t i;
    for(i = hashmap_pop_first(&m->children); i.more; hashmap_pop_next(&i)){
        imaildir_free((imaildir_t*)i.data);
    }
    hashmap_free(&m->children);
    // free all the messages
    for(i = hashmap_pop_first(&m->msgs); i.more; hashmap_pop_next(&i)){
        // TODO: what does this look like?
        // imsg_close((imsg_t*)i.data);
    }
    hashmap_free(&m->msgs);
    // free name
    dstr_free(&m->name);
    // free pointer
    free(m);
}
