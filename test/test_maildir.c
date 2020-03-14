#include <fcntl.h>
#include <limits.h>
#include <errno.h>

#include <libdstr/libdstr.h>
#include <maildir.h>

#include "test_utils.h"


#define NUM_FAKES 1000
#define IDX_DEL_TEST (NUM_FAKES / 2)

// fake names are of the form:
// /cur/NNNN.NNNN,UIDNNNN.hostname:2,ABC
// where NNNN is a normal base-10 index (it used to be zero-padded)
#define IDX_LEN 5
#define UID_LEN (IDX_LEN + 3)
// filenamelength = 41 (40 + \0)
#define FNAME_LEN (5 + IDX_LEN + 1 + IDX_LEN + 1 + UID_LEN + 15)

LIST_STATIC_VAR(dstr_t, g_uids, NUM_FAKES);
DSTR_STATIC_VAR(g_uids_block, NUM_FAKES * (UID_LEN + 1));
LIST_STATIC_VAR(dstr_t, g_fnames, NUM_FAKES);
DSTR_STATIC_VAR(g_fnames_block, NUM_FAKES * (FNAME_LEN + 1));

static derr_t gen_fake_emails(maildir_t* mdir){
    derr_t e = E_OK;
    for(size_t i = 0; i < NUM_FAKES; i++){
        // create padded index
        DSTR_VAR(index, IDX_LEN + 1);
        PROP(&e, FMT(&index, "%x", FU(i)) );
        // create uid
        DSTR_VAR(uid, UID_LEN + 1);
        PROP(&e, FMT(&uid, "UID%x", FU(i)) );
        // create filename
        DSTR_VAR(fname, FNAME_LEN + 1);
        PROP(&e, FMT(&fname, "/cur/%x.%x,%x.hostname;2,ABC",
                  FD(&index), FD(&index), FD(&uid)) );
        // full path to file
        DSTR_VAR(path, 4096);
        PROP(&e, FMT(&path, "%x%x", FD(&mdir->path), FD(&fname)) );
        // create the file
        int ret = compat_open(path.data, O_WRONLY|O_CREAT|O_TRUNC, 0770);
        if(ret < 0){
            TRACE(&e, "%x: %x\n", FS(path.data), FE(&errno));
            ORIG(&e, E_OS, "compat_open() failed");
        }
        // close the file, we don't actually need it
        compat_close(ret);
        // null terminator
        DSTR_STATIC(null, "\0");
        size_t orig_u_len = g_uids_block.len;
        size_t orig_f_len = g_fnames_block.len;
        // append uid and fname to block storage
        PROP(&e, dstr_append(&g_uids_block, &uid) );
        PROP(&e, dstr_append(&g_uids_block, &null) );
        PROP(&e, dstr_append(&g_fnames_block, &fname) );
        PROP(&e, dstr_append(&g_fnames_block, &null) );
        // set pointers of uid and fname
        uid.data = g_uids_block.data + orig_u_len;
        fname.data = g_fnames_block.data + orig_f_len;
        // append uid and fname to LIST(dstr_t)'s
        PROP(&e, LIST_APPEND(dstr_t, &g_uids, uid) );
        PROP(&e, LIST_APPEND(dstr_t, &g_fnames, fname) );
    }
    return e;
}

#define EXP_VS_GOT(exp, got, str_type) { \
    int result = dstr_cmp(exp, got); \
    if(result != 0){ \
        PROP(&e, PFMT("expected \"%x\"\n" \
                           "but got: \"%x\"\n", FD(exp), FD(got)) ); \
        ORIG(&e, E_VALUE, "mismatching " str_type); \
    } \
}

