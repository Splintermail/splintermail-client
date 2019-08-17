#include <pthread.h>

#include <common.h>
#include <queue.h>
#include <networking.h>
#include <logger.h>
#include <loop.h>

#include "test_utils.h"
#include "fake_engine.h"

#define NUM_THREADS 10
#define WRITES_PER_THREAD 10000
#define NUM_READ_EVENTS_PER_LOOP 4

unsigned int listen_port = 12347;
const char* port_str = "12347";

typedef struct {
    pthread_t thread;
    loop_t loop;
    void *downstream;
    derr_t error;
    pthread_mutex_t *mutex;
    pthread_cond_t *cond;
} test_context_t;

static void fake_session_dn_destroyed(fake_session_t *s, derr_t error){
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

typedef struct {
    fake_pipeline_t pipeline;
    listener_spec_t lspec;
} test_lspec_t;
DEF_CONTAINER_OF(test_lspec_t, lspec, listener_spec_t);

static derr_t conn_recvd(listener_spec_t *lspec, session_t **session){
    derr_t e = E_OK;

    test_lspec_t *t = CONTAINER_OF(lspec, test_lspec_t, lspec);

    fake_session_t *s;
    PROP(&e, fake_session_alloc_accept(&s, &t->pipeline, NULL) );
    s->session_destroyed = fake_session_dn_destroyed;
    fake_session_start(s);

    *session = &s->session;

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
                       ctx->downstream, "127.0.0.1", port_str), done);

    // create the listener
    test_lspec_t test_lspec = {
        .pipeline = {
            .loop = &ctx->loop,
        },
        .lspec = {
            .addr = "127.0.0.1",
            .svc = port_str,
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

// callbacks from fake_engine

static void fake_session_up_destroyed(fake_session_t *s, derr_t error){
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

typedef struct {
    size_t nwrites;
    size_t nEOF;
    test_context_t *test_ctx;
    derr_t error;
    cb_reader_writer_t cb_reader_writers[NUM_THREADS];
} session_cb_data_t;

static void launch_second_half_of_test(session_cb_data_t *cb_data,
                                       fake_pipeline_t *fp){
    // make NUM_THREAD connections
    for(size_t i = 0; i < NUM_THREADS; i++){
        derr_t e = E_OK;
        // allocate a new connecting session
        fake_session_t* s;
        PROP_GO(&e, fake_session_alloc_connect(&s, fp, NULL), fail);

        s->session_destroyed = fake_session_up_destroyed;
        // we have to start the session before we can start the cbrw
        fake_session_ref_up_test(&s->session, FAKE_ENGINE_REF_CBRW_PROTECT);

        // start the session
        fake_session_start(s);

        // prepare a cb_reader_writer
        cb_reader_writer_t *cbrw = &cb_data->cb_reader_writers[i];
        event_t *ev_new = cb_reader_writer_init(cbrw, i, WRITES_PER_THREAD, s);
        if(!ev_new){
            ORIG_GO(&e, E_VALUE, "did not get event from cb_reader_writer", fail);
        }

        // attach the cb_reader_writer to the destroy hook
        s->mgr_data = cbrw;

        // pass the write
        fp->loop->engine.pass_event(&fp->loop->engine, ev_new);
        cb_data->nwrites++;

        fake_session_ref_down_test(&s->session, FAKE_ENGINE_REF_CBRW_PROTECT);

        continue;

    fail:
        fake_session_ref_down_test(&s->session, FAKE_ENGINE_REF_CBRW_PROTECT);
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
            fake_pipeline_t *fp = ((fake_session_t*)ev->session)->pipeline;
            fp->loop->engine.pass_event(&fp->loop->engine, ev_new);
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
        event_prep(ev_new);
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
        fake_pipeline_t *fp = ((fake_session_t*)ev->session)->pipeline;
        fp->loop->engine.pass_event(&fp->loop->engine, ev_new);
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
        };
        pthread_create(&threads[i].thread, NULL,
                       reader_writer_thread, &threads[i]);
    }

    session_cb_data_t cb_data = {.test_ctx = &test_ctx, .error = E_OK};

    // catch error from fake_engine_run
    MERGE_CMD(&e, fake_engine_run(
            &fake_engine, &test_ctx.loop.engine,
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
