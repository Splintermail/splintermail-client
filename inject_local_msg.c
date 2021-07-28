// inject_local_msg: a testing tool to add a uid_local msgs to a maildir

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <libdstr/libdstr.h>
#include <libcrypto/libcrypto.h>
#include <libimaildir/libimaildir.h>

// stolen from citm/user.c
static derr_t _load_or_gen_mykey(
    const string_builder_t *key_path, keypair_t **out
){
    derr_t e = E_OK;

    PROP(&e, mkdirs_path(key_path, 0700) );

    string_builder_t mykey_path = sb_append(key_path, FS("mykey.pem"));

    bool have_key;
    PROP(&e, exists_path(&mykey_path, &have_key) );

    if(have_key){
        IF_PROP(&e, keypair_load_path(out, &mykey_path) ){
            // we must have hit an error reading the key
            TRACE(&e, "failed to load mykey...\n");
            DUMP(e);
            DROP_VAR(&e);
            LOG_ERROR("Failed to load mykey, generating a new one.\n");
            // delete the broken key
            PROP(&e, rm_rf_path(&mykey_path) );
        }else{
            // key was loaded successfully
            return e;
        }
    }

    PROP(&e, gen_key_path(4096, &mykey_path) );
    PROP(&e, keypair_load_path(out, &mykey_path) );

    return e;
}

static derr_t encrypt_msg(
    const dstr_t msg, keypair_t *key, const string_builder_t *path
){
    derr_t e = E_OK;

    dstr_t copy = {0};
    dstr_t cipher = {0};
    encrypter_t ec = {0};

    // encrypter expects a list of keys, not a single key
    link_t keys;
    link_init(&keys);
    link_list_append(&keys, &key->link);

    // copy, since encrypter will consume it
    PROP_GO(&e, dstr_copy(&msg, &copy), cu);

    PROP_GO(&e, dstr_new(&cipher, msg.len), cu);

    // do the encryption
    PROP_GO(&e, encrypter_new(&ec), cu);
    PROP_GO(&e, encrypter_start(&ec, &keys, &cipher), cu);
    PROP_GO(&e, encrypter_update(&ec, &copy, &cipher), cu);
    PROP_GO(&e, encrypter_finish(&ec, &cipher), cu);

    // write to the file
    PROP_GO(&e, dstr_write_path(path, &cipher), cu);

cu:
    encrypter_free(&ec);
    dstr_free(&cipher);
    dstr_free(&copy);
    link_remove(&key->link);
    return e;
}

static derr_t inject_local_msg(
    const dstr_t maildir_root,
    const dstr_t user,
    const dstr_t mailbox,
    const dstr_t msg
){
    derr_t e = E_OK;

    keypair_t *mykey = NULL;
    dirmgr_t dm = {0};
    dirmgr_hold_t *hold = NULL;
    imaildir_t *m = NULL;

    string_builder_t root = SB(FD(&maildir_root));
    // root/user
    string_builder_t user_path = sb_append(&root, FD(&user));
    // root/user/keys
    string_builder_t key_path = sb_append(&user_path, FS("keys"));
    // root/user/mail
    string_builder_t mail_path = sb_append(&user_path, FS("mail"));
    // root/PID
    string_builder_t msg_path = sb_append(&root, FI(compat_getpid()));

    PROP(&e, mkdirs_path(&key_path, 0700) );
    PROP(&e, mkdirs_path(&mail_path, 0700) );

    // load a keypair
    PROP_GO(&e, _load_or_gen_mykey(&key_path, &mykey), cu);

    // create a dirmgr
    PROP_GO(&e, dirmgr_init(&dm, mail_path, NULL), cu);

    // get a hold on a mailbox
    PROP_GO(&e, dirmgr_hold_new(&dm, &mailbox, &hold), cu);

    // open the held mailbox
    PROP_GO(&e, dirmgr_hold_get_imaildir(hold, &m), cu);

    // encrypt the message to a file
    PROP_GO(&e, encrypt_msg(msg, mykey, &msg_path), cu);

    // inject the message
    imap_time_t intdate = {0};
    msg_flags_t flags = {0};
    unsigned int uid_up = 0;
    unsigned int uid_dn;
    PROP_GO(&e,
        imaildir_add_local_file(m,
            &msg_path,
            uid_up,
            msg.len,
            intdate,
            flags,
            &uid_dn
        ),
    cu);

    // report which UID was created
    PROP_GO(&e, PFMT("%x\n", FU(uid_dn)), cu);

cu:
    DROP_CMD( remove_path(&msg_path) );
    dirmgr_hold_release_imaildir(hold, &m);
    dirmgr_hold_free(hold);
    dirmgr_free(&dm);
    keypair_free(&mykey);

    return e;
}

static void print_help(FILE *f){
    DROP_CMD(
        FFMT(f, NULL,
            "inject_local_msg: a tool for customizing mailboxes for tests\n"
            "\n"
            "usage: inject_local_msg OPTIONS < plaintext\n"
            "\n"
            "where OPTIONS are any of:\n"
            "  -h, --help\n"
            "  -r, --root=ARG          (required)\n"
            "  -u, --user=ARG          (required)\n"
            "  -m, --mailbox=ARG       (required)\n"
        )
    );
}

int main(int argc, char **argv){
    derr_t e = E_OK;

    DROP_CMD( logger_add_fileptr(LOG_LVL_INFO, stderr) );

    // options
    opt_spec_t o_help     = {'h',  "help",        false, OPT_RETURN_INIT};
    opt_spec_t o_root     = {'r',  "root",        true,  OPT_RETURN_INIT};
    opt_spec_t o_user     = {'u',  "user",        true,  OPT_RETURN_INIT};
    opt_spec_t o_mailbox  = {'m',  "mailbox",      true,  OPT_RETURN_INIT};

    opt_spec_t* spec[] = {
        &o_help,
        &o_root,
        &o_user,
        &o_mailbox,
    };
    size_t speclen = sizeof(spec) / sizeof(*spec);
    int newargc;
    {
        derr_t e = opt_parse(argc, argv, spec, speclen, &newargc);
        CATCH(e, E_ANY){
            DUMP(e);
            DROP_VAR(&e);
            print_help(stderr);
            return 1;
        }
    }

    if(o_help.found){
        print_help(stdout);
        return 0;
    }

    // check for required options
    bool ok = true;
    if(!o_root.found){
        fprintf(stderr, "--root is required\n");
        ok = false;
    }
    if(!o_user.found){
        fprintf(stderr, "--user is required\n");
        ok = false;
    }
    if(!o_mailbox.found){
        fprintf(stderr, "--mailbox is required\n");
        ok = false;
    }
    if(!ok){
        print_help(stderr);
        return 1;
    }

    // read a message from stdin
    dstr_t msg = {0};
    PROP_GO(&e, dstr_new(&msg, 4096), cu);
    PROP_GO(&e, dstr_read_all(0, &msg), cu);

    PROP_GO(&e,
        inject_local_msg(o_root.val, o_user.val, o_mailbox.val, msg),
    cu);

cu:
    dstr_free(&msg);

    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
        return 1;
    }
    return 0;
}
