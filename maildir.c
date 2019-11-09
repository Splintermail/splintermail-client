#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>

#include "maildir.h"
#include "logger.h"
#include "fileops.h"

#include "win_compat.h"

#define HOSTNAME_COMPONENT_MAX_LEN 32

// mkdir maildir/{cur,new,tmp}
/* throws: E_FS (path too long)
           E_OS (unable to create folder)
           E_INTERNAL*/
static derr_t maildir_create(const dstr_t* path){
    derr_t e = E_OK;
    derr_t e2;

    // null terminate path
    // TODO: this should not be necessary
    DSTR_VAR(root, 4096);
    e2 = FMT(&root, "%x", FD(path));
    CATCH(e2, E_FIXEDSIZE){
        RETHROW(&e, &e2, E_FS);
    }else PROP(&e, e2);

    // create maildir
    int ret = mkdir(root.data, 0770);
    if(ret && errno != EEXIST){
        TRACE(&e, "%x: %x\n", FS(root.data), FE(&errno));
        ORIG(&e, E_OS, "unable to make maildir");
    }

    // create maildir/cur
    DSTR_VAR(cur, 4096);
    e2 = FMT(&cur, "%x/cur", FD(path));
    CATCH(e2, E_FIXEDSIZE){
        RETHROW(&e, &e2, E_FS);
    }else PROP_GO(&e, e2, cleanup_1);

    ret = mkdir(cur.data, 0770);
    if(ret && errno != EEXIST){
        TRACE(&e, "%x: %x\n", FS(cur.data), FE(&errno));
        ORIG_GO(&e, E_OS, "unable to make maildir/cur", cleanup_1);
    }

    // create maildir/new
    DSTR_VAR(new, 4096);
    e2 = FMT(&new, "%x/new", FD(path));
    CATCH(e2, E_FIXEDSIZE){
        RETHROW(&e, &e2, E_FS);
    }else PROP_GO(&e, e, cleanup_2);

    ret = mkdir(new.data, 0770);
    if(ret && errno != EEXIST){
        TRACE(&e, "%x: %x\n", FS(new.data), FE(&errno));
        ORIG_GO(&e, E_OS, "unable to make maildir/new", cleanup_2);
    }

    // create maildir/tmp
    DSTR_VAR(tmp, 4096);
    e2 = FMT(&tmp, "%x/tmp", FD(path));
    CATCH(e2, E_FIXEDSIZE){
        RETHROW(&e, &e2, E_FS);
    }else PROP_GO(&e, e2, cleanup_3);

    ret = mkdir(tmp.data, 0770);
    if(ret && errno != EEXIST){
        TRACE(&e, "%x: %x\n", FS(tmp.data), FE(&errno));
        ORIG_GO(&e, E_OS, "unable to make maildir/tmp", cleanup_3);
    }

    return e;

cleanup_3:
    rmdir(new.data);
cleanup_2:
    rmdir(cur.data);
cleanup_1:
    rmdir(root.data);
    return e;
}

// this is called for filenames you created or after you have parsed them
// here, filename includes the subdirectory /{cur|new|tmp}/
/* throw: E_NOMEM
          */
