/*
    Provide an XKEYSYNC command to the dovecot IMAP server.

    Syntax:

        tag XKEYSYNC [fingerprint ...]

            where the list of fingerprints is all fingerprints the client knows

        DONE

            to end the synchronization (only valid after '+ OK' response)

    Responses:
        + OK
            the initial sync is complete (there may have been no update ) and
            the command can now be terminated by DONE

        * XKEYSYNC DELETED fingerprint

            the client should forget an old key

        * XKEYSYNC CREATED pubkey_literal

            the client should remember a new key

        * XKEYSYNC OK

            completes a "packet" of updates; sent anytime that any amount of
            DELETED or CREATED events are detected/sent

    Code is patterned after dovecot/src/imap/cmd-idle.c, since the mechanics of
    XKEYSYNC (long-running, exitable with a DONE, etc) are very similar.
*/

#include "xkey.h"

struct cmd_xkeysync_context {
    struct client *client;
    struct client_command_context *cmd;

    struct timeout *keepalive_to;
    struct timeout *checker_to;

    link_t known_fprs;
    dstr_t uuid;
    char uuid_buffer[SMSQL_UUID_SIZE];
    int retries;
};

static void xkeysync_add_keepalive_timeout(struct cmd_xkeysync_context *ctx);
static void xkeysync_add_checker_timeout(struct cmd_xkeysync_context *ctx);

enum exit_msg {
    EXIT_EXPECTED_DONE = 0,
    EXIT_OK = 1,
    EXIT_INTERNAL_ERROR,
};

/* We only need to sort lists that the client sends us.  This is not an
   efficient sort but realistically we only need to sort a few fprs.  We can
   improve runtime performance by trying to be as fast as possible in the case
   that the client pre-sorts the list, and then writing our client to pre-sort
   the list. */
static void sort_fprs(link_t *fprs){
    // noop for zero- or one-length lists
    if(fprs->next->next == fprs) return;

    link_t temp;
    link_init(&temp);

    // start with the last element of fprs in temp
    link_t *link = link_list_pop_last(fprs);
    link_list_prepend(&temp, link);

    // then repeatedly put the last item before the first item it comes before
    while((link = link_list_pop_last(fprs))){
        smsql_dstr_t *new = CONTAINER_OF(link, smsql_dstr_t, link);
        smsql_dstr_t *old;
        bool placed = false;
        LINK_FOR_EACH(old, &temp, smsql_dstr_t, link){
            if(dstr_cmp(&new->dstr, &old->dstr) > 0){
                // new > old, keep going
                continue;
            }
            // treat old as a list head, and append new to that list
            // (same as inserting just before it in the list)
            link_list_append(&old->link, &new->link);
            placed = true;
            break;
        }
        if(!placed){
            // append new at the end of the whole list
            link_list_append(&temp, &new->link);
        }
    }

    // transfer all links back to fprs
    link_list_append_list(fprs, &temp);
}

static void free_fpr_list(link_t *fprs){
    link_t *link;
    while((link = link_list_pop_first(fprs))){
        smsql_dstr_t *fpr = CONTAINER_OF(link, smsql_dstr_t, link);
        smsql_dstr_free(&fpr);
    }
}

static smsql_dstr_t *list_first(link_t *head){
    if(link_list_isempty(head)) return NULL;
    return CONTAINER_OF(head->next, smsql_dstr_t, link);
}

static smsql_dstr_t *list_next(link_t *head, smsql_dstr_t *elem){
    if(!elem || elem->link.next == head) return NULL;
    return CONTAINER_OF(elem->link.next, smsql_dstr_t, link);
}

// return -1 for a < b
static int smsql_dstr_cmp(smsql_dstr_t *a, smsql_dstr_t *b){
    // treat NULL as the last element of any list
    if(!a) return 1;
    if(!b) return -1;
    return dstr_cmp(&a->dstr, &b->dstr);
}

static derr_t report_one_deleted(const dstr_t fpr, struct client *client){
    derr_t e = E_OK;

    DSTR_VAR(buf, SMSQL_FPR_SIZE + 32);
    NOFAIL(&e, E_FIXEDSIZE,
        FMT(&buf, "* XKEYSYNC DELETED %x", FD(fpr))
    );
    client_send_line(client, buf.data);

    return e;
}

