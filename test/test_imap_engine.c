/* Sandwich the TLS engine in between the loop engine and an echo engine so
   we can test it against our other SSL code.  A true unit test would be better
   but would require using the openssl binary and a fork/exec, which is not
   cross-platform. */

#include <pthread.h>

#include <common.h>
#include <queue.h>
#include <networking.h>
#include <logger.h>
#include <loop.h>
#include <tls_engine.h>

#include "test_utils.h"
#include "fake_engine.h"
#include "fake_imap_logic.h"

#define NUM_THREADS 10
#define WRITES_PER_THREAD 1000
#define NUM_READ_EVENTS_PER_LOOP 5

// path to where the test files can be found
static const char* g_test_files;

unsigned int listen_port = 12348;
const char* port_str = "12348";

typedef struct {
    imap_pipeline_t pipeline;
    ssl_context_t *ssl_ctx;
    listener_spec_t lspec;
} test_lspec_t;
DEF_CONTAINER_OF(test_lspec_t, lspec, listener_spec_t);

static derr_t conn_recvd(listener_spec_t *lspec, session_t **session){
    derr_t e = E_OK;

    test_lspec_t *t = CONTAINER_OF(lspec, test_lspec_t, lspec);

    imap_session_alloc_args_t args = {
        &t->pipeline,
        NULL, // mgr (filled in by echo_session_mgr_new)
        t->ssl_ctx,
        fake_imap_logic_init, // logic_alloc
        NULL, // alloc_data
        NULL, // host
        NULL, // service
        (terminal_t){},
    };

    echo_session_mgr_t *esm;
    PROP(&e, echo_session_mgr_new(&esm, args) );

    imap_session_start(&esm->s);

    *session = &esm->s.session;

    return e;
}

static void *loop_thread(void *arg){
    test_context_t *ctx = arg;
    derr_t e = E_OK;

    // prepare the ssl context
    DSTR_VAR(cert, 4096);
    DSTR_VAR(key, 4096);
    DSTR_VAR(dh, 4096);
    PROP_GO(&e, FMT(&cert, "%x/ssl/good-cert.pem", FS(g_test_files)), done);
    PROP_GO(&e, FMT(&key, "%x/ssl/good-key.pem", FS(g_test_files)), done);
    PROP_GO(&e, FMT(&dh, "%x/ssl/dh_4096.pem", FS(g_test_files)), done);
    PROP_GO(&e, ssl_context_new_server(&ctx->ssl_ctx, cert.data, key.data,
                                    dh.data), done);

    PROP_GO(&e, imape_init(&ctx->imape, 5, &ctx->tlse.engine, 5, &ctx->loop),
            cu_ssl_ctx);

    PROP_GO(&e, tlse_init(&ctx->tlse, 5, 5, &ctx->loop.engine,
                &ctx->imape.engine), cu_imape);

    PROP_GO(&e, loop_init(&ctx->loop, 5, 5, &ctx->tlse.engine), cu_tlse);

    IF_PROP(&e, tlse_add_to_loop(&ctx->tlse, &ctx->loop.uv_loop) ){
        // Loop can't run but can't close without running; shit's fucked
        LOG_ERROR("Failed to assemble pipeline, hard exiting\n");
        exit(13);
    }

    IF_PROP(&e, imape_add_to_loop(&ctx->imape, &ctx->loop.uv_loop) ){
        // Loop can't run but can't close without running; shit's fucked
        LOG_ERROR("Failed to assemble pipeline, hard exiting\n");
        exit(13);
    }

    // create the listener
    test_lspec_t test_lspec = {
        .pipeline = {
            .loop = &ctx->loop,
            .tlse = &ctx->tlse,
            .imape = &ctx->imape,
        },
        .ssl_ctx=&ctx->ssl_ctx,
        .lspec = {
            .addr = "127.0.0.1",
            .svc = port_str,
            .conn_recvd = conn_recvd,
        },
    };
    PROP_GO(&e, loop_add_listener(&ctx->loop, &test_lspec.lspec),
             loop_handle_error);

loop_handle_error:
    // the loop is only cleaned up while it is running.
    if(is_error(e)){
        loop_close(&ctx->loop, SPLIT(e));
    }

    // signal to the main thread
    pthread_mutex_lock(ctx->mutex);
    pthread_cond_signal(ctx->cond);
    pthread_mutex_unlock(ctx->mutex);

    // run the loop
    PROP_GO(&e, loop_run(&ctx->loop), cu_loop);

cu_loop:
    // other threads may call loop_free at a later time, so we don't free here
    // loop_free(&ctx->loop);
cu_tlse:
    tlse_free(&ctx->tlse);
cu_imape:
    imape_free(&ctx->imape);
cu_ssl_ctx:
    ssl_context_free(&ctx->ssl_ctx);
done:
    MERGE_VAR(&ctx->error, &e, "test_imap_engine:loop_thread");

    // signal to the main thread, in case we are exiting early
    pthread_mutex_lock(ctx->mutex);
    pthread_cond_signal(ctx->cond);
    pthread_mutex_unlock(ctx->mutex);

    return NULL;
}

