#include "citm.h"

static void sf_pair_finalize(refs_t *refs){
    sf_pair_t *sf_pair = CONTAINER_OF(refs, sf_pair_t, refs);

    // free state generated at runtime
    dstr_free(&sf_pair->username);

    refs_free(&sf_pair->refs);
    free(sf_pair);
}

void sf_pair_close(sf_pair_t *sf_pair, derr_t error){
    uv_mutex_lock(&sf_pair->mutex);
    bool do_close = !sf_pair->closed;
    sf_pair->closed = true;
    uv_mutex_unlock(&sf_pair->mutex);

    if(!do_close){
        // drop secondary errors
        DROP_VAR(&error);
        return;
    }

    server_close(sf_pair->server, E_OK);
    server_release(sf_pair->server);

    fetcher_close(sf_pair->fetcher, E_OK);
    fetcher_release(sf_pair->fetcher);

    sf_pair->cb->dying(sf_pair->cb, sf_pair, error);
    PASSED(error);
}

// part of server_cb_i
static void server_cb_dying(server_cb_i *cb, derr_t error){
    sf_pair_t *sf_pair = CONTAINER_OF(cb, sf_pair_t, server_cb);

    sf_pair_close(sf_pair, error);

    // ref down for the server
    ref_dn(&sf_pair->refs);
}

// part of server_cb_i
static derr_t server_cb_login(server_cb_i *server_cb, const ie_dstr_t *user,
        const ie_dstr_t *pass){
    derr_t e = E_OK;

    sf_pair_t *sf_pair = CONTAINER_OF(server_cb, sf_pair_t, server_cb);
    uv_mutex_lock(&sf_pair->mutex);
    if(sf_pair->closed) ORIG_GO(&e, E_DEAD, "sf_pair is closed", unlock);

    // save the username in case it succeeds
    PROP_GO(&e, dstr_copy(&user->dstr, &sf_pair->username), unlock);

    PROP_GO(&e, fetcher_login(sf_pair->fetcher, user, pass), unlock);

unlock:
    uv_mutex_unlock(&sf_pair->mutex);
    return e;
}

// part of server_cb_i
static derr_t server_cb_passthru_req(server_cb_i *server_cb,
        passthru_req_t *passthru){
    derr_t e = E_OK;

    sf_pair_t *sf_pair = CONTAINER_OF(server_cb, sf_pair_t, server_cb);
    uv_mutex_lock(&sf_pair->mutex);
    if(sf_pair->closed){
        passthru_req_free(passthru);
        ORIG_GO(&e, E_DEAD, "sf_pair is closed", unlock);
    }

    PROP_GO(&e, fetcher_passthru_req(sf_pair->fetcher, passthru), unlock);
    goto unlock;

unlock:
    uv_mutex_unlock(&sf_pair->mutex);
    return e;
}

// part of server_cb_i
static derr_t server_cb_select(server_cb_i *server_cb, const ie_mailbox_t *m){
    derr_t e = E_OK;

    sf_pair_t *sf_pair = CONTAINER_OF(server_cb, sf_pair_t, server_cb);
    uv_mutex_lock(&sf_pair->mutex);
    if(sf_pair->closed) ORIG_GO(&e, E_DEAD, "sf_pair is closed", unlock);

    PROP_GO(&e, fetcher_select(sf_pair->fetcher, m), unlock);

unlock:
    uv_mutex_unlock(&sf_pair->mutex);
    return e;
}


// part of fetcher_cb_i
static void fetcher_cb_dying(fetcher_cb_i *cb, derr_t error){
    sf_pair_t *sf_pair = CONTAINER_OF(cb, sf_pair_t, fetcher_cb);

    sf_pair_close(sf_pair, error);

    // ref down for the fetcher
    ref_dn(&sf_pair->refs);
}

// part of the fetcher_cb_i
static derr_t fetcher_cb_login_ready(fetcher_cb_i *fetcher_cb){
    derr_t e = E_OK;

    sf_pair_t *sf_pair = CONTAINER_OF(fetcher_cb, sf_pair_t, fetcher_cb);
    uv_mutex_lock(&sf_pair->mutex);

    // allow the server to proceed
    PROP_GO(&e, server_allow_greeting(sf_pair->server), unlock);

unlock:
    uv_mutex_unlock(&sf_pair->mutex);
    return e;
}

// part of the fetcher_cb_i
static derr_t fetcher_cb_login_failed(fetcher_cb_i *fetcher_cb){
    derr_t e = E_OK;

    sf_pair_t *sf_pair = CONTAINER_OF(fetcher_cb, sf_pair_t, fetcher_cb);
    uv_mutex_lock(&sf_pair->mutex);

    PROP_GO(&e, server_login_failed(sf_pair->server), unlock);

unlock:
    uv_mutex_unlock(&sf_pair->mutex);
    return e;
}

