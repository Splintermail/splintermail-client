#include <signal.h>
#include <string.h>

#include "libcitm.h"

static loop_t loop;
static tlse_t tlse;
static imape_t imape;
static citme_t citme;


typedef struct {
    const char *remote_host;
    const char *remote_svc;
    imap_pipeline_t *pipeline;
    citme_t *citme;
    ssl_context_t *ctx_srv;
    ssl_context_t *ctx_cli;
    listener_spec_t lspec;
} citm_lspec_t;
DEF_CONTAINER_OF(citm_lspec_t, lspec, listener_spec_t);


static derr_t conn_recvd(listener_spec_t *lspec, session_t **session){
    derr_t e = E_OK;

    citm_lspec_t *l = CONTAINER_OF(lspec, citm_lspec_t, lspec);

    sf_pair_t *sf_pair;
    PROP(&e,
        sf_pair_new(
            &sf_pair,
            &l->citme->sf_pair_cb,
            &l->citme->engine,
            l->remote_host,
            l->remote_svc,
            l->pipeline,
            l->ctx_srv,
            l->ctx_cli,
            session
        )
    );

    // append managed to the server_mgr's list
    citme_add_sf_pair(l->citme, sf_pair);

    // now it is safe to start the server
    sf_pair_start(sf_pair);

    return e;
}



static void free_pipeline(imap_pipeline_t *pipeline){
    imape_free(pipeline->imape);
    tlse_free(pipeline->tlse);
    loop_free(pipeline->loop);
}


static derr_t build_pipeline(imap_pipeline_t *pipeline, citme_t *citme){
    derr_t e = E_OK;

    // set UV_THREADPOOL_SIZE
    unsigned int nworkers = 2;
    PROP(&e, set_uv_threadpool_size(nworkers + 3, nworkers + 7) );

    // initialize loop
    PROP(&e, loop_init(&loop, 5, 5, &tlse.engine) );

    // intialize TLS engine
    PROP_GO(&e, tlse_init(&tlse, 5, 5, &loop.engine, &imape.engine), fail);
    PROP_GO(&e, tlse_add_to_loop(&tlse, &loop.uv_loop), fail);

    // initialize IMAP engine
    PROP_GO(&e, imape_init(&imape, 5, &tlse.engine, &citme->engine), fail);
    PROP_GO(&e, imape_add_to_loop(&imape, &loop.uv_loop), fail);

    PROP_GO(&e, citme_add_to_loop(citme, &loop.uv_loop), fail);

    *pipeline = (imap_pipeline_t){
        .loop=&loop,
        .tlse=&tlse,
        .imape=&imape,
    };

    return e;

fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("fatal error: failed to construct pipeline\n");
    exit(1);
}


static bool hard_exit = false;
static void stop_loop_on_signal(int signum){
    (void) signum;
    LOG_ERROR("caught signal\n");
    if(hard_exit) exit(1);
    hard_exit = true;
    // launch an asynchronous loop abort
    loop_close(&loop, E_OK);
}


derr_t citm(
    const char *local_host,
    const char *local_svc,
    const char *key,
    const char *cert,
    const char *dh,
    const char *remote_host,
    const char *remote_svc,
    const string_builder_t *maildir_root,
    bool indicate_ready
){
    derr_t e = E_OK;

    // init ssl contexts
    ssl_context_t ctx_srv;
    PROP(&e, ssl_context_new_server(&ctx_srv, cert, key, dh) );

    ssl_context_t ctx_cli;
    PROP_GO(&e, ssl_context_new_client(&ctx_cli), cu_ctx_srv);

    imap_pipeline_t pipeline;

    PROP_GO(&e, citme_init(&citme, maildir_root, &imape.engine), cu_ctx_cli);

    PROP_GO(&e, build_pipeline(&pipeline, &citme), cu_citme);

    /* After building the pipeline, we must run the pipeline if we want to
       cleanup nicely.  That means that we can't follow the normal cleanup
       pattern, and instead we must initialize all of our variables to zero
       (that is, if we had any variables right here) */

    // add the lspec to the loop
    citm_lspec_t citm_lspec = {
        .remote_host = remote_host,
        .remote_svc = remote_svc,
        .pipeline = &pipeline,
        .citme = &citme,
        .ctx_srv = &ctx_srv,
        .ctx_cli = &ctx_cli,
        .lspec = {
            .addr = local_host,
            .svc = local_svc,
            .conn_recvd = conn_recvd,
        },
    };
    PROP_GO(&e, loop_add_listener(&loop, &citm_lspec.lspec), fail);

    // install signal handlers before indicating we are launching the loop
    signal(SIGINT, stop_loop_on_signal);
    signal(SIGTERM, stop_loop_on_signal);

    if(indicate_ready){
        LOG_INFO("listener ready\n");
    }else{
        // always indicate on DEBUG-level logs
        LOG_DEBUG("listener ready\n");
    }

fail:
    if(is_error(e)){
        loop_close(&loop, e);
        // The loop will pass us this error back after loop_run.
        PASSED(e);
    }

    // run the loop
    PROP_GO(&e, loop_run(&loop), cu);

cu:
    free_pipeline(&pipeline);
cu_citme:
    citme_free(&citme);
cu_ctx_cli:
    ssl_context_free(&ctx_cli);
cu_ctx_srv:
    ssl_context_free(&ctx_srv);
    return e;
}
