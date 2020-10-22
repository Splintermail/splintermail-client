#include "citm.h"

static void sf_pair_free_append(sf_pair_t *sf_pair){
    passthru_req_free(STEAL(passthru_req_t, &sf_pair->append.req));
    passthru_resp_free(STEAL(passthru_resp_t, &sf_pair->append.resp));
    dirmgr_hold_free(sf_pair->append.hold);
    sf_pair->append.hold = NULL;
    if(sf_pair->append.tmp_id){
        DSTR_VAR(file, 32);
        // this can't actually fail in practice
        DROP_CMD( FMT(&file, "%x", FU(sf_pair->append.tmp_id)) );
        string_builder_t tmp_path =
            sb_append(&sf_pair->dirmgr->path, FS("tmp"));
        string_builder_t path = sb_append(&tmp_path, FD(&file));
        DROP_CMD( remove_path(&path) );
        sf_pair->append.tmp_id = 0;
    }
    return;
}

void sf_pair_free(sf_pair_t **old){
    sf_pair_t *sf_pair = *old;
    if(!sf_pair) return;

    if(sf_pair->registered_with_keyshare){
        keyshare_unregister(sf_pair->keyshare, &sf_pair->key_listener);
    }

    link_t *link;
    while((link = link_list_pop_first(&sf_pair->keys))){
        keypair_t *kp = CONTAINER_OF(link, keypair_t, link);
        keypair_free(&kp);
    }

    // free state generated at runtime
    ie_login_cmd_free(sf_pair->login_cmd);
    sf_pair_free_append(sf_pair);
    dstr_free(&sf_pair->username);
    dstr_free(&sf_pair->password);

    // TODO: it is possible that the server still has a wake_event_t in a queue!
    // I'm not sure quite how but I hit it once
    server_free(&sf_pair->server);
    fetcher_free(&sf_pair->fetcher);

    refs_free(&sf_pair->refs);
    free(sf_pair);

    *old = NULL;
}

static void sf_pair_finalize(refs_t *refs){
    sf_pair_t *sf_pair = CONTAINER_OF(refs, sf_pair_t, refs);
    sf_pair->cb->release(sf_pair->cb, sf_pair);
}

void sf_pair_close(sf_pair_t *sf_pair, derr_t error){
    bool do_close = !sf_pair->closed;
    sf_pair->closed = true;

    if(!do_close){
        // drop secondary errors
        DROP_VAR(&error);
        return;
    }

    server_close(&sf_pair->server, E_OK);
    fetcher_close(&sf_pair->fetcher, E_OK);

    sf_pair->cb->dying(sf_pair->cb, sf_pair, error);
    PASSED(error);
}

static imap_time_t imap_time_now(void){
    // get epochtime
    time_t epoch;
    time_t time_ret = time(&epoch);
    if(time_ret < 0){
        // if this fails... just use zero
        epoch = ((time_t) 0);
    }

    // convert to struct tm
    struct tm tm, *localtime_ret;
    localtime_ret = localtime_r(&epoch, &tm);
    if(localtime_ret != &tm){
        return (imap_time_t){0};
    }

    // get the timezone, sets extern long timezone to a signed second offset
    tzset();
    int z_hour = 0;
    int z_min = 0;
    if(timezone < 3600 * 24 && timezone > -3600 * 24){
        z_hour = (int)timezone / 3600;
        z_min = ABS((int)timezone - z_hour * 3600) / 60;
    }

    return (imap_time_t){
        .year = tm.tm_year + 1900,
        .month = tm.tm_mon + 1,
        .day = tm.tm_mday,
        .min = tm.tm_min,
        .sec = tm.tm_sec,
        .z_hour = z_hour,
        .z_min = z_min,
    };
}