static derr_t maildir_register(maildir_t* mdir, const dstr_t* subdir,
                               const dstr_t* filename, const dstr_t* uid,
                               size_t length){
    derr_t e = E_OK;

    // append the length to our list of lengths
    PROP(&e, LIST_APPEND(size_t, &mdir->lengths, length) );

    // append the subdir/filename string to our block of filenames
    size_t orig_n_len = mdir->names_block.len;
    char* orig_ptr = mdir->names_block.data;
    // first the subdir string
    PROP_GO(&e, dstr_append(&mdir->names_block, subdir), cleanup_1);
    // then the filename
    PROP_GO(&e, dstr_append(&mdir->names_block, filename), cleanup_2);
    // also make sure the filename can be used as a cstring later
    PROP_GO(&e, dstr_null_terminate(&mdir->names_block), cleanup_2);
    // in case of a reallocated block pointer, we need to redirect a bunch of name pointers
    char* new_ptr = mdir->names_block.data;
    if(new_ptr != orig_ptr){
        // for each name in the filenames list
        for(size_t i = 0; i < mdir->filenames.len; i++){
            mdir->filenames.data[i].data = new_ptr + (mdir->filenames.data[i].data - orig_ptr);
        }
    }

    // create a dstr_t string to point to names_block, and append it to filenames list
    dstr_t subdir_fname;
    subdir_fname.data = new_ptr + orig_n_len;
    subdir_fname.len = subdir->len + filename->len;
    subdir_fname.size = subdir_fname.len;
    subdir_fname.fixed_size = true;
    PROP_GO(&e, LIST_APPEND(dstr_t, &mdir->filenames, subdir_fname), cleanup_2);

    // append the UID string to the UID block
    size_t orig_u_len = mdir->uids_block.len;
    orig_ptr = mdir->uids_block.data;
    PROP_GO(&e, dstr_append(&mdir->uids_block, uid), cleanup_3);
    // in case of a reallocated block pointer, we need to redirect a bunch of uid pointers
    new_ptr = mdir->uids_block.data;
    if(new_ptr != orig_ptr){
        // for each uid in the uids list
        for(size_t i = 0; i < mdir->uids.len; i++){
            // get a pointer to the pointer we will modify (for readability mostly)
            mdir->uids.data[i].data = new_ptr + (mdir->uids.data[i].data - orig_ptr);
        }
    }

    // modify the dstr_t uid to point to the uids_block, and append it to uids list
    // make a new dstr_t that points to uids_block and
    dstr_t uid2 = dstr_sub(&mdir->uids_block, orig_u_len, 0);
    PROP_GO(&e, LIST_APPEND(dstr_t, &mdir->uids, uid2), cleanup_4);

    return e;

    // in case of error during appending, remove added items from other objects
// cleanup_5:
//     mdir->uids.len--;
cleanup_4:
    mdir->uids_block.len = orig_u_len;
cleanup_3:
    mdir->filenames.len--;
cleanup_2:
    mdir->names_block.len = orig_n_len;
cleanup_1:
    mdir->lengths.len--;
    return e;
}

struct parse_and_register_data_t {
    maildir_t* mdir;
    dstr_t* subdir;
};

// from the filename and subdir, parse the filename and register
// this function is written as a hook for for_each_file_in_dir()
/* throws: E_NOMEM
           */
static derr_t parse_and_register(const char* base, const dstr_t* fname,
                                 bool isdir, void* userdata){
    derr_t e = E_OK;
    derr_t e2;
    // always skip directories
    if(isdir) return e;
    // don't need base
    (void)base;

    // cast userdata
    struct parse_and_register_data_t* prdata = userdata;
    maildir_t* mdir = prdata->mdir;
    dstr_t* subdir = prdata->subdir;

    // do some parsing to make sure that fname is likely an email
    /* format is going to be:
           epochtime.length,uid.modded_hostname[:info]
       epochtime is [0-9] (len = 10)
       length is a number of bytes (len >=1)
       uid is a uid which doesn't contain ',' '.' ':' or '/' (len >= 1)
       modded_hostname set in maildir_mod_hostname() (len >= 1)
            (modded hostname will not contain '/' or ':' or '.')

    */

    // first just check to make sure we have met a minimum length
    if(fname->len < 16){
        // this filename is too short for an maildir name, just ignore it
        return e;
    }

    // major tokens is either side of the ':'
    LIST_VAR(dstr_t, major_tokens, 2);
    DSTR_STATIC(colon, ":");
    e2 = dstr_split(fname, &colon, &major_tokens);
    CATCH(e2, E_FIXEDSIZE){
        // this means there was more than 1 ':' -> invalid maildir name, skip it
        DROP_VAR(&e2);
        return e;
    }else PROP(&e, e2);


    LIST_VAR(dstr_t, minor_tokens, 3);
    DSTR_STATIC(dot, ".");
    e2 = dstr_split(&major_tokens.data[0], &dot, &minor_tokens);
    CATCH(e2, E_FIXEDSIZE){
        // this means there was more than 1 '.' -> invalid maildir name, skip it
        DROP_VAR(&e2);
        return e;
    }else PROP(&e, e2);


    if(minor_tokens.len < 3){
        // this means we're missing a field in the maildir name definition
        return e;
    }

    // that's enough requirements, now lets parse out the data we need
    LIST_VAR(dstr_t, fields, 4);
    DSTR_STATIC(comma, ",");
    e2 = dstr_split(&minor_tokens.data[1], &comma, &fields);
    CATCH(e2, E_FIXEDSIZE){
        // this means we put too many data fields in the unique string
        DROP_VAR(&e2);
        return e;
    }else PROP(&e, e2);


    // the first field is the length, the second field is the UID
    size_t length;
    e2 = dstr_toul(&fields.data[0], &length, 10);
    CATCH(e2, E_PARAM){
        // not a number string
        DROP_VAR(&e2);
        return e;
    }else PROP(&e, e2);

    // the second field is the uid
    dstr_t* uid = &fields.data[1];

    PROP(&e, maildir_register(mdir, subdir, fname, uid, length) );

    return e;
}

