#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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
static derr_t get_uuid_from_id(MYSQL *sql, const dstr_t id, dstr_t *uuid){
    derr_t e = E_OK;

    // check for '@' characters
    if(dstr_count2(id, DSTR_LIT("@")) > 0){
        // probably email
        bool ok;
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
        ORIG(&e, E_USERMSG, "usage: get_uuid EMAIL\n");
    }

    dstr_t email = get_arg(argv, 0);
    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    bool ok;

    PROP(&e, get_uuid_for_email(sql, email, &uuid, &ok) );
    if(ok){
        PFMT("%x\n", FSID(uuid));
    }else{
        FFMT(stderr, "no results\n");
    }

    return e;
}

static derr_t get_email_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_USERMSG, "usage: get_email FSID\n");
    }

    dstr_t fsid = get_arg(argv, 0);

    // convert fsid to uuid
    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, to_uuid(fsid, &uuid) );

    DSTR_VAR(email, SMSQL_EMAIL_SIZE);
    bool ok;

    PROP(&e, get_email_for_uuid(sql, uuid, &email, &ok) );
    if(ok){
        PFMT("%x\n", FD(email));
    }else{
        FFMT(stderr, "no results\n");
    }

    return e;
}

static derr_t hash_password_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;
    (void)sql;

    if(argc != 2){
        ORIG(&e, E_USERMSG, "usage: hash_password PASSWORD SALT\n");
    }

    dstr_t pass = get_arg(argv, 0);
    dstr_t salt = get_arg(argv, 1);

    DSTR_VAR(hash, SMSQL_PASSWORD_HASH_SIZE);
    PROP(&e, hash_password(pass, 5000, salt, &hash) );

    PFMT("%x\n", FD(hash));

    return e;
}

// aliases

static derr_t list_aliases_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_USERMSG, "usage: list_aliases (EMAIL|FSID)\n");
    }

    dstr_t id = get_arg(argv, 0);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, id, &uuid) );

    link_t aliases;
    link_init(&aliases);
    PROP(&e, list_aliases(sql, uuid, &aliases) );
    if(link_list_isempty(&aliases)){
        FFMT(stderr, "NO ALIASES\n");
        return e;
    }

    link_t *link;
    while((link = link_list_pop_first(&aliases))){
        smsql_alias_t *alias = CONTAINER_OF(link, smsql_alias_t, link);
        PFMT(
            "%x (%x)\n", FD(alias->alias), FS(alias->paid ? "paid" : "free")
        );
        smsql_alias_free(&alias);
    }

    return e;
}

static derr_t add_random_alias_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_USERMSG, "usage: add_random_alias (EMAIL|FSID)\n");
    }

    dstr_t id = get_arg(argv, 0);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, id, &uuid) );

    DSTR_VAR(alias, SMSQL_EMAIL_SIZE);

    PROP(&e, add_random_alias(sql, uuid, &alias) );
    PFMT("%x\n", FD(alias));

    return e;
}

static derr_t add_primary_alias_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 2){
        ORIG(&e, E_USERMSG, "usage: add_primary_alias (EMAIL|FSID) ALIAS\n");
    }

    dstr_t id = get_arg(argv, 0);
    dstr_t alias = get_arg(argv, 1);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, id, &uuid) );
    PROP(&e, add_primary_alias(sql, uuid, alias) );
    PFMT("OK\n");

    return e;
}

static derr_t delete_alias_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 2){
        ORIG(&e, E_USERMSG, "usage: delete_alias (EMAIL|FSID) ALIAS\n");
    }

    dstr_t id = get_arg(argv, 0);
    dstr_t alias = get_arg(argv, 1);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, id, &uuid) );
    PROP(&e, delete_alias(sql, uuid, alias) );

    PFMT("DELETED\n");

    return e;
}

static derr_t delete_all_aliases_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_USERMSG, "usage: delete_all_aliases (EMAIL|FSID)\n");
    }

    dstr_t id = get_arg(argv, 0);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, id, &uuid) );

    PROP(&e, delete_all_aliases(sql, uuid) );

    PFMT("DONE\n");

    return e;
}

// devices

static derr_t list_device_fprs_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_USERMSG, "usage: list_device_fprs (EMAIL|FSID)\n");
    }

    dstr_t id = get_arg(argv, 0);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, id, &uuid) );

    link_t dstrs;
    link_init(&dstrs);
    PROP(&e, list_device_fprs(sql, uuid, &dstrs) );

    link_t *link;
    while((link = link_list_pop_first(&dstrs))){
        smsql_dstr_t *dstr = CONTAINER_OF(link, smsql_dstr_t, link);
        PFMT("%x\n", FD(dstr->dstr));
        smsql_dstr_free(&dstr);
    }

    return e;
}

