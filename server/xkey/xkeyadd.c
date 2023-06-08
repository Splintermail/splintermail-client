/*
    Provide an XKEYADD command to the dovecot IMAP server.

    Syntax:

        tag XKEYADD pubkey_literal

    Responses:

        * OK [XKEYADD fingerprint] key added

        * NO max devices already reached
*/

#include "xkey.h"

static derr_t add_key(
    const char *sock,
    const dstr_t uuid,
    const dstr_t pubkey,
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
bool cmd_xkeyadd(struct client_command_context *cmd){
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
    IF_PROP(&e, to_uuid(fsid, &uuid) ){
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
    derr_t e2 = add_key(sock, uuid, pubkey, &fpr);
    CATCH(e2, E_USERMSG){
        // assume this is the "too many keys" issue and not the "invalid key"
        // (since it is only our client who uses this command)
        DSTR_VAR(buf, 128);
        DROP_CMD( FMT(&buf, "NO ") );
        consume_e_usermsg(&e2, &buf);
        client_send_tagline(cmd, buf.data);
    }else CATCH(e2, E_ANY){
        badbadbad_alert(DSTR_LIT("error in cmd_xkeyadd()"), e2.msg);
        DUMP(e2);
        DROP_VAR(&e2);
        client_send_command_error(cmd, "internal server failure");
    }else{
        // success case
        DSTR_VAR(buf, SMSQL_FPR_SIZE + 32);
        DROP_CMD( FMT(&buf, "OK [XKEYADD %x] key added", FD(fpr)) );
        client_send_tagline(cmd, buf.data);
    }

    return true;
}
