#include <stdio.h>
#include <string.h>

#include "libsmsql.h"

// smsql: cli interface to predefined splintermail queries

typedef struct {
    const dstr_t *sock;
    const dstr_t *user;
    const dstr_t *pass;
} config_t;

typedef derr_t (*action_f)(MYSQL *sql, int argc, char **argv);

static dstr_t get_arg(char **argv, size_t idx){
    dstr_t out;
    DSTR_WRAP(out, argv[idx], strlen(argv[idx]), true);
    return out;
}

static derr_t get_uuid(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_VALUE, "usage: get_uuid EMAIL\n");
    }

    dstr_t email = get_arg(argv, 0);
    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    bool ok;

    PROP(&e, get_uuid_for_email(sql, &email, &uuid, &ok) );
    if(ok){
        PFMT("%x\n", FSID(&uuid));
    }else{
        FFMT(stderr, NULL, "no results\n");
    }

    return e;
}

static derr_t get_email(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_VALUE, "usage: get_email FSID\n");
    }

    dstr_t fsid = get_arg(argv, 0);

    // convert fsid to uuid
    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, to_uuid(&fsid, &uuid) );

    DSTR_VAR(email, SMSQL_EMAIL_SIZE);
    bool ok;

    PROP(&e, get_email_for_uuid(sql, &uuid, &email, &ok) );
    if(ok){
        PFMT("%x\n", FD(&email));
    }else{
        FFMT(stderr, NULL, "no results\n");
    }

    return e;
}

static derr_t smsql(
    const dstr_t *sock,
    const dstr_t *user,
    const dstr_t *pass,
    action_f action,
    int argc,
    char **argv
){
    derr_t e = E_OK;

    // get sql server ready
    int ret = mysql_library_init(0, NULL, NULL);
    if(ret != 0){
        ORIG(&e, E_SQL, "unable to init mysql library");
    }

    MYSQL sql;
    MYSQL* mret = mysql_init(&sql);
    if(!mret){
        ORIG_GO(&e, E_SQL, "unable to init mysql object", cu_sql_lib);
    }

    PROP_GO(&e, sql_connect_unix(&sql, user, pass, sock), cu_sql);

    PROP_GO(&e, action(&sql, argc, argv), cu_sql);

cu_sql:
    mysql_close(&sql);

cu_sql_lib:
    mysql_library_end();

    return e;
}

static void print_help(FILE *f){
    fprintf(f,
        "smsql: cli interface to predefined splintermail queries\n"
        "\n"
        "usage: smsql [OPTIONS] CMD\n"
        "\n"
        "where OPTIONS are one of:\n"
        "  -h --help\n"
        "  -d --debug\n"
        "  -s --socket PATH     default: /var/run/mysqld/mysqld.sock\n"
        "     --user\n"
        "     --pass\n"
        "     --host            (not yet supported)\n"
        "     --port            (not yet supported)\n"
    );
}

int main(int argc, char **argv){
    derr_t e = E_OK;

    // specify command line options
    opt_spec_t o_help = {'h', "help", false, OPT_RETURN_INIT};
    opt_spec_t o_debug = {'d', "debug", false, OPT_RETURN_INIT};
    opt_spec_t o_sock = {'s', "socket", true, OPT_RETURN_INIT};
    opt_spec_t o_user = {'\0', "user", true, OPT_RETURN_INIT};
    opt_spec_t o_pass = {'\0', "pass", true, OPT_RETURN_INIT};
    opt_spec_t o_host = {'\0', "host", true, OPT_RETURN_INIT};
    opt_spec_t o_port = {'\0', "port", true, OPT_RETURN_INIT};
    opt_spec_t* spec[] = {
        &o_help,
        &o_debug,
        &o_sock,
        &o_user,
        &o_pass,
        &o_host,
        &o_port,
    };

    size_t speclen = sizeof(spec) / sizeof(*spec);
    int newargc;
    // parse command line options
    e = opt_parse(argc, argv, spec, speclen, &newargc);
    if(is_error(e)){
        logger_add_fileptr(LOG_LVL_ERROR, stderr);
        DUMP(e);
        DROP_VAR(&e);
        return 2;
    }

    // print help?
    if(o_help.found){
        print_help(stdout);
        return 0;
    }

    if(newargc < 2){
        print_help(stderr);
        return 1;
    }

    // --debug
    logger_add_fileptr(o_debug.found ? LOG_LVL_DEBUG : LOG_LVL_INFO, stderr);

    // COMMAND
    dstr_t cmd;
    DSTR_WRAP(cmd, argv[1], strlen(argv[1]), true);

    action_f action = NULL;

#define LINK_ACTION(str, act) \
    if(dstr_cmp(&cmd, &DSTR_LIT(str)) == 0){ \
        action = act; \
    }

    LINK_ACTION("get_uuid", get_uuid);
    LINK_ACTION("get_email", get_email);

    if(action == NULL){
        LOG_ERROR("command \"%x\" unknown\n", FD(&cmd));
        print_help(stderr);
        return 1;
    }
#undef LINK_ACTION

    PROP_GO(&e,
        smsql(
            o_sock.found ? &o_sock.val : NULL,
            o_user.found ? &o_user.val : NULL,
            o_pass.found ? &o_pass.val : NULL,
            action, newargc - 2, &argv[2]
        ),
    fail);

    return 0;

fail:
    DUMP(e);
    DROP_VAR(&e);
    return 1;
}