static derr_t list_device_keys_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_USERMSG, "usage: list_device_keys (EMAIL|FSID)\n");
    }

    dstr_t id = get_arg(argv, 0);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, id, &uuid) );

    link_t dstrs;
    link_init(&dstrs);
    PROP(&e, list_device_keys(sql, uuid, &dstrs) );

    link_t *link;
    while((link = link_list_pop_first(&dstrs))){
        smsql_dstr_t *dstr = CONTAINER_OF(link, smsql_dstr_t, link);
        PFMT("%x\n", FD(dstr->dstr));
        smsql_dstr_free(&dstr);
    }

    return e;
}

static derr_t get_device_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 2){
        ORIG(&e, E_USERMSG, "usage: get_device (EMAIL|FSID) FPR\n");
    }

    dstr_t id = get_arg(argv, 0);
    dstr_t fpr = get_arg(argv, 1);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, id, &uuid) );

    DSTR_VAR(pubkey, SMSQL_PUBKEY_SIZE);
    bool ok;
    PROP(&e, get_device(sql, uuid, fpr, &pubkey, &ok) );

    if(ok){
        PFMT("%x\n", FD(pubkey));
    }else{
        FFMT(stderr, "no results\n");
    }

    return e;
}

static derr_t add_device_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_USERMSG, "usage: add_device (EMAIL|FSID) < pubkey.pem\n");
    }

    dstr_t id = get_arg(argv, 0);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, id, &uuid) );

    DSTR_VAR(pubkey, SMSQL_PUBKEY_SIZE);
    PROP(&e, dstr_read_all(0, &pubkey) );

    DSTR_VAR(fpr, SMSQL_FPR_SIZE);
    PROP(&e, add_device(sql, uuid, pubkey, &fpr) );

    PFMT("%x\n", FD(fpr));

    return e;
}

static derr_t delete_device_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 2){
        ORIG(&e, E_USERMSG, "usage: delete_device (EMAIL|FSID) FINGERPRINT\n");
    }

    dstr_t id = get_arg(argv, 0);
    dstr_t fpr_hex = get_arg(argv, 1);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, id, &uuid) );

    PROP(&e, delete_device(sql, uuid, fpr_hex) );

    PFMT("DONE\n");

    return e;
}

// tokens

static derr_t list_tokens_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_USERMSG, "usage: list_tokens (EMAIL|FSID)\n");
    }

    dstr_t id = get_arg(argv, 0);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, id, &uuid) );

    link_t tokens;
    link_init(&tokens);
    PROP(&e, list_tokens(sql, uuid, &tokens) );

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
        ORIG(&e, E_USERMSG, "usage: add_token (EMAIL|FSID)\n");
    }

    dstr_t id = get_arg(argv, 0);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, id, &uuid) );

    unsigned int token;
    DSTR_VAR(secret, SMSQL_APISECRET_SIZE);

    PROP(&e, add_token(sql, uuid, &token, &secret) );

    PFMT("token:%x, secret:%x\n", FU(token), FD(secret));

    return e;
}

static derr_t delete_token_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 2){
        ORIG(&e, E_USERMSG, "usage: delete_token (EMAIL|FSID) TOKEN\n");
    }

    dstr_t id = get_arg(argv, 0);
    dstr_t token_str = get_arg(argv, 1);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, id, &uuid) );

    unsigned int token;
    PROP(&e, dstr_tou(&token_str, &token, 10) );

    PROP(&e, delete_token(sql, uuid, token) );

    PFMT("DONE\n");

    return e;
}

// installations

static derr_t list_installations_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_USERMSG, "usage: list_installations (EMAIL|FSID)\n");
    }

    dstr_t id = get_arg(argv, 0);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, id, &uuid) );

    link_t subdomains;
    link_init(&subdomains);
    PROP(&e, list_installations(sql, uuid, &subdomains) );

    link_t *link;
    while((link = link_list_pop_first(&subdomains))){
        smsql_dstr_t *dstr = CONTAINER_OF(link, smsql_dstr_t, link);
        PFMT("%x\n", FD(dstr->dstr));
        smsql_dstr_free(&dstr);
    }

    return e;
}

