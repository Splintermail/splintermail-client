#ifndef LOOP_H
#define LOOP_H

#include <uv.h>

#include "common.h"
#include "engine.h"
#include "queue.h"

struct loop_t;
typedef struct loop_t loop_t;
struct loop_data_t;
typedef struct loop_data_t loop_data_t;
struct listener_spec_t;
typedef struct listener_spec_t listener_spec_t;
struct async_spec_t;
typedef struct async_spec_t async_spec_t;

/* A tagged union of pointers, mostly important for uv_walk, to determine which
   type of handler we are looking at */
enum uv_ptr_type_t {
    LP_TYPE_LOOP_DATA, // connection uv_socket_t holds pointer to loop_data_t
    LP_TYPE_LISTENER, // listener uv_socket holds pointer to listener_spec_t
};
union uv_ptr_data_t {
    loop_data_t *loop_data;
    listener_spec_t *lspec;
};
typedef struct {
    enum uv_ptr_type_t type;
    union uv_ptr_data_t data;
} uv_ptr_t;

// for a pool of pre-allocated write buffers
typedef struct write_wrapper_t {
    // self-pointer struct for enqueing this struct
    link_t link;
    // we need to know the loop for enqueuing
    loop_t *loop;
    // libuv write request handle
    uv_write_t uv_write;
    // a pointer to the event_t with the data
    event_t *ev;
    // libuv-style buffer (points into event_t)
    uv_buf_t uv_buf;
} write_wrapper_t;
DEF_CONTAINER_OF(write_wrapper_t, link, link_t);

void write_wrapper_prep(write_wrapper_t *wr_wrap, loop_t *loop);

/* Must be type-punnable to a char*, since that is what uv_read_cb receives.
   When we pass it to the downstream engine, we pass just the event_t, but we
   can recover the original struct because the dstr_t element of the event_t
   points back to the head of this struct. */
typedef struct {
    char buffer[4096];
    event_t event;
} read_wrapper_t;
DEF_CONTAINER_OF(read_wrapper_t, event, event_t);

// the socket_engine and event loop, one per pipeline
struct loop_t {
    engine_t engine;
    uv_loop_t uv_loop;
    uv_async_t loop_event_passer;
    uv_async_t loop_closer;
    uv_mutex_t mutex;
    // for pushing reads to the next engine
    queue_t read_events;
    // write reqs, for wrapping incoming write event_t's with libuv stuff
    queue_t write_wrappers;
    queue_t event_q;
    // neighboring engine
    engine_t *downstream;
    event_t quitmsg;
    bool quitting;
    // error passed by loop_close
    derr_t error;
};
DEF_CONTAINER_OF(loop_t, engine, engine_t);

// per-session data struct
struct loop_data_t {
    // stuff defined in prestart
    session_t *session;
    loop_t *loop;
    ref_fn_t ref_up;
    ref_fn_t ref_down;
    const char* host;
    const char* service;
    // a self pointer the uv_tcp_t
    uv_ptr_t uvp;
    queue_cb_t read_pause_qcb;
    // libuv socket
    uv_tcp_t *sock;
    bool connected;
    queue_t preconnected_writes;
    // a single error object is carried through multiple connection attempts
    derr_t connect_iii_error;
    // the only way to pass an error from no_bufs__pause_reading():
    derr_t pausing_error;
    /* during the unpause hook, a buffer is stored here for the next call to
       the allocator */
    event_t *event_for_allocator;
    // standard engine data items
    engine_data_state_t state;
    event_t start_ev;
    event_t close_ev;
    // for upwards connections
    uv_getaddrinfo_t gai_req;
    struct addrinfo hints;
    struct addrinfo *gai_result;
    struct addrinfo *gai_aiptr;
    uv_connect_t connect_req;
};
DEF_CONTAINER_OF(loop_data_t, read_pause_qcb, queue_cb_t);

// Per-listener specificiation.
struct listener_spec_t {
    // a tagged self-pointer, which will be filled out by loop_add_listener()
    uv_ptr_t uvp;
    // these fields should be prepared by the caller of loop_add_listener()
    const char *addr;
    const char *svc;
    // after receiving a connect, allocate a new downwards session
    derr_t (*conn_recvd)(listener_spec_t*, session_t**);
};

// Per-async specification for how to clean up the uv_async_t
struct async_spec_t {
    // the callback we will use to free the uv_async_t handle
    void (*close_cb)(async_spec_t*);
};

// the async close_cb for all asyncs
void async_handle_close_cb(uv_handle_t *handle);

// num_write_wrappers must match the downstream engine's num_write_events
derr_t loop_init(loop_t *loop, size_t num_read_events,
        size_t num_write_wrappers, engine_t *downstream);
void loop_free(loop_t *loop);

derr_t loop_run(loop_t *loop);

void loop_close(loop_t *loop, derr_t error);

derr_t loop_add_listener(loop_t *loop, listener_spec_t *lspec);

/* prestart() is for setting before any errors can happen and before any
   messages can be sent. */
void loop_data_prestart(loop_data_t *ld, loop_t *loop, session_t *session,
        const char *host, const char *service, ref_fn_t ref_up,
        ref_fn_t ref_down);

void loop_data_start(loop_data_t *ld);
/* Not thread safe, must be called exactly once per loop_data_t.  Thread safety
   should be handled at the session level.  Might be called before
   loop_data_onthread_start() is called. */
void loop_data_close(loop_data_t *ld);
/* Note that there is no loop_data_free().  This is intentional.  When a
   session is closed, it should call loop_data_close, which will trigger the
   libuv thread to close the libuv-related resources of the loop_data_t.  After
   that process is complete the loop_data_t has been completely cleaned up, and
   libuv will downref the session.  In the session's down_ref handler, no
   further cleanup steps are necessary in relation to the loop_data_t. */

enum loop_ref_reason_t {
    LOOP_REF_READ = 0,
    LOOP_REF_START_EVENT,
    LOOP_REF_CLOSE_EVENT,
    LOOP_REF_CONNECT_PROTECT,
    LOOP_REF_LIFETIME,
    LOOP_REF_MAXIMUM
};

dstr_t *loop_ref_reason_to_dstr(enum loop_ref_reason_t reason);

#endif // LOOP_H