static derr_t test_maildir(void){
    derr_t e = E_OK;
    // first create the maildir
    maildir_t mdir;
    DSTR_STATIC(mdir_path, "asdf_temp_maildir");
    PROP(&e, maildir_new(&mdir, &mdir_path) );
    // then create fake emails
    PROP(&e, gen_fake_emails(&mdir) );
    // now close maildir
    maildir_free(&mdir);

    // now open it again, and this time it should parse the fake emails
    PROP(&e, maildir_new(&mdir, &mdir_path) );

    // verify the right things were registered
    if(mdir.filenames.len != NUM_FAKES){
        ORIG(&e, E_VALUE, "wrong number of registered files");
    }
    for(size_t i = 0; i < mdir.filenames.len; i++){
        size_t length = mdir.lengths.data[i];
        EXP_VS_GOT(&g_fnames.data[length], &mdir.filenames.data[i], "filename");
        EXP_VS_GOT(&g_uids.data[length], &mdir.uids.data[i], "uid");
    }

    // delete a file in the middle of the registry
    PROP(&e, maildir_delete_message(&mdir, IDX_DEL_TEST) );

    // re-verify the right things remain
    if(mdir.filenames.len != NUM_FAKES - 1){
        ORIG(&e, E_VALUE, "wrong number of registered files after delete_msg");
    }
    for(size_t i = 0; i < mdir.filenames.len; i++){
        size_t length = mdir.lengths.data[i];
        EXP_VS_GOT(&g_fnames.data[length], &mdir.filenames.data[i], "filename");
        EXP_VS_GOT(&g_uids.data[length], &mdir.uids.data[i], "uid");
    }

    // now create a new message through the maildir interface
    DSTR_STATIC(test_uid, "abcdefghijklmnop");
    DSTR_STATIC(test_body, "test body");
    size_t test_len = 123456789;
    // open a new email
    DSTR_VAR(tempname, 4096);
    int fd;
    PROP(&e, maildir_new_tmp_file(&mdir, &tempname, &fd) );
    // write some data into that message
    PROP(&e, dstr_write(fd, &test_body) );
    // done writing
    compat_close(fd);
    // save the message
    PROP(&e, maildir_new_rename(&mdir, tempname.data, &test_uid, test_len) );
    // get the index of that message
    size_t index;
    PROP(&e, maildir_get_index_from_uid(&mdir, &test_uid, &index) );
    // now open the same message
    PROP(&e, maildir_open_message(&mdir, index, &fd) );
    // read the body of the message
    DSTR_VAR(body, 4096);
    size_t amnt_read;
    PROP(&e, dstr_read(fd, &body, 0, &amnt_read) );
    compat_close(fd);
    EXP_VS_GOT(&test_body, &body, "email body");

    // delete all emails
    PROP(&e, rm_rf(mdir.path.data) );

    maildir_free(&mdir);

    return e;
}

static derr_t test_mod_hostname(void){
    derr_t e = E_OK;
    // build input
    DSTR_VAR(in, 256);
    /* since char might be signed or unsigned on a platform, and casting to
       a signed data type can result in undefined behavior, we will use
       the explicit CHAR_MIN and CHAR_MAX values here */
    for(int i = CHAR_MIN; i < CHAR_MAX + 1; i++){
        in.data[in.len++] = (char)i;
    }
    // build expected output
    DSTR_VAR(exp, 1024);
    exp.len = 0;
    for(int i = CHAR_MIN; i < CHAR_MAX + 1; i++){
        switch((char)i){
            case '/': PROP(&e, dstr_append(&exp, &DSTR_LIT("\\057")) ); break;
            case ':': PROP(&e, dstr_append(&exp, &DSTR_LIT("\\072")) ); break;
            case '.': PROP(&e, dstr_append(&exp, &DSTR_LIT("\\056")) ); break;
            default: exp.data[exp.len++] = (char)i; break;
        }
    }
    // run test
    DSTR_VAR(out, 1024);
    PROP(&e, maildir_mod_hostname(&in, &out) );
    if(dstr_cmp(&out, &exp) != 0){
        size_t start = 0;
        while(start + 20 < out.len){
            dstr_t sub1 = dstr_sub(&exp, start, start + 20);
            dstr_t sub2 = dstr_sub(&out, start, start + 20);
            TRACE(&e, " expected: %x\n but got:  %x\n",
                      FD_DBG(&sub1), FD_DBG(&sub2));
            start += 20;
        }
        ORIG(&e, E_VALUE, "maildir_mod_hostname failed");
    }
    return e;
}

int main(int argc, char** argv){
    derr_t e;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_maildir(), test_fail);
    PROP_GO(&e, test_mod_hostname(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