static derr_t add_installation_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_USERMSG, "usage: add_installation (EMAIL|FSID)\n");
    }

    dstr_t id = get_arg(argv, 0);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, id, &uuid) );

    DSTR_VAR(inst_uuid, SMSQL_UUID_SIZE);
    unsigned int token;
    DSTR_VAR(secret, SMSQL_APISECRET_SIZE);
    DSTR_VAR(subdomain, SMSQL_SUBDOMAIN_SIZE);
    DSTR_VAR(email, SMSQL_EMAIL_SIZE);

    PROP(&e,
        add_installation(sql,
            uuid,
            &inst_uuid,
            &token,
            &secret,
            &subdomain,
            &email
        )
    );

    PFMT(
        "inst_uuid: %x, token:%x, secret:%x, subdomain: %x, email: %x\n",
        FX(inst_uuid), FU(token), FD(secret), FD(subdomain), FD(email)
    );

    return e;
}

static derr_t delete_installation_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_USERMSG, "usage: delete_installation SUBDOMAIN\n");
    }

    dstr_t subdomain = get_arg(argv, 0);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    bool ok;
    PROP(&e, subdomain_user(sql, subdomain, &uuid, &ok) );
    if(!ok) ORIG(&e, E_USERMSG, "no such subdomain\n");

    PROP(&e, delete_installation(sql, uuid, subdomain) );

    PFMT("DONE\n");

    return e;
}

static derr_t subdomain_user_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_USERMSG, "usage: subdomain_user SUBDOMAIN\n");
    }

    dstr_t subdomain = get_arg(argv, 0);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    bool ok;
    PROP(&e, subdomain_user(sql, subdomain, &uuid, &ok) );
    if(!ok) ORIG(&e, E_USERMSG, "no such subdomain\n");

    DSTR_VAR(email, SMSQL_EMAIL_SIZE);
    PROP(&e, get_email_for_uuid(sql, uuid, &email, &ok) );
    if(!ok){
        ORIG(&e, E_INTERNAL, "found uuid without email: %x\n", FSID(uuid));
    }

    PFMT("%x\n", FD(email));

    return e;
}

static derr_t set_challenge_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 2){
        ORIG(&e, E_USERMSG, "usage: set_challenge SUBDOMAIN CHALLENGE\n");
    }

    dstr_t subdomain = get_arg(argv, 0);
    dstr_t challenge = get_arg(argv, 1);

    DSTR_VAR(inst_uuid, SMSQL_UUID_SIZE);
    bool ok;
    PROP(&e, subdomain_installation(sql, subdomain, &inst_uuid, &ok) );
    if(!ok) ORIG(&e, E_USERMSG, "no such subdomain\n");

    PROP(&e, set_challenge(sql, inst_uuid, challenge) );

    PFMT("DONE\n");

    return e;
}

static derr_t delete_challenge_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_USERMSG, "usage: delete_challenge SUBDOMAIN\n");
    }

    dstr_t subdomain = get_arg(argv, 0);

    DSTR_VAR(inst_uuid, SMSQL_UUID_SIZE);
    bool ok;
    PROP(&e, subdomain_installation(sql, subdomain, &inst_uuid, &ok) );
    if(!ok) ORIG(&e, E_USERMSG, "no such subdomain\n");

    PROP(&e, delete_challenge(sql, inst_uuid) );

    PFMT("DONE\n");

    return e;
}

static derr_t list_challenges_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;
    (void)argv;

    if(argc != 0){
        ORIG(&e, E_USERMSG, "usage: list_challenges\n");
    }

    link_t challenges;
    link_init(&challenges);
    PROP(&e, list_challenges(sql, &challenges) );

    link_t *link;
    while((link = link_list_pop_first(&challenges))){
        smsql_dpair_t *dpair = CONTAINER_OF(link, smsql_dpair_t, link);
        PFMT("%x:%x\n", FD(dpair->a), FD(dpair->b));
        smsql_dpair_free(&dpair);
    }

    return e;
}

// misc

static derr_t create_account_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 2){
        ORIG(&e, E_USERMSG, "usage: create_account EMAIL PASSWORD\n");
    }

    dstr_t email = get_arg(argv, 0);
    dstr_t pass = get_arg(argv, 1);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);

    PROP(&e, create_account(sql, email, pass, &uuid) );

    PFMT("uuid: %x\n", FSID(uuid));

    return e;
}