static derr_t report_one_created(
    const dstr_t fpr, struct cmd_xkeysync_context *ctx, MYSQL *sql
){
    derr_t e = E_OK;

    // get the public key
    DSTR_VAR(pubkey, SMSQL_PUBKEY_SIZE);
    bool ok;
    PROP(&e, get_device(sql, ctx->uuid, fpr, &pubkey, &ok) );
    if(!ok){
        // race condition is possible, but a bug is far more likely.
        ORIG(&e, E_INTERNAL, "couldn't find the new key: race condition?");
    }

    // pass the pubkey back in an imap literal
    DSTR_VAR(buf, SMSQL_PUBKEY_SIZE + 64);
    NOFAIL(&e, E_FIXEDSIZE,
        FMT(&buf, "* XKEYSYNC CREATED {%x}\r\n%x", FU(pubkey.len), FD(pubkey))
    );
    client_send_line(ctx->client, buf.data);

    return e;
}

static derr_t report_changes(
    link_t *old_head,
    link_t *new_head,
    struct cmd_xkeysync_context *ctx,
    MYSQL *sql
){
    derr_t e = E_OK;
    bool change_detected = false;

    struct client *client = ctx->client;
    // client_send_line(client, "* OK XKEYSYNC report_changes");

    // walk through two sorted lists, looking for mismatches.
    smsql_dstr_t *old = list_first(old_head);
    smsql_dstr_t *new = list_first(new_head);
    while(old || new){
        int cmp = smsql_dstr_cmp(old, new);
        // {
        //     DSTR_VAR(buf, 256);
        //     FMT(
        //         &buf, "* OK XKEYSYNC old:%x, new=%x, cmp=%x",
        //         old ? FD(&old->dstr) : FS("null"),
        //         new ? FD(&new->dstr) : FS("null"),
        //         FI(cmp)
        //     );
        //     client_send_line(client, buf.data);
        // }
        if(cmp == 0){
            // matching strings is a no-op
            old = list_next(old_head, old);
            new = list_next(new_head, new);
            continue;
        }
        if(cmp < 0){
            // old < new, so old was deleted
            PROP(&e, report_one_deleted(old->dstr, client) );
            old = list_next(old_head, old);
            change_detected = true;
            continue;
        }else{
            // old > new, so new was created
            PROP(&e, report_one_created(new->dstr, ctx, sql) );
            new = list_next(new_head, new);
            change_detected = true;
            continue;
        }
    }

    // complete one "packet" of updates
    if(change_detected){
        client_send_line(ctx->client, "* XKEYSYNC OK");
        // since we used the network, reset the keepalive timeout
        xkeysync_add_keepalive_timeout(ctx);
    }

    // replace the old list with the new list on success
    free_fpr_list(old_head);
    link_list_append_list(old_head, new_head);

    return e;
}


// returns true on error
static bool xkeysync_check_now(struct cmd_xkeysync_context *ctx){
    derr_t e = E_OK;

    struct client *client = ctx->client;

    // look up the socket path
    const char *sock = mail_user_plugin_getenv(client->user, "sql_socket");
    if(sock == NULL || sock[0] == '\0'){
        sock = "/var/run/mysqld/mysqld.sock";
    }

    DSTR_VAR(d_sock, 256);
    PROP_GO(&e, FMT(&d_sock, "%x", FS(sock)), done);

    MYSQL sql;
    PROP_GO(&e, dmysql_init(&sql), done);

    PROP_GO(&e, sql_connect_unix(&sql, NULL, NULL, &d_sock), cu_sql);

    DSTR_VAR(email, SMSQL_EMAIL_SIZE);
    bool ok;
    PROP_GO(&e, get_email_for_uuid(&sql, ctx->uuid, &email, &ok), cu_sql);

    // get the keys from the database (comes pre-sorted)
    link_t all_fprs;
    link_init(&all_fprs);
    PROP_GO(&e, list_device_fprs(&sql, ctx->uuid, &all_fprs), cu_sql);
    // {
    //     DSTR_VAR(buf, 256);
    //     FMT(&buf, "* OK fpr list:");
    //     client_send_line(ctx->client, buf.data);
    //     smsql_dstr_t *fpr;
    //     LINK_FOR_EACH(fpr, &all_fprs, smsql_dstr_t, link){
    //         DSTR_VAR(buf, 256);
    //         FMT(&buf, "* OK     %x", FD(&fpr->dstr));
    //         client_send_line(ctx->client, buf.data);
    //     }
    // }

    PROP_GO(&e,
        report_changes(&ctx->known_fprs, &all_fprs, ctx, &sql),
    cu_fprs);

cu_fprs:
    free_fpr_list(&all_fprs);

cu_sql:
    mysql_close(&sql);

    bool retval;
done:
    retval = is_error(e);
    if(is_error(e)){
        badbadbad_alert(DSTR_LIT("error in xkeysync_check_now()"), e.msg);
        DUMP(e);
        DROP_VAR(&e);
    }

    // always come back later
    xkeysync_add_checker_timeout(ctx);

    return retval;
}

