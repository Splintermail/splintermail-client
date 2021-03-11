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

// handle (EMAIL|FSID)-like args
static derr_t get_uuid_from_id(MYSQL *sql, const dstr_t *id, dstr_t *uuid){
    derr_t e = E_OK;

    // check for '@' characters
    if(dstr_count(id, &DSTR_LIT("@")) > 0){
        // probably email
        bool ok = false;
        PROP(&e, get_uuid_for_email(sql, id, uuid, &ok) );
        if(!ok) ORIG(&e, E_PARAM, "no user for provided email");
    }else{
        // probably fsid
        PROP(&e, to_uuid(id, uuid) );
    }

    return e;
}

//

static derr_t get_uuid_action(MYSQL *sql, int argc, char **argv){
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

static derr_t get_email_action(MYSQL *sql, int argc, char **argv){
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

static derr_t hash_password_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;
    (void)sql;

    if(argc != 2){
        ORIG(&e, E_VALUE, "usage: hash_password PASSWORD HEX_SALT\n");
    }

    dstr_t pass = get_arg(argv, 0);
    dstr_t hex_salt = get_arg(argv, 1);

    DSTR_VAR(salt, 8);
    PROP(&e, hex2bin(&hex_salt, &salt) );

    DSTR_VAR(hash, SMSQL_PASSWORD_HASH_SIZE);
    PROP(&e, hash_password(&pass, 5000, &salt, &hash) );

    PFMT("%x\n", FD(&hash));

    return e;
}

// aliases

static derr_t list_aliases_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_VALUE, "usage: list_aliases (EMAIL|FSID)\n");
    }

    dstr_t id = get_arg(argv, 0);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, &id, &uuid) );

    link_t aliases;
    link_init(&aliases);
    PROP(&e, list_aliases(sql, &uuid, &aliases) );
    if(link_list_isempty(&aliases)){
        PFMT("NO ALIASES\n");
        return e;
    }

    link_t *link;
    while((link = link_list_pop_first(&aliases))){
        smsql_alias_t *alias = CONTAINER_OF(link, smsql_alias_t, link);
        PFMT(
            "%x (%x)\n", FD(&alias->alias), FS(alias->paid ? "paid" : "free")
        );
        smsql_alias_free(&alias);
    }

    return e;
}

static derr_t add_random_alias_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_VALUE, "usage: add_random_alias (EMAIL|FSID)\n");
    }

    dstr_t id = get_arg(argv, 0);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, &id, &uuid) );

    DSTR_VAR(alias, SMSQL_EMAIL_SIZE);

    bool ok;
    PROP(&e, add_random_alias(sql, &uuid, &alias, &ok) );
    if(ok){
        PFMT("%x\n", FD(&alias));
    }else{
        FFMT(stderr, NULL, "FAILURE: WOULD EXCEED MAX RANDOM ALIASES\n");
    }

    return e;
}

static derr_t add_primary_alias_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 2){
        ORIG(&e, E_VALUE, "usage: add_primary_alias (EMAIL|FSID) ALIAS\n");
    }

    dstr_t id = get_arg(argv, 0);
    dstr_t alias = get_arg(argv, 1);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, &id, &uuid) );
    bool ok;
    PROP(&e, add_primary_alias(sql, &uuid, &alias, &ok) );
    if(ok){
        PFMT("OK\n");
    }else{
        FFMT(stderr, NULL, "FAILURE\n");
    }

    return e;
}

static derr_t delete_alias_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 2){
        ORIG(&e, E_VALUE, "usage: delete_alias (EMAIL|FSID) ALIAS\n");
    }

    dstr_t id = get_arg(argv, 0);
    dstr_t alias = get_arg(argv, 1);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, &id, &uuid) );
    bool deleted;
    PROP(&e, delete_alias(sql, &uuid, &alias, &deleted) );
    if(deleted){
        PFMT("DELETED\n");
    }else{
        PFMT("NOOP\n");
    }

    return e;
}

static derr_t delete_all_aliases_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_VALUE, "usage: delete_all_aliases (EMAIL|FSID)\n");
    }

    dstr_t id = get_arg(argv, 0);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, &id, &uuid) );

    PROP(&e, delete_all_aliases(sql, &uuid) );

    PFMT("DONE\n");

    return e;
}

// devices

static derr_t list_device_fprs_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_VALUE, "usage: list_device_fprs (EMAIL|FSID)\n");
    }

    dstr_t id = get_arg(argv, 0);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, &id, &uuid) );

    link_t dstrs;
    link_init(&dstrs);
    PROP(&e, list_device_fprs(sql, &uuid, &dstrs) );

    link_t *link;
    while((link = link_list_pop_first(&dstrs))){
        smsql_dstr_t *dstr = CONTAINER_OF(link, smsql_dstr_t, link);
        PFMT("%x\n", FD(&dstr->dstr));
        smsql_dstr_free(&dstr);
    }

    return e;
}

static derr_t list_device_keys_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_VALUE, "usage: list_device_keys (EMAIL|FSID)\n");
    }

    dstr_t id = get_arg(argv, 0);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, &id, &uuid) );

    link_t dstrs;
    link_init(&dstrs);
    PROP(&e, list_device_keys(sql, &uuid, &dstrs) );

    link_t *link;
    while((link = link_list_pop_first(&dstrs))){
        smsql_dstr_t *dstr = CONTAINER_OF(link, smsql_dstr_t, link);
        PFMT("%x\n", FD(&dstr->dstr));
        smsql_dstr_free(&dstr);
    }

    return e;
}

