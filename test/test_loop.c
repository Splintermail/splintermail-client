#include <pthread.h>

#include <common.h>
#include <queue.h>
#include <networking.h>
#include <logger.h>
#include <loop.h>

#include "test_utils.h"

#define NUM_THREADS 10
#define WRITES_PER_THREAD 10000
#define NUM_READ_EVENTS_PER_LOOP 4

unsigned int listen_port = 12346;
const char* port_str = "12346";

typedef struct {
    derr_t error;
    size_t thread_id;
    pthread_t thread;
    pthread_mutex_t *mutex;
    pthread_cond_t *cond;
    size_t num_threads;
    size_t *threads_ready;
} reader_writer_context_t;

static void *reader_writer_thread(void *arg){
    reader_writer_context_t *ctx = arg;
    derr_t error;

    // generate all the buffers we are going to send
    LIST(dstr_t) out_bufs;
    PROP_GO( LIST_NEW(dstr_t, &out_bufs, WRITES_PER_THREAD), fail);
    for(size_t i = 0; i < WRITES_PER_THREAD; i++){
        dstr_t temp;
        // allocate the dstr in the list
        PROP_GO( dstr_new(&temp, 64), free_out_bufs);
        // write something into the buffer
        PROP_GO( FMT(&temp, "%x:%x\n", FU(ctx->thread_id), FU(i)), free_temp);
        // add it to the list
        PROP_GO( LIST_APPEND(dstr_t, &out_bufs, temp), free_temp);
        continue;
    free_temp:
        dstr_free(&temp);
        goto free_out_bufs;
    }

    // check if we are the last thread ready
    pthread_mutex_lock(ctx->mutex);
    (*ctx->threads_ready)++;
    // last thread signals the others
    if(*ctx->threads_ready == ctx->num_threads){
        pthread_cond_broadcast(ctx->cond);
    }
    // other threads wait for the last one
    else{
        while(*ctx->threads_ready < ctx->num_threads){
            pthread_cond_wait(ctx->cond, ctx->mutex);
        }
    }
    pthread_mutex_unlock(ctx->mutex);

    // open a connection
    connection_t conn;
    connection_new(&conn, "127.0.0.1", listen_port);
    // write all of the buffers
    for(size_t i = 0; i < out_bufs.len; i++){
        PROP_GO( connection_write(&conn, &out_bufs.data[i]), close_conn);
    }
    // read all of the buffers into a single place
    dstr_t recvd;
    PROP_GO( dstr_new(&recvd, 8192), close_conn);
    while( dstr_count(&recvd, &DSTR_LIT("\n")) < out_bufs.len){
        PROP_GO( connection_read(&conn, &recvd, NULL), free_recvd);
    }
    // now compare the buffers
    size_t compared = 0;
    for(size_t i = 0; i < out_bufs.len; i++){
        // cmp is the section of leftovers that
        dstr_t cmp = dstr_sub(&recvd, compared,
                              compared + out_bufs.data[i].len);
        if(dstr_cmp(&cmp, &out_bufs.data[i]) != 0)
            ORIG_GO(E_VALUE, "received bad response!", free_recvd);
        compared += out_bufs.data[i].len;
    }

    // done!

free_recvd:
    dstr_free(&recvd);
close_conn:
    connection_close(&conn);
free_out_bufs:
    for(size_t i = 0; i < out_bufs.len; i++){
        dstr_free(&out_bufs.data[i]);
    }
    LIST_FREE(dstr_t, &out_bufs);
fail:
    ctx->error = error;
    return NULL;
}

// a simple session, with a session interface for the loop to interact with
typedef struct session_t {
    pthread_mutex_t mutex;
    int refs;
    bool closed;
    loop_data_t loop_data;
}session_t;

static void session_ref_up(void *session){
    session_t *s = session;
    pthread_mutex_lock(&s->mutex);
    s->refs++;
    pthread_mutex_unlock(&s->mutex);
}

static void session_ref_down(void *session){
    session_t *s = session;
    pthread_mutex_lock(&s->mutex);
    int refs = --s->refs;
    pthread_mutex_unlock(&s->mutex);

    if(refs > 0) return;

    // free the session
    pthread_mutex_destroy(&s->mutex);
    free(s);
}