/* clean up our cmd_xkeysync_context and free the ctx->cmd if we finish in an
   entrypoint other than a command_func_t */
static void xkeysync_finish(
    struct cmd_xkeysync_context *ctx, enum exit_msg exit_msg, bool free_cmd
){
    struct client *client = ctx->client;

    // clean up the ctx
    free_fpr_list(&ctx->known_fprs);

    // remove all callbacks from io loop
    timeout_remove(&ctx->keepalive_to);
    timeout_remove(&ctx->checker_to);

    /* Why does idle_finish have a cork/uncork?

       cork/uncork doesn't seem to be nestable (it's more like a flag than a
       reference count), so the cork will sometimes be redundant and the
       uncork will sometimes render some other uncork higher in the callstack
       redundant.

       I guess you know that idle_finish is the chance to write output, so the
       uncork higher in the stack that becomes redundant doesn't matter, since
       there won't be any output between our uncork here and that one. */
    o_stream_cork(client->output);

    // not sure why this is inside the cork; but it is for idle_finish()
    io_remove(&client->io);

    switch(exit_msg){
        case EXIT_EXPECTED_DONE:
            client_send_tagline(ctx->cmd, "BAD expected DONE");
            break;
        case EXIT_OK:
            client_send_tagline(ctx->cmd, "OK xkeysync complete");
            break;
        case EXIT_INTERNAL_ERROR:
            client_send_tagline(ctx->cmd, "BAD internal server failure");
            break;
    }

    o_stream_uncork(client->output);

    // I'm not totally sure why this is required sometimes but not others...
    // But I'm trying to call it at the same times as cmd-idle.c does.
    if(free_cmd){
        client_command_free(&ctx->cmd);
    }
}

static void keepalive_timeout(struct cmd_xkeysync_context *ctx){

    /* cmd-idle.c checks if ctx->client->output_cmd_lock is set and avoids
       emitting anything here.  That only gets set by FETCH, URLFETCH, and
       GETMETADATA commands, which our XKEYSYNC caller will never ever invoke,
       so we ignore that case entirely. */

    if(o_stream_get_buffer_used_size(ctx->client->output) == 0){
        // send any packet to wake up NATs or stateful firewalls
        o_stream_cork(ctx->client->output);
        client_send_line(ctx->client, "* OK still syncing");
        o_stream_uncork(ctx->client->output);
    }

    /* cmd-idle.c resets this timeout, saying that it's to keep idling
       connections from getting disconnected */
    timeout_reset(ctx->client->to_idle);

    // calculate next keepalive timeout
    xkeysync_add_keepalive_timeout(ctx);
}