static derr_t test_imape(void){
    derr_t e = E_OK;
    // prepare the client ssl context
    ssl_context_t ssl_ctx_client;
    PROP(&e, ssl_context_new_client(&ssl_ctx_client) );

    // get the conditional variable and mutex ready
    pthread_cond_t cond;
    pthread_cond_init(&cond, NULL);
    pthread_mutex_t mutex;
    bool unlock_mutex_on_error = false;
    if(pthread_mutex_init(&mutex, NULL)){
        perror("mutex_init");
        ORIG_GO(&e, E_NOMEM, "failed to allocate mutex", cu_ssl_ctx_client);
    }

    // start the loop thread
    pthread_mutex_lock(&mutex);
    unlock_mutex_on_error = true;
    test_context_t test_ctx = {
        .mutex = &mutex,
        .cond = &cond,
    };
    pthread_create(&test_ctx.thread, NULL, loop_thread, &test_ctx);

    // wait for loop to be set up
    pthread_cond_wait(&cond, &mutex);
    pthread_mutex_unlock(&mutex);
    unlock_mutex_on_error = false;

    if(test_ctx.error.type != E_NONE){
        // if the test thread signaled us from its exit stage, skip to the end
        goto join_test_thread;
    }

    // start up a few threads
    reader_writer_context_t threads[NUM_THREADS];
    size_t threads_ready = 0;
    for(size_t i = 0; i < sizeof(threads) / sizeof(*threads); i++){
        threads[i] = (reader_writer_context_t){
            .error = E_OK,
            .thread_id = i,
            .mutex = &mutex,
            .cond = &cond,
            .num_threads = NUM_THREADS,
            .writes_per_thread = WRITES_PER_THREAD,
            .listen_port = listen_port,
            .threads_ready = &threads_ready,
            .use_tls = true,
        };
        pthread_create(&threads[i].thread, NULL,
                       reader_writer_thread, &threads[i]);
    }

    // join all the threads
    for(size_t i = 0; i < sizeof(threads) / sizeof(*threads); i++){
        pthread_join(threads[i].thread, NULL);
        // check for error
        MERGE_VAR(&e, &threads[i].error, "test thread");
    }

    // done with the loop
    loop_close(&test_ctx.loop, E_OK);

join_test_thread:
    pthread_join(test_ctx.thread, NULL);
    MERGE_VAR(&e, &test_ctx.error, "test context");
    // now that we know nobody will close the loop, we are safe to free it
    loop_free(&test_ctx.loop);

    if(unlock_mutex_on_error) pthread_mutex_unlock(&mutex);
    pthread_mutex_destroy(&mutex);

cu_ssl_ctx_client:
    ssl_context_free(&ssl_ctx_client);
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;

    // ignore SIGPIPE, required to work with OpenSSL
    // see https://mta.openssl.org/pipermail/openssl-users/2017-May/005776.html
    // (but SIGPIPE doesnt exist in windows)
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, &g_test_files, LOG_LVL_INFO);

    PROP_GO(&e, test_imape(), test_fail);

test_fail:
    if(e.type){
        DUMP(e);
        DROP_VAR(&e);
        LOG_ERROR("FAIL\n");
        return 1;
    }
    LOG_ERROR("PASS\n");
    return 0;
}
