#include "citm.h"

static void sf_pair_finalize(refs_t *refs){
    sf_pair_t *sf_pair = CONTAINER_OF(refs, sf_pair_t, refs);

    if(!sf_pair->canceled){
        sf_pair->mgr->dead(sf_pair->mgr, sf_pair);
    }

    refs_free(&sf_pair->refs);
    free(sf_pair);
}

void sf_pair_close(sf_pair_t *sf_pair, derr_t error){
    uv_mutex_lock(&sf_pair->mutex);
    bool do_close = !sf_pair->closing;
    sf_pair->closing = true;
    uv_mutex_unlock(&sf_pair->mutex);

    if(!do_close){
        // drop secondary errors
        DROP_VAR(&error);
        return;
    }

    server_close(sf_pair->server, E_OK);
    server_release(sf_pair->server);

    if(sf_pair->fetcher){
        fetcher_close(sf_pair->fetcher, E_OK);
        fetcher_release(sf_pair->fetcher);
    }

    sf_pair->mgr->dying(sf_pair->mgr, sf_pair, error);
    PASSED(error);
}

static void conn_dying(manager_i *mgr, void *caller, derr_t error){
    sf_pair_t *sf_pair = CONTAINER_OF(mgr, sf_pair_t, conn_mgr);
    (void)caller;

    sf_pair_close(sf_pair, error);

    // lose a reference now, since we hand out noop_mgr_dead
    ref_dn(&sf_pair->refs);
}


derr_t sf_pair_new(
    sf_pair_t **out,
    imap_pipeline_t *p,
    ssl_context_t *ctx_srv,
    manager_i *mgr,
    session_t **session
){
    derr_t e = E_OK;

    *out = NULL;
    *session = NULL;

    sf_pair_t *sf_pair = malloc(sizeof(*sf_pair));
    if(!sf_pair) ORIG(&e, E_NOMEM, "nomem");
    *sf_pair = (sf_pair_t){
        .mgr = mgr,
        .conn_mgr = {
            .dying = conn_dying,
            .dead = noop_mgr_dead,
        },
    };

    link_init(&sf_pair->link);

    int ret = uv_mutex_init(&sf_pair->mutex);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing mutex", fail_malloc);
    }

    PROP_GO(&e, refs_init(&sf_pair->refs, 1, sf_pair_finalize), fail_mutex);

    PROP_GO(&e, server_new(&sf_pair->server, p, ctx_srv, &sf_pair->conn_mgr,
                session), fail_refs);

    // ref ourselves for the server we now own
    ref_up(&sf_pair->refs);

    *out = sf_pair;

    return e;

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
}

void sf_pair_cancel(sf_pair_t *sf_pair){
    sf_pair->canceled = true;

    server_cancel(sf_pair->server);

    // lose a reference, since the conn_dying call won't be made
    ref_dn(&sf_pair->refs);

    sf_pair_release(sf_pair);
}

void sf_pair_release(sf_pair_t *sf_pair){
    ref_dn(&sf_pair->refs);
}
