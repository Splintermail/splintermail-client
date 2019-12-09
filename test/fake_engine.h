#ifndef FAKE_ENGINE_H
#define FAKE_ENGINE_H

#include <pthread.h>

#include <common.h>
#include <queue.h>
#include <engine.h>
#include <loop.h>
#include <tls_engine.h>
#include <imap_engine.h>
#include <imap_session.h>

/* reader-writer thread is an independent thread that just starts a connection,
   writes a bunch of shit, and tests that whatever was returned matches what
   was sent:

    RW Thread starts the connection
                      _______________________________________________
                     |                                               |
                     | imap_session, via imap_session_alloc_accpet() |
    ___________      |  _______________       _______________        |
   |           | --> | |               | --> |               |       |
   | RW Thread |     | | test pipeline |     |  fake engine  |       |
   |___________| <-- | |_______________| <-- | (just echoes) |       |
                     |                       |_______________|       |
                     |_______________________________________________|

*/
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

typedef struct {
    manager_i mgr;
    // cbrw or echo?
    bool is_cbrw;
} tagged_mgr_t;
DEF_CONTAINER_OF(tagged_mgr_t, mgr, manager_i);

typedef struct {
    tagged_mgr_t tmgr;
    imap_session_t s;
} echo_session_mgr_t;
DEF_CONTAINER_OF(echo_session_mgr_t, tmgr, tagged_mgr_t);

derr_t echo_session_mgr_new(echo_session_mgr_t **out,
        imap_session_alloc_args_t args);

/* cb_reader_writer is just like reader_writer except it connects via the loop.
   This tests the ability to make connections.

   There are two sessions, which are distinguishable by sess->mgr_data.
    ____________________________________________________
   |                                                    |
   | imap_session, via imap_session_alloc_connect()     |
   |         _______________       ___________________  |
   |    +-> |               | --> |                   | |    ______
   |    |   | test pipeline |     |  fake engine      | <-- |      |
   |  +-|-- |               | <-- |  (mgr_data!=NULL, | |   | CBRW |
   |  | |   |               |     |   trigger cbrw)   | --> |______|
   |__|_|___|_ _ _ _ _ _ _ _|_____|_ _ _ _ _ _ _ _ _ _|_|
    __|_|___|_ _ _ _ _ _ _ _|_____|_ _ _ _ _ _ _ _ _ _|_
   |  | |   |               |     |                   | |
   |  | +-- |               | <-- | (mgr_data==NULL,  | |
   |  |     |               |     |    just echoes)   | |
   |  +---> |_______________| --> |___________________| |
   |                                                    |
   |                                                    |
   | imap_session, via imap_session_alloc_accept()      |
   |____________________________________________________|

*/

typedef struct {
    derr_t error;
    size_t id;
    dstr_t out;
    dstr_t in;
    size_t nwrites; // total writes, not a count of writes-so-far
    size_t nrecvd;
    engine_t *fake_engine;
    imap_session_t s;
    tagged_mgr_t tmgr;
    bool dying;
} cb_reader_writer_t;
DEF_CONTAINER_OF(cb_reader_writer_t, tmgr, tagged_mgr_t);

// returns the first WRITE event, or NULL if there was an error
derr_t cb_reader_writer_init(cb_reader_writer_t *cbrw, size_t id,
        size_t nwrites, engine_t *fake_engine, imap_session_alloc_args_t args);
void cb_reader_writer_free(cb_reader_writer_t *cbrw);
// returns the next WRITE event, if there is one
// event_t *cb_reader_writer_read(cb_reader_writer_t *cbrw, dstr_t *buffer);

/* The fake engine expects to be the last engine in a pipeline.  It will make
   callbacks into the test whenever a READ or WRITE_DONE is passed to it. */
typedef struct {
    engine_t engine;
    queue_t event_q;
} fake_engine_t;
DEF_CONTAINER_OF(fake_engine_t, engine, engine_t);

derr_t fake_engine_init(fake_engine_t *fake_engine);
void fake_engine_free(fake_engine_t *fake_engine);

derr_t fake_engine_get_write_event(engine_t *engine, dstr_t *text,
        event_t **out);

// for launching the loop thread
typedef struct {
    pthread_t thread;
    loop_t loop;
    tlse_t tlse;
    imape_t imape;
    ssl_context_t ssl_ctx;
    engine_t *downstream;
    derr_t error;
    pthread_mutex_t *mutex;
    pthread_cond_t *cond;
} test_context_t;

// for tracking the second half of the test
typedef struct {
    size_t nwrites;
    size_t nEOF;
    test_context_t *test_ctx;
    derr_t error;
    // there won't be more than 128 threads... right?
    cb_reader_writer_t cb_reader_writers[128];
    ssl_context_t *ssl_ctx_client;
    size_t num_threads;
    size_t writes_per_thread;
    const char *host;
    const char *port;
    imap_session_alloc_args_t session_connect_args;
} session_cb_data_t;

derr_t fake_engine_run(fake_engine_t *fe, engine_t *upstream,
        session_cb_data_t *cb_data, loop_t *loop);

#endif // FAKE_ENGINE_H