derr_t maildir_new(maildir_t* mdir, dstr_t* mdir_path){
    derr_t e = E_OK;
    derr_t e2;
    // wrap the path buffer
    DSTR_WRAP_ARRAY(mdir->path, mdir->path_buffer);
    // copy path over
    e2 = dstr_copy(mdir_path, &mdir->path);
    CATCH(e2, E_FIXEDSIZE){
        // filename is too long
        RETHROW(&e, &e2, E_FS);
    }else PROP(&e, e2);

    // null terminate path
    e2 = dstr_null_terminate(&mdir->path);
    CATCH(e2, E_FIXEDSIZE){
        // filename is too long
        RETHROW(&e, &e2, E_FS);
    }else PROP(&e, e2);

    // create maildir if it doesnt exist yet
    PROP(&e, maildir_create(mdir_path) );

    PROP(&e, LIST_NEW(dstr_t, &mdir->filenames, 32) );
    PROP_GO(&e, dstr_new(&mdir->names_block, 4096), fail_1 );
    PROP_GO(&e, LIST_NEW(size_t, &mdir->lengths, 32), fail_2 );
    PROP_GO(&e, LIST_NEW(dstr_t, &mdir->uids, 32), fail_3 );
    PROP_GO(&e, dstr_new(&mdir->uids_block, 4096), fail_4 );


    // get paths to cur, new folders (/tmp is things that might not be complete)
    DSTR_STATIC(cur, "/cur/");
    DSTR_STATIC(new, "/new/");

    dstr_t* subdirs[] = {&cur, &new};

    // search each subdir for emails
    for(size_t i = 0; i < sizeof(subdirs) / sizeof(*subdirs); i++){
        dstr_t* subdir = subdirs[i];
        struct parse_and_register_data_t prdata = {mdir, subdir};
        DSTR_VAR(path, 4096);
        e2 = FMT(&path, "%x%x", FD(mdir_path), FD(subdir));
        CATCH(e2, E_FIXEDSIZE){
            RETHROW(&e, &e2, E_FS);
        }else PROP_GO(&e, e2, fail_5);
        // register all emails in that file
        PROP_GO(&e, for_each_file_in_dir(path.data, parse_and_register,
                                      (void*)&prdata), fail_5);
    }

    return e;

fail_5:
    dstr_free(&mdir->uids_block);
fail_4:
    LIST_FREE(dstr_t, &mdir->uids);
fail_3:
    LIST_FREE(size_t, &mdir->lengths);
fail_2:
    dstr_free(&mdir->names_block);
fail_1:
    LIST_FREE(dstr_t, &mdir->filenames);
    return e;
}

void maildir_free(maildir_t* mdir){
    dstr_free(&mdir->uids_block);
    LIST_FREE(dstr_t, &mdir->uids);
    LIST_FREE(size_t, &mdir->lengths);
    dstr_free(&mdir->names_block);
    LIST_FREE(dstr_t, &mdir->filenames);
}

derr_t maildir_mod_hostname(const dstr_t* host, dstr_t* mod){
    /* modded_hostname replaces '/' with "\057"
                            and ':' with "\072"
                            and '.' with "\056" (nonstandard)
    */
    derr_t e = E_OK;

    LIST_PRESET(dstr_t, search, DSTR_LIT("/"),
                             DSTR_LIT(":"),
                             DSTR_LIT("."));
    LIST_PRESET(dstr_t, replace, DSTR_LIT("\\057"),
                              DSTR_LIT("\\072"),
                              DSTR_LIT("\\056"));
    PROP(&e, dstr_recode(host, mod, &search, &replace, false) );

    return e;
}

derr_t maildir_new_tmp_file(maildir_t* mdir, dstr_t* tempname, int* fd){
    derr_t e = E_OK;
    // find a filename that is not in use
    int i = 0;
    while(++i){
        // build the filename
        tempname->len = 0;
        derr_t e2 = FMT(tempname, "%x/tmp/%x", FD(&mdir->path), FI(i));
        CATCH(e2, E_FIXEDSIZE){
            RETHROW(&e, &e2, E_FS);
        }else PROP(&e, e2);

        // make sure file does not exist
        errno = 0;
        struct stat s;
        int ret = stat(tempname->data, &s);
        if(ret == -1){
            errno = 0;
            break;
        }
        // cut off after a reasonable amount of tries
        if(i > 1000){
            ORIG(&e, E_INTERNAL, "unable to find an unused file in maildir/tmp");
        }
    }
    *fd = open(tempname->data, O_CREAT | O_TRUNC | O_RDWR, 0660);

    if(*fd < 0){
        TRACE(&e, "%x: %x\n", FS("creating /tmp/ message"), FE(&errno));
        ORIG(&e, E_FS, "unable to create temp message in maildir");
    }

    return e;
}