// we will modify the content of the append command directly
static derr_t sf_pair_append_req(sf_pair_t *sf_pair){
    derr_t e = E_OK;

    ie_append_cmd_t *append = sf_pair->append.req->arg.append;

    // step 1: write the unencrytped text to a file for saving
    sf_pair->append.tmp_id = dirmgr_new_tmp_id(sf_pair->dirmgr);
    DSTR_VAR(file, 32);
    PROP_GO(&e, FMT(&file, "%x", FU(sf_pair->append.tmp_id)), fail);
    string_builder_t tmp_path =
        sb_append(&sf_pair->dirmgr->path, FS("tmp"));
    string_builder_t path = sb_append(&tmp_path, FD(&file));
    PROP_GO(&e, dstr_fwrite_path(&path, &append->content->dstr), fail);

    // step 2: copy some details from the APPEND command
    sf_pair->append.len = append->content->dstr.len;
    sf_pair->append.flags = msg_flags_from_flags(append->flags);
    if(append->time.year){
        // an explicit intdate was passed in
        sf_pair->append.intdate = append->time;
    }else{
        // use the time right now
        sf_pair->append.intdate = imap_time_now();
        // also pass that value to the server to ensure that we are synced
        append->time = sf_pair->append.intdate;
    }

    // step 3: start a hold on the mailbox
    PROP_GO(&e,
        dirmgr_hold_new(
            sf_pair->dirmgr,
            ie_mailbox_name(append->m),
            &sf_pair->append.hold
        ),
    fail);

    // step 4: encrypt the text to all the keys we know of
    ie_dstr_t *content = ie_dstr_new_empty(&e);
    CHECK_GO(&e, fail);

    encrypter_t ec;
    PROP_GO(&e, encrypter_new(&ec), cu_content);
    PROP_GO(&e, encrypter_start(&ec, &sf_pair->keys, &content->dstr), cu_ec);
    PROP_GO(&e,
        encrypter_update(&ec, &append->content->dstr, &content->dstr),
    cu_ec);
    PROP_GO(&e, encrypter_finish(&ec, &content->dstr), cu_ec);

    // step 5: modify the APPEND and relay it to the fetcher
    ie_dstr_free(append->content);
    append->content = STEAL(ie_dstr_t, &content);
    fetcher_passthru_req(
        &sf_pair->fetcher, STEAL(passthru_req_t, &sf_pair->append.req)
    );

cu_ec:
    encrypter_free(&ec);
cu_content:
    ie_dstr_free(content);

fail:
    if(is_error(e)) sf_pair_free_append(sf_pair);

    return e;
}

static derr_t sf_pair_append_resp(sf_pair_t *sf_pair){
    derr_t e = E_OK;

    const ie_st_resp_t *st_resp = sf_pair->append.resp->st_resp;

    if(st_resp->status != IE_ST_OK){
        // just relay and cleanup
        goto relay;
    }

    // snag the uid from the APPENDUID status code
    if(st_resp->code->type != IE_ST_CODE_APPENDUID){
        ORIG_GO(&e, E_RESPONSE, "expected APPENDUID in APPEND response", cu);
    }
    unsigned int uidvld_up = st_resp->code->arg.appenduid.uidvld;
    unsigned int uid_up = st_resp->code->arg.appenduid.uid;

    // get the path to the temporary file
    DSTR_VAR(file, 32);
    PROP_GO(&e, FMT(&file, "%x", FU(sf_pair->append.tmp_id)), cu);
    string_builder_t tmp_path =
        sb_append(&sf_pair->dirmgr->path, FS("tmp"));
    string_builder_t path = sb_append(&tmp_path, FD(&file));

    // add the temporary file to the maildir
    PROP_GO(&e,
        dirmgr_hold_add_local_file(
            sf_pair->append.hold,
            &path,
            uidvld_up,
            uid_up,
            sf_pair->append.len,
            sf_pair->append.intdate,
            sf_pair->append.flags
        ),
    cu);

    // done with temporary file
    sf_pair->append.tmp_id = 0;

relay:
    server_passthru_resp(
        &sf_pair->server, STEAL(passthru_resp_t, &sf_pair->append.resp)
    );

cu:
    sf_pair_free_append(sf_pair);

    return e;
}

static void sf_pair_enqueue(sf_pair_t *sf_pair){
    if(sf_pair->closed || sf_pair->enqueued) return;
    sf_pair->enqueued = true;
    // ref_up for wake_ev
    ref_up(&sf_pair->refs);
    sf_pair->engine->pass_event(sf_pair->engine, &sf_pair->wake_ev.ev);
}

static void sf_pair_wakeup(wake_event_t *wake_ev){
    sf_pair_t *sf_pair = CONTAINER_OF(wake_ev, sf_pair_t, wake_ev);
    sf_pair->enqueued = false;
    // ref_dn for wake_ev
    ref_dn(&sf_pair->refs);

    derr_t e = E_OK;
    // what did we wake up for?
    if(sf_pair->login_result){
        sf_pair->login_result = false;
        // request an owner
        PROP_GO(&e,
            sf_pair->cb->request_owner(
                sf_pair->cb,
                sf_pair,
                &sf_pair->username,
                &sf_pair->password
            ),
        fail);
    }else if(sf_pair->login_cmd){
        // save the username and password in case the login succeeds
        PROP_GO(&e,
            dstr_copy(&sf_pair->login_cmd->user->dstr, &sf_pair->username),
        fail);

        PROP_GO(&e,
            dstr_copy(&sf_pair->login_cmd->pass->dstr, &sf_pair->password),
        fail);

        fetcher_login(
            &sf_pair->fetcher, STEAL(ie_login_cmd_t, &sf_pair->login_cmd)
        );
    }else if(sf_pair->got_owner_resp){
        sf_pair->got_owner_resp = false;

        // register with the keyshare
        PROP_GO(&e,
            keyshare_register(
                sf_pair->keyshare, &sf_pair->key_listener, &sf_pair->keys
            ),
        fail);
        sf_pair->registered_with_keyshare = true;

        // share the dirmgr with the server and the fetcher
        fetcher_set_dirmgr(&sf_pair->fetcher, sf_pair->dirmgr);
        server_set_dirmgr(&sf_pair->server, sf_pair->dirmgr);

        /* at this point we have successfully:
             - logged in on the fetcher_t
             - created or selected a user_t
             - the user_t's keyfetcher has synchronized the keyshare
             - registered with the keyshare
           and it is safe for the server_t to continue */
        server_login_result(&sf_pair->server, true);
    }else if(sf_pair->append.req){
        PROP_GO(&e, sf_pair_append_req(sf_pair), fail);
    }else if(sf_pair->append.resp){
        PROP_GO(&e, sf_pair_append_resp(sf_pair), fail);
    }else{
        LOG_ERROR("sf_pair_wakeup() for no reason\n");
    }
    return;

fail:
    sf_pair_close(sf_pair, e);
}