// to allocate new sessions (when loop.c only know about a single child struct)
// (the void** sets the session pointer, the void* argument is sess_alloc_data)
static derr_t session_alloc(void** sptr, void* data, loop_t *loop,
                            ssl_context_t* ssl_ctx){
    (void)data;
    (void)ssl_ctx;
    // allocate the struct
    session_t *s = malloc(sizeof(*s));
    if(!s) ORIG(E_NOMEM, "no mem");
    *s = (session_t){0};
    // prepare the refs
    pthread_mutex_init(&s->mutex, NULL);
    s->refs = 1;
    s->closed = false;
    // init the loop_data element
    loop_data_start(&s->loop_data, loop, s);
    *sptr = s;
    session_ref_down(s);
    return E_OK;
}

static void session_close(void *session, derr_t error){
    (void)error;
    session_t *s = session;
    pthread_mutex_lock(&s->mutex);
    bool do_close = !s->closed;
    s->closed = true;
    pthread_mutex_unlock(&s->mutex);

    if(!do_close) return;

    loop_data_close(&s->loop_data);
}

static void session_lock(void *session){
    session_t *s = session;
    pthread_mutex_lock(&s->mutex);
}

static void session_unlock(void *session){
    session_t *s = session;
    pthread_mutex_unlock(&s->mutex);
}

static bool session_is_invalid(void *session){
    session_t *s = session;
    return s->closed;
}

static bool session_is_complete(void *session){
    session_t *s = session;
    return s->closed;
}

static session_iface_t iface = {
    .ref_up = session_ref_up,
    .ref_down = session_ref_down,
    .close = session_close,
    .lock = session_lock,
    .unlock = session_unlock,
    .is_invalid = session_is_invalid,
    .is_complete = session_is_complete,
};

static void *session_get_loop_data(void *session){
    session_t *s = session;
    return (void*)&s->loop_data;
}

// an event passer that just passes directly into an event queue
static void queue_passer(void *engine, event_t *ev){
    queue_t *q = engine;
    queue_append(q, &ev->qe);
}

typedef struct {
    pthread_t thread;
    loop_t loop;
    void *downstream;
    session_deref_t get_loop_data;
    session_allocator_t session_alloc;
    derr_t error;
    void *session_alloc_data;
    pthread_mutex_t *mutex;
    pthread_cond_t *cond;
} loop_context_t;

static void *loop_thread(void *arg){
    loop_context_t *ctx = arg;
    derr_t error;
    size_t num_read_events = NUM_READ_EVENTS_PER_LOOP;
    /* we can have a lot of simultaneous writes, which is not realistic
       compared to the real behavior of a pipeline, so we won't worry about
       testing that behavior */
    size_t num_write_wrappers = NUM_THREADS * WRITES_PER_THREAD;
    PROP_GO( loop_init(&ctx->loop, num_read_events, num_write_wrappers,
                       ctx->downstream, queue_passer,
                       iface,
                       ctx->get_loop_data,
                       ctx->session_alloc,
                       ctx->session_alloc_data), done);

    // create the listener
    uv_ptr_t uvp = {.type = LP_TYPE_LISTENER};
    PROP_GO( loop_add_listener(&ctx->loop, "127.0.0.1", port_str, &uvp),
             cu_loop);

    // signal to the main thread
    pthread_mutex_lock(ctx->mutex);
    pthread_cond_signal(ctx->cond);
    pthread_mutex_unlock(ctx->mutex);

    // run the loop
    PROP_GO( loop_run(&ctx->loop), cu_loop);

cu_loop:
    loop_free(&ctx->loop);
done:
    ctx->error = error;
    return NULL;
}

