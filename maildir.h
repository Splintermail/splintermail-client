#ifndef MAILDIR_H
#define MAILDIR_H

#include "common.h"

typedef struct {
    char path_buffer[4096];
    dstr_t path;
    LIST(dstr_t) filenames;
    dstr_t names_block;
    LIST(size_t) lengths;
    LIST(dstr_t) uids;
    dstr_t uids_block;
} maildir_t;

// maildir_new will allocate the necessary memory for the maildir_t objects
// also create and parse the cur/, tmp/, and new/ folders to get all files and UIDs
derr_t maildir_new(maildir_t* mdir, dstr_t* mdir_path);
/* throws: E_FS (mdir_path too long, or issues in for_each_file_in_dir())
           E_OS (incorrect permissions, or from for_each_file_in_dir())
           E_INTERNAL
           E_NOMEM */

void maildir_free(maildir_t* mdir);


// emails will be treated as immutable, you can only create, read, or delete

// it will be a write-to-file, maybe-decrypt, and save-to-maildir flow

// this will always hand a null-terminated tempname back
// you have to close fd when you are done
derr_t maildir_new_tmp_file(maildir_t* mdir, dstr_t* tempname, int* fd);
/* throws: E_INTERNAL (from FMT, or unable to find new file)
           E_FS (path too long or permissions error) */


derr_t maildir_new_rename(maildir_t* mdir, const char* tempname,
                          const dstr_t* uid, size_t length);
/* throws: E_INTERNAL
           E_FS
           E_NOMEM */

// helper function for getting the index from the uid (its an O(N) operation)
derr_t maildir_get_index_from_uid(const maildir_t* mdir, const dstr_t* uid,
                                    size_t* index);
// maildir_open_message() is for reading existing messages
// then you just use close() directly on the fd
derr_t maildir_open_message(const maildir_t* mdir, size_t index, int* fd);

derr_t maildir_delete_message(maildir_t* mdir, size_t index);

// this is only exposed for testing
derr_t maildir_mod_hostname(const dstr_t* host, dstr_t* mod);
/* throws: E_FIXEDSIZE (from dstr_recode)
           E_NOMEM     (from dstr_recode) */

#endif // MAILDIR_H
