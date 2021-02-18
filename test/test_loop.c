#include <pthread.h>

#include <libdstr/libdstr.h>
#include <libuvthread/libuvthread.h>
#include <libengine/libengine.h>
#include <libcrypto/libcrypto.h>

#include "test_utils.h"
#include "fake_engine.h"

#define NUM_THREADS 10
#define WRITES_PER_THREAD 10000
#define NUM_READ_EVENTS_PER_LOOP 4

const char *host = "127.0.0.1";
const char *port = "12347";
unsigned int listen_port = 12347;

typedef struct {
    imap_pipeline_t pipeline;
    listener_spec_t lspec;
} test_lspec_t;
DEF_CONTAINER_OF(test_lspec_t, lspec, listener_spec_t);

static derr_t conn_recvd(listener_spec_t *lspec, session_t **session){
    derr_t e = E_OK;

    test_lspec_t *t = CONTAINER_OF(lspec, test_lspec_t, lspec);

    imap_session_alloc_args_t args = {
        &t->pipeline,
        NULL, // mgr (filled in by echo_session_mgr_new)
        NULL, // ssl_ctx;
        NULL, // imap_control
        NULL, // imape_data's downstream
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
    size_t num_read_events = NUM_READ_EVENTS_PER_LOOP;

    /* we can have a lot of simultaneous writes, which is not realistic
       compared to the real behavior of a pipeline, so we won't worry about
       testing that behavior */
    size_t num_write_wrappers = NUM_THREADS * WRITES_PER_THREAD;
    PROP_GO(&e, loop_init(&ctx->loop, num_read_events, num_write_wrappers,
                       ctx->downstream), done);

    // create the listener
    test_lspec_t test_lspec = {
        .pipeline = {
            .loop = &ctx->loop,
        },
        .lspec = {
            .addr = "127.0.0.1",
            .svc = port,
            .conn_recvd = conn_recvd,
        },
    };
    PROP_GO(&e, loop_add_listener(&ctx->loop, &test_lspec.lspec), cu_loop);

    // signal to the main thread
    pthread_mutex_lock(ctx->mutex);
    pthread_cond_signal(ctx->cond);
    pthread_mutex_unlock(ctx->mutex);

    // run the loop
    PROP_GO(&e, loop_run(&ctx->loop), cu_loop);

cu_loop:
    // other threads may call loop_free at a later time, so we don't free here
    // loop_free(&ctx->loop);
done:
    MERGE_VAR(&ctx->error, &e, "test_loop:loop_thread");

    // signal to the main thread, in case we are exiting early
    pthread_mutex_lock(ctx->mutex);
    pthread_cond_signal(ctx->cond);
    pthread_mutex_unlock(ctx->mutex);

    return NULL;
}

static derr_t test_loop(void){
    derr_t e = E_OK;
    // get the conditional variable and mutex ready
    pthread_cond_t cond;
    pthread_cond_init(&cond, NULL);
    pthread_mutex_t mutex;
    bool unlock_mutex_on_error = false;
    pthread_mutex_init(&mutex, NULL);

    // get the event queue ready
    fake_engine_t fake_engine;
    PROP_GO(&e, fake_engine_init(&fake_engine), cu_mutex);

    // start the loop thread
    pthread_mutex_lock(&mutex);
    unlock_mutex_on_error = true;
    test_context_t test_ctx = {
        .downstream = &fake_engine.engine,
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
        };
        pthread_create(&threads[i].thread, NULL,
                       reader_writer_thread, &threads[i]);
    }

    imap_session_alloc_args_t session_connect_args = {
        &(imap_pipeline_t){
            .loop=&test_ctx.loop,
        },
        NULL, // mgr (filled in by cb_reader_writer_init)
        NULL, // ssl_ctx;
        NULL, // imap_control
        NULL, // imape_data's downstream
        host,
        port,
        (terminal_t){},
    };

    session_cb_data_t cb_data = {
        .test_ctx = &test_ctx,
        .error = E_OK,
        .num_threads = NUM_THREADS,
        .writes_per_thread = WRITES_PER_THREAD,
        .session_connect_args = session_connect_args,
    };

    // catch error from fake_engine_run
    MERGE_CMD(&e, fake_engine_run(&fake_engine, &test_ctx.loop.engine,
                &cb_data, &test_ctx.loop), "fake_engine_run");

    // join all the threads
    for(size_t i = 0; i < sizeof(threads) / sizeof(*threads); i++){
        pthread_join(threads[i].thread, NULL);
        // check for error
        MERGE_VAR(&e, &threads[i].error, "test thread");
    }
join_test_thread:
    pthread_join(test_ctx.thread, NULL);
    MERGE_VAR(&e, &test_ctx.error, "test context");
    // now that we know nobody will close the loop, we are safe to free it
    loop_free(&test_ctx.loop);

    // clean up the queue
    fake_engine_free(&fake_engine);
cu_mutex:
    if(unlock_mutex_on_error) pthread_mutex_unlock(&mutex);
    pthread_mutex_destroy(&mutex);

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_loop(), test_fail);

test_fail:
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
        LOG_ERROR("FAIL\n");
        return 1;
    }
    LOG_ERROR("PASS\n");
    return 0;
}