static derr_t test_loop(void){
    derr_t error;
    // get the conditional variable and mutex ready
    pthread_cond_t cond;
    pthread_cond_init(&cond, NULL);
    pthread_mutex_t mutex;
    bool unlock_mutex_on_error = false;
    pthread_mutex_init(&mutex, NULL);

    // get the event queue ready
    queue_t event_q;
    PROP_GO( queue_init(&event_q), cu_mutex);

    // start the loop thread
    pthread_mutex_lock(&mutex);
    unlock_mutex_on_error = true;
    loop_context_t loop_ctx = {
        .downstream = &event_q,
        .get_loop_data = session_get_loop_data,
        .session_alloc = session_alloc,
        .session_alloc_data = NULL,
        .mutex = &mutex,
        .cond = &cond,
    };
    pthread_create(&loop_ctx.thread, NULL, loop_thread, &loop_ctx);

    // wait for loop to be set up
    pthread_cond_wait(&cond, &mutex);
    pthread_mutex_unlock(&mutex);
    unlock_mutex_on_error = false;

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
            .threads_ready = &threads_ready,
        };
        pthread_create(&threads[i].thread, NULL,
                       reader_writer_thread, &threads[i]);
    }

    // process incoming events from the loop
    event_t *ev;
    bool success = true;
    event_t *quit_ev = NULL;
    size_t nwrites = 0;
    size_t nEOF = 0;
    while(true){
        if(!(ev = queue_pop_first(&event_q, true))) break;
        switch(ev->ev_type){
            case EV_READ:
                //LOG_ERROR("got read\n");
                // check for an error
                if(ev->error){
                    success = false;
                }
                // check for EOF
                else if(ev->buffer.len == 0){
                    // done with this session
                    session_close(ev->session, E_OK);
                    // was that the last session?
                    if(++nEOF == NUM_THREADS){
                        loop_close(&loop_ctx.loop, E_OK);
                    }
                }
                // otherwise, echo back the message
                else{
                    event_t *ev_new = malloc(sizeof(*ev_new));
                    if(!ev_new){
                        LOG_ERROR("no memory!\n");
                        success = false;
                        goto pass_back;
                    }
                    event_prep(ev_new, NULL);
                    if(dstr_new(&ev_new->buffer, ev->buffer.len)){
                        LOG_ERROR("no memory!\n");
                        free(ev_new);
                        success = false;
                        goto pass_back;
                    }
                    dstr_copy(&ev->buffer, &ev_new->buffer);
                    ev_new->session = ev->session;
                    session_ref_up(ev_new->session);
                    ev_new->ev_type = EV_WRITE;
                    // pass the write
                    loop_pass_event(&loop_ctx.loop, ev_new);
                    nwrites++;
                }
            pass_back:
                // return buffer
                ev->ev_type = EV_READ_DONE;
                loop_pass_event(&loop_ctx.loop, ev);
                break;
            case EV_QUIT_DOWN:
                // check if we need to wait for write events to be returned
                if(nwrites == 0){
                    ev->ev_type = EV_QUIT_UP;
                    loop_pass_event(&loop_ctx.loop, ev);
                    goto done;
                }else{
                    quit_ev = ev;
                }
                break;
            case EV_WRITE_DONE:
                // check for error
                if(ev->error){
                    LOG_ERROR("write error detected\n");
                    success = false;
                }
                // downref session
                session_ref_down(ev->session);
                // free event
                dstr_free(&ev->buffer);
                free(ev);
                nwrites--;
                // check for quitting condition
                if(quit_ev && nwrites == 0){
                    quit_ev->ev_type = EV_QUIT_UP;
                    loop_pass_event(&loop_ctx.loop, quit_ev);
                    goto done;
                }
                break;
            // other events should not happen
            case EV_READ_DONE:
            case EV_WRITE:
            case EV_QUIT_UP:
            default:
                LOG_ERROR("unexpected event type from loop engine\n");
                success = false;
        }
    }
done:
    // join all the threads
    for(size_t i = 0; i < sizeof(threads) / sizeof(*threads); i++){
        pthread_join(threads[i].thread, NULL);
        // check for error
        if(threads[i].error != E_OK){
            LOG_ERROR("thread %s returned %s\n", FU(i),
                      FD(error_to_dstr(threads[i].error)));
            success = false;
        }
    }
    pthread_join(loop_ctx.thread, NULL);
    if(loop_ctx.error){
        LOG_ERROR("loop thread returned %s\n",
                  FD(error_to_dstr(loop_ctx.error)));
        success = false;
    }

    // clean up the queue
    queue_free(&event_q);
cu_mutex:
    if(unlock_mutex_on_error) pthread_mutex_unlock(&mutex);
    pthread_mutex_destroy(&mutex);

    if(error == E_OK && success == false)
        ORIG(E_VALUE, "failure detected, check log");
    return error;
}

int main(int argc, char** argv){
    derr_t error;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO( test_loop(), test_fail);

test_fail:
    if(error){
        LOG_ERROR("FAIL\n");
        return 1;
    }
    LOG_ERROR("PASS\n");
    return 0;
}
