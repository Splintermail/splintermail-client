#include "citm.h"

void sf_pair_free(sf_pair_t **old){
    sf_pair_t *sf_pair = *old;
    if(!sf_pair) return;

    // free state generated at runtime
    ie_login_cmd_free(sf_pair->login_cmd);
    dstr_free(&sf_pair->username);
    dstr_free(&sf_pair->password);

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
            sf_pair->cb->set_owner(
                sf_pair->cb,
                sf_pair,
                &sf_pair->username,
                &sf_pair->password,
                &sf_pair->owner),
            fail);
        server_login_result(&sf_pair->server, true);
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
    }else{
        LOG_ERROR("sf_pair_wakeup() for no reason\n");
    }
    return;

fail:
    sf_pair_close(sf_pair, e);
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
    fetcher_passthru_req(&sf_pair->fetcher, passthru_req);
}

// part of server_cb_i
static void server_cb_select(server_cb_i *server_cb, ie_mailbox_t *m){
    // printf("---- server_cb_select\n");
    sf_pair_t *sf_pair = CONTAINER_OF(server_cb, sf_pair_t, server_cb);
    fetcher_select(&sf_pair->fetcher, m);
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
    };

    link_init(&sf_pair->citme_link);
    link_init(&sf_pair->user_link);

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