static void xkeysync_add_keepalive_timeout(struct cmd_xkeysync_context *ctx){
    /* Reuse the keepalive time for IDLE to reuse dovecot's system for
       synchronizing keepalives to mobile devices */
    struct client *client = ctx->client;
    unsigned int interval = client->set->imap_idle_notify_interval;
    interval = imap_keepalive_interval_msecs(
        client->user->username,
        client->user->conn.remote_ip,
        interval
    );

    timeout_remove(&ctx->keepalive_to);

    /* no idea why timeout_add trigglers a vla warning, but we are not going
       to not have a timeout, so it seems fine to ignore it */
#   ifdef __GNUC__
#   pragma GCC diagnostic push
#   pragma GCC diagnostic ignored "-Wvla"
#   pragma GCC diagnostic ignored "-Wconversion"
#   endif // __GNUC__
    ctx->keepalive_to = timeout_add(interval, keepalive_timeout, ctx);
#   ifdef __GNUC__
#   pragma GCC diagnostic pop
#   endif // __GNUC__
}

static void checker_timeout(struct cmd_xkeysync_context *ctx){
    struct client *client = ctx->client;

    // cmd-idle.c checks if ctx->client->output_cmd_lock here, too

    o_stream_cork(client->output);
    bool failure = xkeysync_check_now(ctx);
    o_stream_uncork(client->output);

    if(failure){
        // allow temporary failures gracefully
        if(ctx->retries++ >= 5){
            // free the command, since this is not a command_func_t entrypoint
            xkeysync_finish(ctx, EXIT_INTERNAL_ERROR, true);
        }
    }else{
        // reset retry count
        ctx->retries = 0;
    }
}

static void xkeysync_add_checker_timeout(struct cmd_xkeysync_context *ctx){
    unsigned int interval = 3000;  // milliseconds
    timeout_remove(&ctx->checker_to);

#   ifdef __GNUC__
#   pragma GCC diagnostic push
#   pragma GCC diagnostic ignored "-Wvla"
#   pragma GCC diagnostic ignored "-Wconversion"
#   endif // __GNUC__
    ctx->checker_to = timeout_add(interval, checker_timeout, ctx);
#   ifdef __GNUC__
#   pragma GCC diagnostic pop
#   endif // __GNUC__
}

/* hardcode the only string comparison we need to do, to avoid linking into
   extra dovecot libraries */
static enum exit_msg is_done(const char *s){
    const char DONE[] = "DONE";
    const char done[] = "done";
    for(size_t i = 0; i < sizeof(DONE); i++){
        if(s[i] != DONE[i] && s[i] != done[i]) return EXIT_EXPECTED_DONE;
    }
    return EXIT_OK;
}

// process input, return true if the command has finished
static bool xkeysync_client_handle_input(
    struct cmd_xkeysync_context *ctx, bool free_cmd
){
    const char *line;
    while((line = i_stream_next_line(ctx->client->input)) != NULL){
        if(ctx->client->input_skip_line){
            ctx->client->input_skip_line = false;
        } else {
            // the first full line we recieve is always the only line of input
            xkeysync_finish(ctx, is_done(line), free_cmd);
            return true;
        }
    }
    return false;
}

// callback on new inputs: process input and maybe trigger the next command
static void xkeysync_client_input(struct cmd_xkeysync_context *ctx){
    struct client *client = ctx->client;

    client->last_input = ioloop_time;
    timeout_reset(client->to_idle);

    switch(i_stream_read(client->input)){
        case -1: // disconnected
            client_disconnect(client, NULL);
            /* client_continue_pending_input() handles the disconnected case
               specially somehow, and we should call it (cmd-idle.c does) */
            goto cmd_finished;
        case -2: // input buffer is full
            client->input_skip_line = true;
            xkeysync_finish(ctx, EXIT_INTERNAL_ERROR, true);
            goto cmd_finished;
    }

    // we read successfully!

    if(xkeysync_client_handle_input(ctx, true)){
        goto cmd_finished;
    }

    return;

cmd_finished:
    client_continue_pending_input(client);
    return;
}

/* cmd_xkeysync_continue is a "secondary" command_func_t, it's what we set as
   the cmd->func after the command initially starts */
