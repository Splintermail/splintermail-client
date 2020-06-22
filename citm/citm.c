#include <signal.h>
#include <string.h>

#include "citm.h"

static loop_t loop;
static tlse_t tlse;
static imape_t imape;
static citme_t citme;

// the global keypair
keypair_t *g_keypair;


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
        ) );

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


static derr_t citm(
        const char *local_host,
        const char *local_svc,
        const char *key,
        const char *cert,
        const char *dh,
        const char *keyfile,
        const char *remote_host,
        const char *remote_svc,
        const dstr_t *maildir_root){
    derr_t e = E_OK;

    // init OpenSSL
    PROP(&e, ssl_library_init() );

    // init ssl contexts
    ssl_context_t ctx_srv;
    PROP_GO(&e, ssl_context_new_server(&ctx_srv, cert, key, dh), cu_ssl_lib);

    ssl_context_t ctx_cli;
    PROP_GO(&e, ssl_context_new_client(&ctx_cli), cu_ctx_srv);

    // init global keypair
    PROP_GO(&e, keypair_load(&g_keypair, keyfile), cu_ctx_cli);

    imap_pipeline_t pipeline;

    string_builder_t root = SB(FD(maildir_root));

    PROP_GO(&e, citme_init(&citme, &root, &imape.engine), cu_keypair);

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
    LOG_INFO("listener ready\n");

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
cu_keypair:
    keypair_free(&g_keypair);
cu_ctx_cli:
    ssl_context_free(&ctx_cli);
cu_ctx_srv:
    ssl_context_free(&ctx_srv);
cu_ssl_lib:
    ssl_library_close();
    return e;
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


int main(int argc, char **argv){
    // ignore SIGPIPE, required to work with OpenSSL
    // see https://mta.openssl.org/pipermail/openssl-users/2017-May/005776.html
    // (but SIGPIPE doesnt exist in windows)
    signal(SIGINT, stop_loop_on_signal);
    signal(SIGTERM, stop_loop_on_signal);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    // defaults
    const char *d_l_host   = "127.0.0.1";
    const char *d_l_port   = "1993";
    const char *d_r_host   = "127.0.0.1";
    const char *d_r_port   = "993";
    const char *d_tls_key  = "../c/test/files/ssl/good-key.pem";
    const char *d_tls_cert = "../c/test/files/ssl/good-cert.pem";
    const char *d_tls_dh   = "../c/test/files/ssl/dh_4096.pem";
    const char *d_mail_key = "../c/test/files/key_tool/key_m.pem";
    DSTR_STATIC(d_maildirs, "/tmp/maildir_root");

    // options
    opt_spec_t o_l_host   = {'\0', "local-host",  true, OPT_RETURN_INIT};
    opt_spec_t o_l_port   = {'\0', "local-port",  true, OPT_RETURN_INIT};
    opt_spec_t o_r_host   = {'\0', "remote-host", true, OPT_RETURN_INIT};
    opt_spec_t o_r_port   = {'\0', "remote-port", true, OPT_RETURN_INIT};
    opt_spec_t o_tls_key  = {'\0', "tls-key",     true, OPT_RETURN_INIT};
    opt_spec_t o_tls_cert = {'\0', "tls-cert",    true, OPT_RETURN_INIT};
    opt_spec_t o_tls_dh   = {'\0', "tls-dh",      true, OPT_RETURN_INIT};
    opt_spec_t o_mail_key = {'\0', "mail-key",    true, OPT_RETURN_INIT};
    opt_spec_t o_maildirs = {'\0', "maildirs",    true, OPT_RETURN_INIT};

    opt_spec_t* spec[] = {
        &o_l_host,
        &o_l_port,
        &o_r_host,
        &o_r_port,
        &o_tls_key,
        &o_tls_cert,
        &o_tls_dh,
        &o_mail_key,
        &o_maildirs,
    };
    size_t speclen = sizeof(spec) / sizeof(*spec);
    int newargc;
    {
        derr_t e = opt_parse(argc, argv, spec, speclen, &newargc);
        CATCH(e, E_ANY){
            DUMP(e);
            DROP_VAR(&e);
            return 1;
        }
    }

    // resolve options
    const char *l_host = o_l_host.found ? o_l_host.val.data : d_l_host;
    const char *l_port = o_l_port.found ? o_l_port.val.data : d_l_port;
    const char *r_host = o_r_host.found ? o_r_host.val.data : d_r_host;
    const char *r_port = o_r_port.found ? o_r_port.val.data : d_r_port;
    const char *tls_key = o_tls_key.found ? o_tls_key.val.data : d_tls_key;
    const char *tls_cert = o_tls_cert.found ? o_tls_cert.val.data : d_tls_cert;
    const char *tls_dh = o_tls_dh.found ? o_tls_dh.val.data : d_tls_dh;
    const char *mail_key = o_mail_key.found ? o_mail_key.val.data : d_mail_key;
    const dstr_t *maildirs = o_maildirs.found ? &o_maildirs.val : &d_maildirs;

    // add logger
    logger_add_fileptr(LOG_LVL_INFO, stdout);
    auto_log_flush(true);

    derr_t e = citm(
        l_host,
        l_port,
        tls_key,
        tls_cert,
        tls_dh,
        mail_key,
        r_host,
        r_port,
        maildirs);
    CATCH(e, E_ANY){
        DUMP(e);
        DROP_VAR(&e);
        return 1;
    }

    return 0;
}
