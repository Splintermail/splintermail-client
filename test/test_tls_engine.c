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

#define NUM_THREADS 10
#define WRITES_PER_THREAD 1000
#define NUM_READ_EVENTS_PER_LOOP 5

// path to where the test files can be found
static const char* g_test_files;

unsigned int listen_port = 12346;
const char* port_str = "12346";

typedef struct {
    pthread_t thread;
    loop_t loop;
    tlse_t tlse;
    ssl_context_t ssl_ctx;
    void *downstream;
    derr_t error;
    pthread_mutex_t *mutex;
    pthread_cond_t *cond;
} test_context_t;

static void fake_session_destroyed(fake_session_t *s, derr_t error){
    // free the cb_reader_writer if there is one
    if(s->mgr_data){
        cb_reader_writer_t *cbrw = s->mgr_data;
        // catch an error from the cbrw
        MERGE_VAR(&error, &cbrw->error, "cb reader/writer");
        cb_reader_writer_free(cbrw);
    }
    if(error.type != E_NONE){
        loop_close(s->pipeline->loop, error);
        PASSED(error);
    }
}

/* In the first half of the test, we use reader-writer threads to connect to
   the loop.  In the second half of the test, we use the loop to connect to
   itself. */
static bool test_mode_self_connect = false;

static void fake_session_accepted(fake_session_t *s){
    if(!test_mode_self_connect) return;
    s->session_destroyed = fake_session_destroyed;
}