static derr_t add_device_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_VALUE, "usage: add_device (EMAIL|FSID) < pubkey.pem\n");
    }

    dstr_t id = get_arg(argv, 0);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, &id, &uuid) );

    DSTR_VAR(pubkey, SMSQL_PUBKEY_SIZE);
    PROP(&e, dstr_read_all(0, &pubkey) );

    bool ok;
    PROP(&e, add_device(sql, &uuid, &pubkey, &ok) );

    if(ok){
        PFMT("SUCCESS\n");
    }else{
        FFMT(stderr, NULL, "FAILURE: WOULD EXCEED MAX DEVICES\n");
    }

    return e;
}

static derr_t delete_device_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 2){
        ORIG(&e, E_VALUE, "usage: delete_device (EMAIL|FSID) FINGERPRINT\n");
    }

    dstr_t id = get_arg(argv, 0);
    dstr_t fpr_hex = get_arg(argv, 1);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, &id, &uuid) );

    PROP(&e, delete_device(sql, &uuid, &fpr_hex) );

    PFMT("DONE\n");

    return e;
}

// tokens

static derr_t list_tokens_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_VALUE, "usage: list_tokens (EMAIL|FSID)\n");
    }

    dstr_t id = get_arg(argv, 0);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, &id, &uuid) );

    link_t tokens;
    link_init(&tokens);
    PROP(&e, list_tokens(sql, &uuid, &tokens) );

    link_t *link;
    while((link = link_list_pop_first(&tokens))){
        smsql_uint_t *uint = CONTAINER_OF(link, smsql_uint_t, link);
        PFMT("%x\n", FU(uint->uint));
        smsql_uint_free(&uint);
    }

    return e;
}

static derr_t add_token_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_VALUE, "usage: add_token (EMAIL|FSID)\n");
    }

    dstr_t id = get_arg(argv, 0);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, &id, &uuid) );

    unsigned int token;
    DSTR_VAR(secret, SMSQL_APISECRET_SIZE);

    PROP(&e, add_token(sql, &uuid, &token, &secret) );

    PFMT("token:%x, secret:%x\n", FU(token), FD(&secret));

    return e;
}

static derr_t delete_token_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 2){
        ORIG(&e, E_VALUE, "usage: delete_token (EMAIL|FSID) TOKEN\n");
    }

    dstr_t id = get_arg(argv, 0);
    dstr_t token_str = get_arg(argv, 1);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, &id, &uuid) );

    unsigned int token;
    PROP(&e, dstr_tou(&token_str, &token, 10) );

    PROP(&e, delete_token(sql, &uuid, token) );

    PFMT("DONE\n");

    return e;
}

// misc

static derr_t account_info_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_VALUE, "usage: account_info (EMAIL|FSID)\n");
    }

    dstr_t id = get_arg(argv, 0);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, &id, &uuid) );

    size_t dvcs;
    size_t paids;
    size_t frees;
    PROP(&e, account_info(sql, &uuid, &dvcs, &paids, &frees) );

    PFMT(
        "num_devices: %x, num_primary_aliases: %x, num_random_aliases: %x\n",
        FU(dvcs), FU(paids), FU(frees)
    );

    PFMT("DONE\n");

    return e;
}

static derr_t validate_password_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;
    (void)sql;

    if(argc != 2){
        ORIG(&e, E_VALUE, "usage: validate_password (EMAIL|FSID) PASSWORD\n");
    }

    dstr_t id = get_arg(argv, 0);
    dstr_t pass = get_arg(argv, 1);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, &id, &uuid) );

    bool ok;
    PROP(&e, validate_user_password(sql, &uuid, &pass, &ok) );

    PFMT("%x\n", FS(ok ? "CORRECT" : "INCORRECT") );

    return e;
}

static derr_t change_password_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;
    (void)sql;

    if(argc != 2){
        ORIG(&e, E_VALUE, "usage: change_password (EMAIL|FSID) PASSWORD\n");
    }

    dstr_t id = get_arg(argv, 0);
    dstr_t pass = get_arg(argv, 1);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, &id, &uuid) );

    PROP(&e, change_password(sql, &uuid, &pass) );

    PFMT("DONE\n");

    return e;
}

//////

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

    LINK_ACTION("get_uuid", get_uuid_action);
    LINK_ACTION("get_email", get_email_action);
    LINK_ACTION("hash_password", hash_password_action);
    LINK_ACTION("list_aliases", list_aliases_action);
    LINK_ACTION("add_random_alias", add_random_alias_action);
    LINK_ACTION("add_primary_alias", add_primary_alias_action);
    LINK_ACTION("delete_alias", delete_alias_action);
    LINK_ACTION("delete_all_aliases", delete_all_aliases_action);
    LINK_ACTION("list_device_fprs", list_device_fprs_action);
    LINK_ACTION("list_device_keys", list_device_keys_action);
    LINK_ACTION("add_device", add_device_action);
    LINK_ACTION("delete_device", delete_device_action);
    LINK_ACTION("list_tokens", list_tokens_action);
    LINK_ACTION("add_token", add_token_action);
    LINK_ACTION("delete_token", delete_token_action);
    LINK_ACTION("account_info", account_info_action);
    LINK_ACTION("validate_password", validate_password_action);
    LINK_ACTION("change_password", change_password_action);

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
