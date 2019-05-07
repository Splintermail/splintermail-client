#ifndef FAKE_ENGINE_H
#define FAKE_ENGINE_H

#include <pthread.h>

#include <common.h>
#include <logger.h>
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


// the fake_session is meant to be useful in a variety of unit tests

typedef struct {
    // null engines are ignored during session hooks
    loop_t *loop;
    tlse_t *tlse;
} fake_pipeline_t;

typedef struct {
    pthread_mutex_t mutex;
    int refs;
    bool closed;
    // stuff for getters
    ssl_context_t *ssl_ctx;
    // engines
    fake_pipeline_t *pipeline;
    // engine_data elements
    loop_data_t loop_data;
    tlse_data_t tlse_data;
    // reference counts
    int loop_refs[LOOP_REF_MAXIMUM];
    int tlse_refs[TLSE_REF_MAXIMUM];
    int test_refs[FAKE_ENGINE_REF_MAXIMUM];
} fake_session_t;

derr_t fake_session_alloc(void **sptr, void *fake_pipeline,
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

bool fake_engine_run(fake_engine_t *fe,
                     event_passer_t pass_up, void *upstream,
                     void (*handle_read)(void*, event_t*),
                     void (*handle_write_done)(void*, event_t*),
                     bool (*quit_ready)(void*),
                     void *cb_data);

#endif // FAKE_ENGINE_H
