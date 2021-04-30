/*
    Provide an XKEYADD command to the dovecot IMAP server.

    Syntax:

        tag XKEYADD pubkey_literal

    Responses:

        * OK [XKEYADD fingerprint] key added

        * NO max devices already reached
*/

// compatibility with dovecot's build system:
#define UOFF_T_LONG  // gcc reports that off_t is a `long int` on linux 64
#define SSIZE_T_MAX SSIZE_MAX
#define HAVE_SOCKLEN_T
#define HAVE__BOOL
#define HAVE_STRUCT_IOVEC
#define FLEXIBLE_ARRAY_MEMBER  // c99 flexible array is allowed by gcc
#define STATIC_ARRAY static  // c99 static array keyword is honored by gcc

// Let dovecot do it's thing unfettered by our warnings.
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#endif // __GNUC__

    #include "imap-common.h"
    #include "imap-commands.h"
    #include "imap-arg.h"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif // __GNUC__


#include "libdstr/libdstr.h"
#include "server/mysql_util/mysql_util.h"
#include "server/libsmsql.h"

static derr_t add_key(
    const char *sock,
    const dstr_t *uuid,
    const dstr_t *pubkey,
    dstr_t *fpr
){
    derr_t e = E_OK;

    DSTR_VAR(d_sock, 256);
    PROP(&e, FMT(&d_sock, "%x", FS(sock)) );

    MYSQL sql;
    MYSQL* mret = mysql_init(&sql);
    if(!mret){
        ORIG(&e, E_SQL, "unable to init mysql object");
    }

    PROP_GO(&e, sql_connect_unix(&sql, NULL, NULL, &d_sock), cu_sql);

    PROP_GO(&e, add_device(&sql, uuid, pubkey, fpr), cu_sql);

cu_sql:
    mysql_close(&sql);

    return e;
}

// cmd_xkeyadd is a command_func_t
static bool cmd_xkeyadd(struct client_command_context *cmd){
    derr_t e = E_OK;

    struct client *client = cmd->client;
    const struct imap_arg *args;
    const char *arg;

    if(!client_read_args(cmd, 0, 0, &args))
        return false;

    /* client_read_args returns a list with a special sentinal so it's always
       safe to deref the first element */
    if(!imap_arg_get_astring(&args[0], &arg)){
        client_send_command_error(cmd, "invalid key");
        return true;
    }

    DSTR_VAR(pubkey, SMSQL_PUBKEY_SIZE);
    IF_PROP(&e, FMT(&pubkey, "%x", FS(arg)) ){
        DROP_VAR(&e);
        client_send_command_error(cmd, "invalid key; too long");
        return true;
    }

    if(!IMAP_ARG_IS_EOL(&args[1])){
        client_send_command_error(cmd, "extra args");
        return true;
    }

    // get the user's uuid from their FSID@x.splintermail.com login result
    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    DSTR_VAR(fsid, SMSQL_FSID_SIZE);
    if(client->user->username != NULL){
        fsid.len = MIN(fsid.size, strlen(client->user->username));
        memcpy(fsid.data, client->user->username, fsid.len);
    }
    IF_PROP(&e, to_uuid(&fsid, &uuid) ){
        DUMP(e);
        DROP_VAR(&e);
        client_send_command_error(
            cmd, "internal error: failed to decode user_uuid from login"
        );
        return true;
    }

    // look up the socket path
    const char *sock = mail_user_plugin_getenv(client->user, "sql_socket");
    if(sock == NULL || sock[0] == '\0'){
        sock = "/var/run/mysqld/mysqld.sock";
    }

    DSTR_VAR(fpr, SMSQL_FPR_SIZE);
    derr_t e2 = add_key(sock, &uuid, &pubkey, &fpr);
    CATCH(e2, E_USERMSG){
        // assume this is the "too many keys" issue and not the "invalid key"
        // (since it is only our client who uses this command)
        DSTR_VAR(buf, 128);
        DROP_CMD( FMT(&buf, "NO ") );
        consume_e_usermsg(&e2, &buf);
        client_send_tagline(cmd, buf.data);
    }else CATCH(e2, E_ANY){
        DUMP(e);
        DROP_VAR(&e);
        client_send_command_error(cmd, "internal server failure");
    }else{
        // success case
        DSTR_VAR(buf, SMSQL_FPR_SIZE + 32);
        DROP_CMD( FMT(&buf, "OK [XKEYADD %x] key added", FD(&fpr)) );
        client_send_tagline(cmd, buf.data);
    }

    return true;
}

static const char *cmd_name = "XKEYADD";

// happily, our command is basically totally independent of mailboxes
static const enum command_flags cmd_flags = 0
// | COMMAND_FLAG_USES_SEQS
// | COMMAND_FLAG_BREAKS_SEQS
// | COMMAND_FLAG_USES_MAILBOX
// | COMMAND_FLAG_REQUIRES_SYNC
// | COMMAND_FLAG_USE_NONEXISTENT
;

// externally linkable plugin hooks

void xkeyadd_plugin_init(struct module *module ATTR_UNUSED);
void xkeyadd_plugin_init(struct module *module ATTR_UNUSED){
    // hack: let xkeysync plugin configure mysql and logging
    command_register(cmd_name, cmd_xkeyadd, cmd_flags);
}

void xkeyadd_plugin_deinit(void);
void xkeyadd_plugin_deinit(void){
    // hack: let xkeysync cleanup cleanup mysql and logging
    command_unregister(cmd_name);
}