static derr_t delete_account_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_USERMSG, "usage: delete_account (EMAIL|FSID)\n");
    }

    dstr_t id = get_arg(argv, 0);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, id, &uuid) );

    PROP(&e, delete_account(sql, uuid) );

    IF_PROP(&e, trigger_deleter(sql, uuid) ){
        DUMP(e);
        DROP_VAR(&e);
    }

    PFMT("DONE\n");

    return e;
}

static derr_t account_info_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_USERMSG, "usage: account_info (EMAIL|FSID)\n");
    }

    dstr_t id = get_arg(argv, 0);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, id, &uuid) );

    size_t dvcs;
    size_t paids;
    size_t frees;
    PROP(&e, account_info(sql, uuid, &dvcs, &paids, &frees) );

    PFMT(
        "num_devices: %x, num_primary_aliases: %x, num_random_aliases: %x\n",
        FU(dvcs), FU(paids), FU(frees)
    );

    PFMT("DONE\n");

    return e;
}

static derr_t validate_password_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 2){
        ORIG(&e,
            E_USERMSG, "usage: validate_password (EMAIL|FSID) PASSWORD\n"
        );
    }

    dstr_t id = get_arg(argv, 0);
    dstr_t pass = get_arg(argv, 1);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, id, &uuid) );

    bool ok;
    PROP(&e, validate_user_password(sql, uuid, pass, &ok) );

    PFMT("%x\n", FS(ok ? "CORRECT" : "INCORRECT") );

    return e;
}

static derr_t change_password_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 2){
        ORIG(&e, E_USERMSG, "usage: change_password (EMAIL|FSID) PASSWORD\n");
    }

    dstr_t id = get_arg(argv, 0);
    dstr_t pass = get_arg(argv, 1);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, id, &uuid) );

    PROP(&e, change_password(sql, uuid, pass) );

    PFMT("DONE\n");

    return e;
}


static derr_t user_owns_address_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 2){
        ORIG(&e, E_USERMSG, "usage: user_owns_address (EMAIL|FSID) ADDRESS\n");
    }

    dstr_t id = get_arg(argv, 0);
    dstr_t address = get_arg(argv, 1);

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, get_uuid_from_id(sql, id, &uuid) );

    bool ok;
    PROP(&e, user_owns_address(sql, uuid, address, &ok) );

    PFMT("%x\n", FB(ok));

    return e;
}


static derr_t gtid_current_pos_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;
    (void)argv;

    if(argc != 0){
        ORIG(&e, E_USERMSG, "usage: gtid_current_pos\n");
    }

    DSTR_VAR(buf, 1024);
    PROP(&e, gtid_current_pos(sql, &buf) );

    PFMT("%x\n", FD(buf));

    return e;
}

static derr_t list_deletions_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;

    if(argc != 1){
        ORIG(&e, E_USERMSG, "usage: list_deletions SERVER_ID\n");
    }

    dstr_t server_id_str = get_arg(argv, 0);

    int server_id;
    PROP(&e, dstr_toi(&server_id_str, &server_id, 10) );

    link_t uuids;
    link_init(&uuids);
    PROP(&e, list_deletions(sql, server_id, &uuids) );
    if(link_list_isempty(&uuids)){
        FFMT(stderr, "nothing to delete\n");
        return e;
    }

    link_t *link;
    while((link = link_list_pop_first(&uuids))){
        smsql_dstr_t *uuid = CONTAINER_OF(link, smsql_dstr_t, link);
        PFMT("%x\n", FSID(uuid->dstr));
        smsql_dstr_free(&uuid);
    }

    return e;
}

// sysadmin utils

static derr_t list_users_action(MYSQL *sql, int argc, char **argv){
    derr_t e = E_OK;
    (void)argv;

    if(argc != 0){
        ORIG(&e, E_USERMSG, "usage: list_users\n");
    }

    link_t users;
    link_init(&users);
    PROP(&e, list_users(sql, &users) );
    if(link_list_isempty(&users)){
        FFMT(stderr, "(none)\n");
        return e;
    }

    link_t *link;
    while((link = link_list_pop_first(&users))){
        smsql_dstr_t *user = CONTAINER_OF(link, smsql_dstr_t, link);
        PFMT("%x\n", FD(user->dstr));
        smsql_dstr_free(&user);
    }

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

    PROP(&e, dmysql_library_init() );

    MYSQL sql;
    PROP_GO(&e, dmysql_init(&sql), cu_sql_lib);

    PROP_GO(&e, sql_connect_unix(&sql, user, pass, sock), cu_sql);

    PROP_GO(&e, action(&sql, argc, argv), cu_sql);

cu_sql:
    mysql_close(&sql);

cu_sql_lib:
    mysql_library_end();

    return e;
}