derr_t maildir_new_rename(maildir_t* mdir, const char* tempname,
                          const dstr_t* uid, size_t length){
    derr_t e = E_OK;
    // get host name
    DSTR_VAR(hostname, 256);
    gethostname(hostname.data, hostname.size);
    hostname.len = strnlen(hostname.data, HOSTNAME_COMPONENT_MAX_LEN);

    // modify the hostname (replace unallowed characters)
    DSTR_VAR(modname, 4*HOSTNAME_COMPONENT_MAX_LEN);
    NOFAIL(&e, E_ANY, maildir_mod_hostname(&hostname, &modname) );

    // get the time
    time_t tloc;
    time_t tret = time(&tloc);
    if(tret == ((time_t) -1)){
        // if this fails... just use zero
        tloc = ((time_t) 0);
    }

    // get new filename
    DSTR_VAR(filename, 255);
    derr_t e2 = FMT(&filename, "%x.%x,%x.%x",
                           FI(tloc), FU(length), FD(uid), FD(&modname));
    CATCH(e2, E_FIXEDSIZE){
        RETHROW(&e, &e2, E_FS);
    }else PROP(&e, e2);

    // get the new file path
    DSTR_VAR(newpath, 4096);
    DSTR_STATIC(subdir, "/new/");
    e2 = FMT(&newpath, "%x%x%x", FD(&mdir->path), FD(&subdir), FD(&filename));
    CATCH(e2, E_FIXEDSIZE){
        RETHROW(&e, &e2, E_FS);
    }else PROP(&e, e2);

    // rename the file
    int ret = rename(tempname, newpath.data);
    if(ret != 0){
        TRACE(&e, "rename %x to %x: %x\n", FS(tempname), FS(newpath.data), FE(&errno));
        ORIG(&e, E_FS, "unable to rename temporary file");
    }

    // register the new email with the maildir
    PROP(&e, maildir_register(mdir, &subdir, &filename, uid, length) );

    return e;
}

derr_t maildir_get_index_from_uid(const maildir_t* mdir, const dstr_t* uid,
                                    size_t* index){
    derr_t e = E_OK;
    // search for matching uid
    for(size_t i = 0; i < mdir->uids.len; i++){
        int result = dstr_cmp(uid, &mdir->uids.data[i]);
        if(result == 0){
            *index = i;
            return e;
        }
    }
    // we are here if we found nothing
    ORIG(&e, E_INTERNAL, "did not find uid in the maildir");
}

derr_t maildir_open_message(const maildir_t* mdir, size_t index, int* fd){
    derr_t e = E_OK;
    // check inputs
    if(index >= mdir->uids.len)
        ORIG(&e, E_BADIDX, "index too high");

    // get filename
    dstr_t* filename = &mdir->filenames.data[index];

    // open file
    DSTR_VAR(path, 4096);
    PROP(&e, FMT(&path, "%x%x", FD(&mdir->path), FD(filename)) );

    *fd = open(path.data, O_RDONLY);

    if(*fd < 0){
        TRACE(&e, "%x: %x\n", FS(path.data), FE(&errno));
        ORIG(&e, E_OS, "unable to open message for reading in maildir");
    }

    return e;
}

derr_t maildir_delete_message(maildir_t* mdir, size_t index){
    derr_t e = E_OK;
    if(index >= mdir->uids.len)
        ORIG(&e, E_VALUE, "index too high");

    // get filename
    dstr_t* filename = &mdir->filenames.data[index];

    // delete the file
    DSTR_VAR(path, 4096);
    PROP(&e, FMT(&path, "%x%x", FD(&mdir->path), FD(filename)) );
    int ret = remove(path.data);
    if(ret != 0){
        TRACE(&e, "%x: %x\n", FD(&path), FE(&errno));
        ORIG(&e, E_OS, "unable to delete message from maildir");
    }

    // don't forget to eliminate things from the registry
    LIST_DELETE(dstr_t, &mdir->filenames, index);
    LIST_DELETE(dstr_t, &mdir->uids, index);
    LIST_DELETE(size_t, &mdir->lengths, index);

    return e;
}