// the interface the server/fetcher provides to its owner
void sf_pair_owner_resp(sf_pair_t *sf_pair, dirmgr_t *dirmgr,
        keyshare_t *keyshare){
    sf_pair->got_owner_resp = true;
    sf_pair->dirmgr = dirmgr;
    sf_pair->keyshare = keyshare;
    sf_pair_enqueue(sf_pair);
}

// part of key_listener_i
static void key_listener_add_key(key_listener_i *key_listener, keypair_t *kp){
    sf_pair_t *sf_pair = CONTAINER_OF(key_listener, sf_pair_t, key_listener);
    link_list_append(&sf_pair->keys, &kp->link);
}

// part of key_listener_i
static void key_listener_del_key(key_listener_i *key_listener,
        const dstr_t *fingerprint){
    sf_pair_t *sf_pair = CONTAINER_OF(key_listener, sf_pair_t, key_listener);

    keypair_t *kp, *temp;
    LINK_FOR_EACH_SAFE(kp, temp, &sf_pair->keys, keypair_t, link){
        if(dstr_cmp(fingerprint, kp->fingerprint) == 0){
            link_remove(&kp->link);
            keypair_free(&kp);
            return;
        }
    }
    LOG_ERROR("key_listener_del_key did not find matching fingerprint!\n");
}

// part of server_cb_i
static void server_cb_dying(server_cb_i *cb, derr_t error){
    // printf("---- server_cb_dying\n");
    sf_pair_t *sf_pair = CONTAINER_OF(cb, sf_pair_t, server_cb);

    sf_pair_close(sf_pair, error);
}

// part of server_cb_i
static void server_cb_release(server_cb_i *cb){
    // printf("---- server_cb_release\n");
    sf_pair_t *sf_pair = CONTAINER_OF(cb, sf_pair_t, server_cb);

    // ref down for the server
    ref_dn(&sf_pair->refs);
}


// part of server_cb_i
static void server_cb_login(server_cb_i *server_cb,
        ie_login_cmd_t *login_cmd){
    // printf("---- server_cb_login\n");
    sf_pair_t *sf_pair = CONTAINER_OF(server_cb, sf_pair_t, server_cb);
    // the copy of credentials can fail, so enqueue the work
    sf_pair->login_cmd = login_cmd;
    sf_pair_enqueue(sf_pair);
}

// part of server_cb_i
static void server_cb_passthru_req(server_cb_i *server_cb,
        passthru_req_t *passthru_req){
    // printf("---- server_cb_passthru_req\n");
    sf_pair_t *sf_pair = CONTAINER_OF(server_cb, sf_pair_t, server_cb);
    if(passthru_req->type == PASSTHRU_APPEND){
        // intercept APPEND requests, and process them asynchronously
        sf_pair->append.req = passthru_req;
        sf_pair_enqueue(sf_pair);
        return;
    }
    fetcher_passthru_req(&sf_pair->fetcher, passthru_req);
}

// part of server_cb_i
static void server_cb_select(server_cb_i *server_cb, ie_mailbox_t *m,
        bool examine){
    // printf("---- server_cb_select\n");
    sf_pair_t *sf_pair = CONTAINER_OF(server_cb, sf_pair_t, server_cb);
    fetcher_select(&sf_pair->fetcher, m, examine);
}


// part of fetcher_cb_i
static void fetcher_cb_dying(fetcher_cb_i *cb, derr_t error){
    // printf("---- fetcher_cb_dying\n");
    sf_pair_t *sf_pair = CONTAINER_OF(cb, sf_pair_t, fetcher_cb);
    sf_pair_close(sf_pair, error);
}

// part of fetcher_cb_i
static void fetcher_cb_release(fetcher_cb_i *cb){
    // printf("---- fetcher_cb_release\n");
    sf_pair_t *sf_pair = CONTAINER_OF(cb, sf_pair_t, fetcher_cb);
    // ref down for the fetcher
    ref_dn(&sf_pair->refs);
}

