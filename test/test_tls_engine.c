/* Sandwich the TLS engine in between the loop engine and an echo engine so
   we can test it against our other SSL code.  A true unit test would be better
   but would require using the openssl binary and a fork/exec, which is not
   cross-platform. */

#include <libdstr/libdstr.h>
#include <libduv/libduv.h>
#include <libengine/libengine.h>
#include <libcrypto/libcrypto.h>

#include "test_utils.h"
#include "fake_engine.h"

#define NUM_THREADS 10
#define WRITES_PER_THREAD 1000
#define NUM_READ_EVENTS_PER_LOOP 5

// path to where the test files can be found
static const char* g_test_files;

static unsigned int listen_port = 12348;
static const char *host = "127.0.0.1";
static const char *port_str = "12348";

typedef struct {
    imap_pipeline_t pipeline;
    ssl_context_t *ssl_ctx;
    listener_spec_t lspec;
} test_lspec_t;
DEF_CONTAINER_OF(test_lspec_t, lspec, listener_spec_t)

static derr_t conn_recvd(listener_spec_t *lspec, session_t **session){
    derr_t e = E_OK;

    test_lspec_t *t = CONTAINER_OF(lspec, test_lspec_t, lspec);

    imap_session_alloc_args_t args = {
        &t->pipeline,
        NULL, // mgr (filled in by echo_session_mgr_new)
        t->ssl_ctx,
        NULL, // imap_control
        NULL, // imape_data's downstream
        NULL, // host
        NULL, // service
        (terminal_t){0},
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

    PROP_GO(&e, tlse_init(&ctx->tlse, 5, 5, &ctx->loop.engine,
                ctx->downstream), cu_ssl_ctx);

    PROP_GO(&e, loop_init(&ctx->loop, 5, 5, &ctx->tlse.engine), cu_tlse);

    IF_PROP(&e, tlse_add_to_loop(&ctx->tlse, &ctx->loop.uv_loop) ){
        // Loop can't run but can't close without running; shit's fucked
        LOG_ERROR("Failed to assemble pipeline, hard exiting\n");
        exit(13);
    }

    // create the listener
    test_lspec_t test_lspec = {
        .pipeline = {
            .loop = &ctx->loop,
            .tlse = &ctx->tlse,
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
    dmutex_lock(ctx->mutex);
    dcond_signal(ctx->cond);
    dmutex_unlock(ctx->mutex);

    // run the loop
    PROP_GO(&e, loop_run(&ctx->loop), cu_loop);

cu_loop:
    // other threads may call loop_free at a later time, so we don't free here
    // loop_free(&ctx->loop);
cu_tlse:
    tlse_free(&ctx->tlse);
cu_ssl_ctx:
    ssl_context_free(&ctx->ssl_ctx);
done:
    MERGE_VAR(&ctx->error, &e, "test_tls_engine:loop_thread");

    // signal to the main thread, in case we are exiting early
    dmutex_lock(ctx->mutex);
    dcond_signal(ctx->cond);
    dmutex_unlock(ctx->mutex);

    return NULL;
}

static derr_t test_tlse(void){
    derr_t e = E_OK;
    // prepare the client ssl context
    ssl_context_t ssl_ctx_client;
    PROP(&e, ssl_context_new_client(&ssl_ctx_client) );

    // get the conditional variable and mutex ready
    dcond_t cond;
    PROP_GO(&e, dcond_init(&cond), cu_ssl_ctx_client);
    dmutex_t mutex;
    bool unlock_mutex_on_error = false;
    PROP_GO(&e, dmutex_init(&mutex), cu_cond);

    // get the event queue ready
    fake_engine_t fake_engine;
    PROP_GO(&e, fake_engine_init(&fake_engine), cu_mutex);

    // start the loop thread
    dmutex_lock(&mutex);
    unlock_mutex_on_error = true;
    test_context_t test_ctx = {
        .downstream = &fake_engine.engine,
        .mutex = &mutex,
        .cond = &cond,
    };
    PROP_GO(&e,
        dthread_create(&test_ctx.thread, loop_thread, &test_ctx),
    cu_mutex);

    // wait for loop to be set up
    dcond_wait(&cond, &mutex);
    dmutex_unlock(&mutex);
    unlock_mutex_on_error = false;

    if(test_ctx.error.type != E_NONE){
        // if the test thread signaled us from its exit stage, skip to the end
        goto join_test_thread;
    }

    // start up a few threads
    reader_writer_context_t threads[NUM_THREADS];
    size_t threads_ready = 0;
    for(size_t i = 0; i < NUM_THREADS; i++){
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
        PROP_GO(&e,
            dthread_create(
                &threads[i].thread, reader_writer_thread, &threads[i]
            ),
        join_test_thread);
    }

    imap_session_alloc_args_t session_connect_args = {
        &(imap_pipeline_t){
            .loop=&test_ctx.loop,
            .tlse=&test_ctx.tlse,
        },
        NULL, // mgr (filled in by cb_reader_writer_init)
        &ssl_ctx_client,
        NULL, // imap_control
        NULL, // imape_data's downstream
        host,
        port_str,
        (terminal_t){0},
    };

    session_cb_data_t *cb_data;
    PROP_GO(&e, session_cb_data_new(NUM_THREADS, &cb_data), join_test_thread);
    cb_data->test_ctx = &test_ctx;
    cb_data->error = E_OK;
    cb_data->ssl_ctx_client = &ssl_ctx_client;
    cb_data->num_threads = NUM_THREADS;
    cb_data->writes_per_thread = WRITES_PER_THREAD;
    cb_data->session_connect_args = session_connect_args;

    // catch error from fake_engine_run
    MERGE_CMD(&e, fake_engine_run(&fake_engine, &test_ctx.tlse.engine,
                cb_data, &test_ctx.loop), "fake_engine_run");

    // join all the threads
    for(size_t i = 0; i < NUM_THREADS; i++){
        dthread_join(&threads[i].thread);
        // check for error
        MERGE_VAR(&e, &threads[i].error, "test thread");
    }

    session_cb_data_free(&cb_data);

join_test_thread:
    dthread_join(&test_ctx.thread);
    MERGE_VAR(&e, &test_ctx.error, "test context");
    // now that we know nobody will close the loop, we are safe to free it
    loop_free(&test_ctx.loop);

    // clean up the queue
    fake_engine_free(&fake_engine);
cu_mutex:
    if(unlock_mutex_on_error) dmutex_unlock(&mutex);
    dmutex_free(&mutex);

cu_cond:
    dcond_free(&cond);

cu_ssl_ctx_client:
    ssl_context_free(&ssl_ctx_client);
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, &g_test_files, LOG_LVL_INFO);

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    PROP_GO(&e, test_tlse(), test_fail);

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
