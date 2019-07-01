#ifndef FAKE_ENGINE_H
#define FAKE_ENGINE_H

#include <pthread.h>

#include <common.h>
#include <queue.h>
#include <engine.h>
#include <loop.h>
#include <tls_engine.h>

enum fake_engine_ref_reason_t {
    FAKE_ENGINE_REF_READ,
    FAKE_ENGINE_REF_WRITE,
    FAKE_ENGINE_REF_START_PROTECT,
    FAKE_ENGINE_REF_CLOSE_PROTECT,
    FAKE_ENGINE_REF_MAXIMUM
};

struct fake_session_t;
typedef struct fake_session_t fake_session_t;

/* reader-writer thread is an independent thread that just starts a connection,
   writes a bunch of shit, and tests that whatever was returned matches what
   was sent */
typedef struct {
    derr_t error;
    size_t thread_id;
    pthread_t thread;
    pthread_mutex_t *mutex;
    pthread_cond_t *cond;
    size_t num_threads;
    size_t writes_per_thread;
    size_t *threads_ready;
    unsigned int listen_port;
    bool use_tls;
} reader_writer_context_t;

// a pthread function, always returns NULL
void *reader_writer_thread(void *reader_writer_context);

/* cb_reader_writer is just like reader_writer except it connects via the loop
   to the listener in the loop. */

typedef struct {
    derr_t error;
    size_t id;
    dstr_t out;
    dstr_t in;
    size_t nwrites; // total writes, not a count of writes-so-far
    size_t nrecvd;
    fake_session_t *session;
    event_passer_t passer;
    void *passer_engine;
} cb_reader_writer_t;

// returns the first WRITE event, or NULL if there was an error
event_t *cb_reader_writer_init(cb_reader_writer_t *cbrw, size_t id,
                             size_t nwrites, fake_session_t *s);
void cb_reader_writer_free(cb_reader_writer_t *cbrw);
// returns the next WRITE event, if there is one
event_t *cb_reader_writer_read(cb_reader_writer_t *cbrw, dstr_t *buffer);

// the fake_session is meant to be useful in a variety of unit tests

typedef struct {
    // null engines are ignored during session hooks
    loop_t *loop;
    tlse_t *tlse;
    // session manager hook, returns cb_data for future calls
    void (*fake_session_accepted)(fake_session_t*);
} fake_pipeline_t;

struct fake_session_t {
    pthread_mutex_t mutex;
    int refs;
    bool closed;
    size_t id;
    derr_t error;
    // stuff for getters
    ssl_context_t *ssl_ctx;
    bool upwards;
    // engines
    fake_pipeline_t *pipeline;
    // engine_data elements
    loop_data_t loop_data;
    tlse_data_t tlse_data;
    // per-reason-per-engine reference counts
    int loop_refs[LOOP_REF_MAXIMUM];
    int tlse_refs[TLSE_REF_MAXIMUM];
    int test_refs[FAKE_ENGINE_REF_MAXIMUM];
    // session manager hook
    void *mgr_data;
    void (*session_destroyed)(fake_session_t*, derr_t);
};

derr_t fake_session_alloc_accept(void **sptr, void *fake_pipeline,
                                 ssl_context_t* ssl_ctx);

derr_t fake_session_alloc_connect(void **sptr, void *fake_pipeline,
                                  ssl_context_t* ssl_ctx);

// only for use on loop thread
void fake_session_ref_up_loop(void *session, int reason);
void fake_session_ref_down_loop(void *session, int reason);

// only for use on tlse thread
void fake_session_ref_up_tlse(void *session, int reason);
void fake_session_ref_down_tlse(void *session, int reason);

// only for use on test thread (fake engine and callbacks)
void fake_session_ref_up_test(void *session, int reason);
void fake_session_ref_down_test(void *session, int reason);

void fake_session_close(void *session, derr_t error);

extern session_iface_t fake_session_iface_loop;
extern session_iface_t fake_session_iface_tlse;

loop_data_t *fake_session_get_loop_data(void *session);
tlse_data_t *fake_session_get_tlse_data(void *session);
ssl_context_t *fake_session_get_ssl_ctx(void *session);
bool fake_session_get_upwards(void *session);


/* The fake engine expects to be the last engine in a pipeline.  It will make
   callbacks into the test whenever a READ or WRITE_DONE is passed to it. */
typedef struct {
    queue_t event_q;
} fake_engine_t;

derr_t fake_engine_init(fake_engine_t *fake_engine);
void fake_engine_free(fake_engine_t *fake_engine);
void fake_engine_pass_event(void *engine, event_t *ev);

derr_t fake_engine_run(fake_engine_t *fe, event_passer_t pass_up,
        void *upstream, void (*handle_read)(void*, event_t*),
        void (*handle_write_done)(void*, event_t*), bool (*quit_ready)(void*),
        void *cb_data);

#endif // FAKE_ENGINE_H