static bool cmd_xkeysync_continue(struct client_command_context *cmd){
    struct cmd_xkeysync_context *ctx = cmd->context;
    struct client *client = cmd->client;

    if(cmd->cancel){
        /* client_command_cancel() calls us with cmd->cancel == TRUE and always
           calls client_command_free right afterwards, so we set free_cmd=false
           here */
        xkeysync_finish(ctx, EXIT_INTERNAL_ERROR, false);
        return true;
    }

    if(client->output->closed){
        xkeysync_finish(ctx, EXIT_INTERNAL_ERROR, false);
        return true;
    }

    /* TODO: when does this get called?  It seems we always exit out of it but
       I don't fully understand if that's the right call */
    fprintf(stderr, "cmd_xkeysync_continue() called for unknown reason!\n");
    xkeysync_finish(ctx, EXIT_INTERNAL_ERROR, false);
    return true;
}

// cmd_xkeysync is a command_func_t
bool cmd_xkeysync(struct client_command_context *cmd){
    derr_t e = E_OK;

    // struct client *client = cmd->client;
    const struct imap_arg *args;
    const char *arg;

    if(!client_read_args(cmd, 0, 0, &args))
        return false;

    link_t known_fprs;
    link_init(&known_fprs);

    for(size_t i = 0; !IMAP_ARG_IS_EOL(&args[i]); i++){
        // validate arg
        if(!imap_arg_get_astring(&args[i], &arg)
                || strlen(arg) != SMSQL_FPR_SIZE){
            client_send_command_error(cmd, "invalid fingerprint");
            goto fail_fprs;
        }
        // remember arg
        smsql_dstr_t *fpr;
        IF_PROP(&e, smsql_dstr_new_cstr(&fpr, arg) ){
            client_send_command_error(cmd, "failed to allocate in xkeysync");
            goto fail_fprs;
        }
        link_list_append(&known_fprs, &fpr->link);
    }

    // sort fprs from the client
    sort_fprs(&known_fprs);

    struct client *client = cmd->client;

    // get the user's uuid from their FSID@x.splintermail.com login result
    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    DSTR_VAR(fsid, SMSQL_FSID_SIZE);
    if(client->user->username != NULL){
        fsid.len = MIN(fsid.size, strlen(client->user->username));
        memcpy(fsid.data, client->user->username, fsid.len);
    }
    IF_PROP(&e, to_uuid(fsid, &uuid) ){
        client_send_command_error(
            cmd, "internal error: failed to decode user_uuid from login"
        );
        goto fail_fprs;
    }

    // allocate a new ctx
    struct cmd_xkeysync_context *ctx;
    ctx = p_new(cmd->pool, struct cmd_xkeysync_context, 1);
    ctx->cmd = cmd;
    ctx->client = client;
    link_init(&ctx->known_fprs);
    link_list_append_list(&ctx->known_fprs, &known_fprs);
    ctx->retries = 0;

    DSTR_WRAP_ARRAY(ctx->uuid, ctx->uuid_buffer);
    IF_PROP(&e, dstr_copy(&uuid, &ctx->uuid) ){
        // this can't happen
        client_send_command_error(
            cmd, "internal error: hit an unreachable error"
        );
        goto fail_ctx;
    }

    /* read input from client; we don't really need to support DONE but it
       best to have some way to cleanup our state, so we do support it.  That
       also lets us handle client disconnects easily. */
    io_remove(&client->io);
    client->io = io_add_istream(client->input, xkeysync_client_input, ctx);

    cmd->func = cmd_xkeysync_continue;
    cmd->context = ctx;
    cmd->state = CLIENT_COMMAND_STATE_WAIT_INPUT;

    o_stream_cork(client->output);

    // do the first check now
    if(xkeysync_check_now(ctx)){
        o_stream_uncork(client->output);
        // first check failed
        xkeysync_finish(ctx, EXIT_INTERNAL_ERROR, false);
        return true;
    }

    // initial synchronization point; now it is ok to send DONE
    client_send_line(client, "+ OK");
    o_stream_uncork(client->output);

    xkeysync_add_keepalive_timeout(ctx);

    return xkeysync_client_handle_input(ctx, false);

fail_ctx:
    xkeysync_finish(ctx, EXIT_INTERNAL_ERROR, false);
fail_fprs:
    badbadbad_alert(DSTR_LIT("error in cmd_xkeysync"), e.msg);
    DUMP(e);
    DROP_VAR(&e);
    free_fpr_list(&known_fprs);
    return true;
}