// part of the fetcher_cb_i
static void fetcher_cb_login_ready(fetcher_cb_i *fetcher_cb){
    // printf("---- fetcher_cb_login_ready\n");
    sf_pair_t *sf_pair = CONTAINER_OF(fetcher_cb, sf_pair_t, fetcher_cb);
    server_allow_greeting(&sf_pair->server);
}

// part of the fetcher_cb_i
static void fetcher_cb_login_result(fetcher_cb_i *fetcher_cb,
        bool login_result){
    // printf("---- fetcher_cb_login_result\n");
    sf_pair_t *sf_pair = CONTAINER_OF(fetcher_cb, sf_pair_t, fetcher_cb);

    if(login_result){
        // the set_owner call can fail, so enqueue the work
        sf_pair->login_result = true;
        sf_pair_enqueue(sf_pair);
    }else{
        // pass failures through immediately
        server_login_result(&sf_pair->server, false);
    }
}

// part of fetcher_cb_i
static void fetcher_cb_passthru_resp(fetcher_cb_i *fetcher_cb,
        passthru_resp_t *passthru_resp){
    // printf("---- fetcher_cb_passthru_resp\n");
    sf_pair_t *sf_pair = CONTAINER_OF(fetcher_cb, sf_pair_t, fetcher_cb);
    if(passthru_resp->type == PASSTHRU_APPEND){
        // intercept APPEND responses, and process them asynchronously
        sf_pair->append.resp = passthru_resp;
        sf_pair_enqueue(sf_pair);
        return;
    }
    server_passthru_resp(&sf_pair->server, passthru_resp);
}

// part of the fetcher_cb_i
static void fetcher_cb_select_result(fetcher_cb_i *fetcher_cb,
        ie_st_resp_t *st_resp){
    // printf("---- fetcher_cb_select_result\n");
    sf_pair_t *sf_pair = CONTAINER_OF(fetcher_cb, sf_pair_t, fetcher_cb);
    server_select_result(&sf_pair->server, st_resp);
}


derr_t sf_pair_new(
    sf_pair_t **out,
    sf_pair_cb_i *cb,
    engine_t *engine,
    const char *remote_host,
    const char *remote_svc,
    imap_pipeline_t *p,
    ssl_context_t *ctx_srv,
    ssl_context_t *ctx_cli,
    session_t **session
){
    derr_t e = E_OK;

    *out = NULL;
    *session = NULL;

    sf_pair_t *sf_pair = malloc(sizeof(*sf_pair));
    if(!sf_pair) ORIG(&e, E_NOMEM, "nomem");
    *sf_pair = (sf_pair_t){
        .cb = cb,
        .engine = engine,
        .server_cb = {
            .dying = server_cb_dying,
            .release = server_cb_release,
            .login = server_cb_login,
            .passthru_req = server_cb_passthru_req,
            .select = server_cb_select,
        },
        .fetcher_cb = {
            .dying = fetcher_cb_dying,
            .release = fetcher_cb_release,
            .login_ready = fetcher_cb_login_ready,
            .login_result = fetcher_cb_login_result,
            .passthru_resp = fetcher_cb_passthru_resp,
            .select_result = fetcher_cb_select_result,
        },
        .key_listener = {
            .add = key_listener_add_key,
            .del = key_listener_del_key,
        },
    };

    link_init(&sf_pair->citme_link);
    link_init(&sf_pair->user_link);
    link_init(&sf_pair->key_listener.link);
    link_init(&sf_pair->keys);

    event_prep(&sf_pair->wake_ev.ev, NULL, NULL);
    sf_pair->wake_ev.ev.ev_type = EV_INTERNAL;
    sf_pair->wake_ev.handler = sf_pair_wakeup;

    // start with a server ref and a fetcher ref
    PROP_GO(&e, refs_init(&sf_pair->refs, 2, sf_pair_finalize), fail_malloc);

    PROP_GO(&e,
        server_init(
            &sf_pair->server,
            &sf_pair->server_cb,
            p,
            engine,
            ctx_srv,
            session),
        fail_refs);

    PROP_GO(&e,
        fetcher_init(
            &sf_pair->fetcher,
            &sf_pair->fetcher_cb,
            remote_host,
            remote_svc,
            p,
            engine,
            ctx_cli),
        fail_server);

    *out = sf_pair;

    return e;

fail_server:
    server_free(&sf_pair->server);
fail_refs:
    refs_free(&sf_pair->refs);
fail_malloc:
    free(sf_pair);
    return e;
}

void sf_pair_start(sf_pair_t *sf_pair){
    server_start(&sf_pair->server);
    fetcher_start(&sf_pair->fetcher);
}