typedef struct {
    dstr_t name;
    action_f action;
} action_link_t;

// list all actions
static action_link_t action_links[128];
static size_t nactions = 0;

static void print_help(FILE *f){
    fprintf(f,
        "smsql: cli interface to predefined splintermail queries\n"
        "\n"
        "usage: smsql [OPTIONS] CMD\n"
        "\n"
        "where CMD is one of:\n"
    );
    for(size_t i = 0; i < nactions; i++){
        DROP_CMD( FFMT(f, "     %x\n", FD(action_links[i].name)) );
    }
    fprintf(f,
        "\n"
        "and where OPTIONS are one of:\n"
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

    size_t max_actions = sizeof(action_links) / sizeof(*action_links);

#define LINK_ACTION(str, act) do { \
    if(nactions == max_actions){ \
        fprintf(stderr, "too many actions!\n"); \
        exit(1); \
    } \
    action_links[nactions++] = (action_link_t){ \
        .name = DSTR_LIT(str), .action = act \
    }; \
} while (0)
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
    LINK_ACTION("get_device", get_device_action);
    LINK_ACTION("add_device", add_device_action);
    LINK_ACTION("delete_device", delete_device_action);
    LINK_ACTION("list_tokens", list_tokens_action);
    LINK_ACTION("add_token", add_token_action);
    LINK_ACTION("delete_token", delete_token_action);
    LINK_ACTION("subdomain_user", subdomain_user_action);
    LINK_ACTION("list_installations", list_installations_action);
    LINK_ACTION("add_installation", add_installation_action);
    LINK_ACTION("delete_installation", delete_installation_action);
    LINK_ACTION("set_challenge", set_challenge_action);
    LINK_ACTION("delete_challenge", delete_challenge_action);
    LINK_ACTION("list_challenges", list_challenges_action);
    LINK_ACTION("create_account", create_account_action);
    LINK_ACTION("delete_account", delete_account_action);
    LINK_ACTION("account_info", account_info_action);
    LINK_ACTION("validate_password", validate_password_action);
    LINK_ACTION("change_password", change_password_action);
    LINK_ACTION("user_owns_address", user_owns_address_action);
    LINK_ACTION("gtid_current_pos", gtid_current_pos_action);
    LINK_ACTION("list_deletions", list_deletions_action);
    LINK_ACTION("list_users", list_users_action);
#undef LINK_ACTION

    // specify command line options
    opt_spec_t o_help = {'h', "help", false};
    opt_spec_t o_debug = {'d', "debug", false};
    opt_spec_t o_sock = {'s', "socket", true};
    opt_spec_t o_user = {'\0', "user", true};
    opt_spec_t o_pass = {'\0', "pass", true};
    opt_spec_t o_host = {'\0', "host", true};
    opt_spec_t o_port = {'\0', "port", true};
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
    IF_PROP(&e, opt_parse(argc, argv, spec, speclen, &newargc) ){
        print_help(stdout);
        DROP_CMD( logger_add_fileptr(LOG_LVL_ERROR, stderr) );
        goto fail;
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
    PROP_GO(&e,
        logger_add_fileptr(
            o_debug.found ? LOG_LVL_DEBUG : LOG_LVL_INFO, stderr
        ),
    fail);

    // COMMAND
    dstr_t cmd;
    DSTR_WRAP(cmd, argv[1], strlen(argv[1]), true);

    action_f action = NULL;

    for(size_t i = 0; i < nactions; i++){
        if(dstr_cmp(&cmd, &action_links[i].name) == 0){
            action = action_links[i].action;
        }
    }

    if(action == NULL){
        LOG_ERROR("command \"%x\" unknown\n", FD(cmd));
        print_help(stderr);
        return 1;
    }

    derr_t e2 = smsql(
        o_sock.found ? &o_sock.val : NULL,
        o_user.found ? &o_user.val : NULL,
        o_pass.found ? &o_pass.val : NULL,
        action, newargc - 2, &argv[2]
    );
    CATCH(&e2, E_USERMSG){
        DSTR_VAR(usermsg, 256);
        consume_e_usermsg(&e2, &usermsg);
        FFMT(stderr, "%x\n", FD(usermsg));
        return 1;
    }else PROP_VAR_GO(&e, &e2, fail);

    return 0;

fail:
    DUMP(e);
    DROP_VAR(&e);
    return 1;
}