static void *loop_thread(void *arg){
    test_context_t *ctx = arg;
    derr_t e = E_OK;

    fake_pipeline_t pipeline = {
        .loop=&ctx->loop,
        .tlse=&ctx->tlse,
        .fake_session_accepted = fake_session_accepted,
    };

    // prepare the ssl context
    DSTR_VAR(cert, 4096);
    DSTR_VAR(key, 4096);
    DSTR_VAR(dh, 4096);
    PROP_GO(&e, FMT(&cert, "%x/ssl/good-cert.pem", FS(g_test_files)), done);
    PROP_GO(&e, FMT(&key, "%x/ssl/good-key.pem", FS(g_test_files)), done);
    PROP_GO(&e, FMT(&dh, "%x/ssl/dh_4096.pem", FS(g_test_files)), done);
    PROP_GO(&e, ssl_context_new_server(&ctx->ssl_ctx, cert.data, key.data,
                                    dh.data), done);

    PROP_GO(&e, tlse_init(&ctx->tlse, 5, 5,
                       fake_session_iface_tlse,
                       fake_session_get_tlse_data,
                       fake_session_get_ssl_ctx,
                       fake_session_get_upwards,
                       loop_pass_event, &ctx->loop,
                       fake_engine_pass_event, ctx->downstream), cu_ssl_ctx);

    PROP_GO(&e, loop_init(&ctx->loop, 5, 5,
                       &ctx->tlse, tlse_pass_event,
                       fake_session_iface_loop,
                       fake_session_get_loop_data,
                       fake_session_alloc_accept,
                       &pipeline,
                       "127.0.0.1", port_str), cu_tlse);

    IF_PROP(&e, tlse_add_to_loop(&ctx->tlse, &ctx->loop.uv_loop) ){
        // Loop can't run but can't close without running; shit's fucked
        LOG_ERROR("Failed to assemble pipeline, hard exiting\n");
        exit(13);
    }

    // create the listener
    uv_ptr_t uvp = {.type = LP_TYPE_LISTENER, .data={.ssl_ctx=&ctx->ssl_ctx}};
    PROP_GO(&e, loop_add_listener(&ctx->loop, "127.0.0.1", port_str, &uvp),
             loop_handle_error);

loop_handle_error:
    // the loop is only cleaned up while it is running.
    if(e.type != E_NONE){
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
cu_ssl_ctx:
    ssl_context_free(&ctx->ssl_ctx);
done:
    MERGE_VAR(&ctx->error, &e, "test_tls_engine:loop_thread");

    // signal to the main thread, in case we are exiting early
    pthread_mutex_lock(ctx->mutex);
    pthread_cond_signal(ctx->cond);
    pthread_mutex_unlock(ctx->mutex);

    return NULL;
}

// callbacks from fake_engine

typedef struct {
    size_t nwrites;
    size_t nEOF;
    test_context_t *test_ctx;
    derr_t error;
    cb_reader_writer_t cb_reader_writers[NUM_THREADS];
    ssl_context_t *ssl_ctx_client;
} session_cb_data_t;

static void launch_second_half_of_test(session_cb_data_t *cb_data,
                                       fake_pipeline_t *fp){
    // start the second half of the tests
    test_mode_self_connect = true;
    // make NUM_THREAD connections
    for(size_t i = 0; i < NUM_THREADS; i++){
        derr_t e = E_OK;
        // allocate a new connecting session
        void* session;
        PROP_GO(&e, fake_session_alloc_connect(&session, fp,
                    cb_data->ssl_ctx_client), fail);

        // attach the destroy hook
        fake_session_t *s = session;
        s->session_destroyed = fake_session_destroyed;

        // prepare a cb_reader_writer
        cb_reader_writer_t *cbrw = &cb_data->cb_reader_writers[i];
        event_t *ev_new = cb_reader_writer_init(cbrw, i, WRITES_PER_THREAD, s);
        if(!ev_new){
            ORIG_GO(&e, E_VALUE, "did not get event from cb_reader_writer\n", fail);
        }

        // attach the cb_reader_writer to the destroy hook
        s->mgr_data = cbrw;

        // pass the write
        tlse_pass_event(&cb_data->test_ctx->tlse, ev_new);
        cb_data->nwrites++;

        continue;

    fail:
        loop_close(&cb_data->test_ctx->loop, e);
        PASSED(e);
        break;
    }
}

static void handle_read(void *data, event_t *ev){
    session_cb_data_t *cb_data = data;
    if(ev->buffer.len == 0){
        // done with this session
        fake_session_close(ev->session, E_OK);
        // was that the last session?
        cb_data->nEOF++;
        if(cb_data->nEOF == NUM_THREADS){
            // reuse the fake_pipline
            fake_pipeline_t *fp = ((fake_session_t*)ev->session)->pipeline;
            launch_second_half_of_test(cb_data, fp);
        }else if(cb_data->nEOF == NUM_THREADS*2){
            // test is over
            loop_close(&cb_data->test_ctx->loop, E_OK);
        }
    }
    // is this session a cb_reader_writer session?
    else if(ev->session && ((fake_session_t*)ev->session)->mgr_data){
        cb_reader_writer_t *cbrw = ((fake_session_t*)ev->session)->mgr_data;
        event_t *ev_new = cb_reader_writer_read(cbrw, &ev->buffer);
        if(ev_new){
            // pass the write
            tlse_pass_event(&cb_data->test_ctx->tlse, ev_new);
            cb_data->nwrites++;
        }
    }
    // otherwise, echo back the message
    else{
        event_t *ev_new = malloc(sizeof(*ev_new));
        if(!ev_new){
            derr_t e = E_OK;
            TRACE_ORIG(&e, E_NOMEM, "no memory!");
            MERGE_VAR(&cb_data->error, &e, "malloc");
            return;
        }
        event_prep(ev_new, NULL);
        if(dstr_new_quiet(&ev_new->buffer, ev->buffer.len)){
            derr_t e = E_OK;
            TRACE_ORIG(&e, E_NOMEM, "no memory!");
            MERGE_VAR(&cb_data->error, &e, "malloc");
            free(ev_new);
            return;
        }
        dstr_copy(&ev->buffer, &ev_new->buffer);
        ev_new->session = ev->session;
        fake_session_ref_up_test(ev_new->session, FAKE_ENGINE_REF_WRITE);
        ev_new->ev_type = EV_WRITE;
        // pass the write
        tlse_pass_event(&cb_data->test_ctx->tlse, ev_new);
        cb_data->nwrites++;
    }
}

static void handle_write_done(void *data, event_t *ev){
    session_cb_data_t *cb_data = data;
    // downref session
    fake_session_ref_down_test(ev->session, FAKE_ENGINE_REF_WRITE);
    // free event
    dstr_free(&ev->buffer);
    free(ev);
    cb_data->nwrites--;
}

static bool quit_ready(void *data){
    session_cb_data_t *cb_data = data;
    return cb_data->nwrites == 0;
}

static derr_t test_tlse(void){
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

    // get the event queue ready
    fake_engine_t fake_engine;
    PROP_GO(&e, fake_engine_init(&fake_engine), cu_mutex);

    // start the loop thread
    pthread_mutex_lock(&mutex);
    unlock_mutex_on_error = true;
    test_context_t test_ctx = {
        .downstream = &fake_engine,
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

    session_cb_data_t cb_data = {
        .test_ctx = &test_ctx,
        .error = E_OK,
        .ssl_ctx_client = &ssl_ctx_client,
    };

    // catch error from fake_engine_run
    MERGE_CMD(&e, fake_engine_run(
            &fake_engine, tlse_pass_event, &test_ctx.tlse,
            handle_read, handle_write_done, quit_ready, &cb_data),
            "fake_engine_run");

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