// part of the fetcher_cb_i
static derr_t fetcher_cb_login_succeeded(
        fetcher_cb_i *fetcher_cb, dirmgr_t **out){
    derr_t e = E_OK;

    sf_pair_t *sf_pair = CONTAINER_OF(fetcher_cb, sf_pair_t, fetcher_cb);
    uv_mutex_lock(&sf_pair->mutex);

    // request an owner
    dirmgr_t *dirmgr;
    PROP_GO(&e,
        sf_pair->cb->set_owner(
            sf_pair->cb,
            sf_pair,
            &sf_pair->username,
            &dirmgr,
            &sf_pair->owner),
        unlock);

    // share the exciting news with the fetcher and the server
    *out = dirmgr;
    PROP_GO(&e, server_login_succeeded(sf_pair->server, dirmgr), unlock);

unlock:
    uv_mutex_unlock(&sf_pair->mutex);
    return e;
}

// part of fetcher_cb_i
static derr_t fetcher_cb_passthru_resp(fetcher_cb_i *fetcher_cb,
        passthru_resp_t *passthru){
    derr_t e = E_OK;

    sf_pair_t *sf_pair = CONTAINER_OF(fetcher_cb, sf_pair_t, fetcher_cb);
    uv_mutex_lock(&sf_pair->mutex);
    if(sf_pair->closed){
        passthru_resp_free(passthru);
        ORIG_GO(&e, E_DEAD, "sf_pair is closed", unlock);
    }

    PROP_GO(&e, server_passthru_resp(sf_pair->server, passthru), unlock);

unlock:
    uv_mutex_unlock(&sf_pair->mutex);
    return e;
}

// part of the fetcher_cb_i
static derr_t fetcher_cb_select_succeeded(fetcher_cb_i *fetcher_cb){
    derr_t e = E_OK;

    sf_pair_t *sf_pair = CONTAINER_OF(fetcher_cb, sf_pair_t, fetcher_cb);
    uv_mutex_lock(&sf_pair->mutex);
    if(sf_pair->closed) ORIG_GO(&e, E_DEAD, "sf_pair is closed", unlock);

    PROP_GO(&e, server_select_succeeded(sf_pair->server), unlock);

unlock:
    uv_mutex_unlock(&sf_pair->mutex);
    return e;
}

// part of the fetcher_cb_i
static derr_t fetcher_cb_select_failed(fetcher_cb_i *fetcher_cb,
        const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    sf_pair_t *sf_pair = CONTAINER_OF(fetcher_cb, sf_pair_t, fetcher_cb);
    uv_mutex_lock(&sf_pair->mutex);
    if(sf_pair->closed) ORIG_GO(&e, E_DEAD, "sf_pair is closed", unlock);

    PROP_GO(&e, server_select_failed(sf_pair->server, st_resp), unlock);

unlock:
    uv_mutex_unlock(&sf_pair->mutex);
    return e;
}


derr_t sf_pair_new(
    sf_pair_t **out,
    sf_pair_cb_i *cb,
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
        .server_cb = {
            .dying = server_cb_dying,
            .login = server_cb_login,
            .passthru_req = server_cb_passthru_req,
            .select = server_cb_select,
        },
        .fetcher_cb = {
            .dying = fetcher_cb_dying,
            .login_ready = fetcher_cb_login_ready,
            .login_succeeded = fetcher_cb_login_succeeded,
            .login_failed = fetcher_cb_login_failed,
            .passthru_resp = fetcher_cb_passthru_resp,
            .select_succeeded = fetcher_cb_select_succeeded,
            .select_failed = fetcher_cb_select_failed,
        },
    };

    link_init(&sf_pair->link);

    int ret = uv_mutex_init(&sf_pair->mutex);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing mutex", fail_malloc);
    }

    // start with an owner ref, a server ref, and a fetcher ref
    PROP_GO(&e, refs_init(&sf_pair->refs, 3, sf_pair_finalize), fail_mutex);

    PROP_GO(&e,
        server_new(
            &sf_pair->server,
            &sf_pair->server_cb,
            p,
            ctx_srv,
            session),
        fail_refs);

    PROP_GO(&e,
        fetcher_new(
            &sf_pair->fetcher,
            &sf_pair->fetcher_cb,
            remote_host,
            remote_svc,
            p,
            ctx_cli),
        fail_server);

    *out = sf_pair;

    return e;

fail_server:
    server_cancel(sf_pair->server);
fail_refs:
    refs_free(&sf_pair->refs);
fail_mutex:
    uv_mutex_destroy(&sf_pair->mutex);
fail_malloc:
    free(sf_pair);
    return e;
}

void sf_pair_start(sf_pair_t *sf_pair){
    server_start(sf_pair->server);
    fetcher_start(sf_pair->fetcher);
}

void sf_pair_cancel(sf_pair_t *sf_pair){
    sf_pair->canceled = true;

    server_cancel(sf_pair->server);
    fetcher_cancel(sf_pair->fetcher);

    // lose a reference, since the conn_dying call won't be made
    ref_dn(&sf_pair->refs);
    ref_dn(&sf_pair->refs);

    sf_pair_release(sf_pair);
}

void sf_pair_release(sf_pair_t *sf_pair){
    ref_dn(&sf_pair->refs);
}
