// inject_local_msg: a testing tool to add a uid_local msgs to a maildir

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdlib.h>

#include <libdstr/libdstr.h>
#include <libcrypto/libcrypto.h>
#include <libimaildir/libimaildir.h>

static derr_t inject_local_msg(
    const dstr_t maildir_root,
    const dstr_t user,
    const dstr_t mailbox,
    const dstr_t msg
){
    derr_t e = E_OK;

    dirmgr_t dm = {0};
    dirmgr_hold_t *hold = NULL;
    imaildir_t *m = NULL;

    string_builder_t root = SBD(maildir_root);
    // root/user
    string_builder_t user_path = sb_append(&root, SBD(user));
    // root/user/mail
    string_builder_t mail_path = sb_append(&user_path, SBSN("mail", 4));
    // root/PID
    string_builder_t msg_path = sb_append(&root, SBI(compat_getpid()));

    PROP(&e, mkdirs_path(&mail_path, 0700) );

    // create a dirmgr
    PROP_GO(&e, dirmgr_init(&dm, mail_path, NULL), cu);

    // get a hold on a mailbox
    PROP_GO(&e, dirmgr_hold_new(&dm, &mailbox, &hold), cu);

    // open the held mailbox
    PROP_GO(&e, dirmgr_hold_get_imaildir(hold, &m), cu);

    // write the message to a file
    PROP_GO(&e, dstr_write_path(&msg_path, &msg), cu);

    // inject the message
    imap_time_t intdate = {0};
    msg_flags_t flags = {0};
    unsigned int uid_up = 0;
    void *up_noresync = NULL;
    unsigned int uid_dn;
    PROP_GO(&e,
        imaildir_add_local_file(m,
            &msg_path,
            uid_up,
            msg.len,
            intdate,
            flags,
            up_noresync,
            &uid_dn
        ),
    cu);

    // report which UID was created
    PROP_GO(&e, FFMT(stdout, "%x\n", FU(uid_dn)), cu);

cu:
    DROP_CMD( remove_path(&msg_path) );
    dirmgr_hold_release_imaildir(hold, &m);
    dirmgr_hold_free(hold);
    dirmgr_free(&dm);

    return e;
}

static void print_help(FILE *f){
    FFMT_QUIET(f,
        "inject_local_msg: a tool for customizing mailboxes for tests\n"
        "\n"
        "usage: inject_local_msg OPTIONS < plaintext\n"
        "\n"
        "where OPTIONS are any of:\n"
        "  -h, --help\n"
        "  -r, --root=ARG          (required)\n"
        "  -u, --user=ARG          (required)\n"
        "  -m, --mailbox=ARG       (required)\n"
    );
}

int main(int argc, char **argv){
    derr_t e = E_OK;

    DROP_CMD( logger_add_fileptr(LOG_LVL_INFO, stderr) );

    // options
    opt_spec_t o_help     = {'h',  "help",    false};
    opt_spec_t o_root     = {'r',  "root",    true};
    opt_spec_t o_user     = {'u',  "user",    true};
    opt_spec_t o_mailbox  = {'m',  "mailbox", true};

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
