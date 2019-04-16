#ifndef LOOP_H
#define LOOP_H

#include <uv.h>

#include "common.h"
#include "engine.h"
#include "queue.h"
#include "networking.h"

struct loop_t;
typedef struct loop_t loop_t;
struct loop_data_t;
typedef struct loop_data_t loop_data_t;

/* A tagged union of pointers, mostly important for uv_walk, to determine which
   type of handler we are looking at */
enum uv_ptr_type_t {
    LP_TYPE_LOOP_DATA, // connection uv_socket_t holds pointer to loop_data_t
    LP_TYPE_LISTENER, // listener uv_socket holds pointer to ssl_context_t
};
struct loop_data_t;
union uv_ptr_data_t {
    struct loop_data_t *loop_data;
    // for listener sockets, we only need a single pointer:
    struct ssl_context_t *ssl_ctx;
};
typedef struct {
    enum uv_ptr_type_t type;
    union uv_ptr_data_t data;
} uv_ptr_t;

// for a pool of pre-allocated write buffers
typedef struct write_wrapper_t {
    // self-pointer struct for enqueing this struct
    queue_elem_t qe;
    // we need to know the loop for enqueuing
    loop_t *loop;
    // libuv write request handle
    uv_write_t uv_write;
    // a pointer to the event_t with the data
    event_t *ev;
    // libuv-style buffer (points into event_t)
    uv_buf_t uv_buf;
} write_wrapper_t;

void write_wrapper_prep(write_wrapper_t *wr_wrap, loop_t *loop);

/* Must be type-punnable to a char*, since that is what uv_read_cb receives.
   When we pass it to the downstream engine, we pass just the event_t, but we
   can recover the original struct because the dstr_t element of the event_t
   points back to the head of this struct. */
typedef struct {
    char buffer[4096];
    event_t event;
} read_wrapper_t;

// to allocate new sessions (when loop.c only know about a single child struct)
// (the void** sets the session pointer, the void* argument is sess_alloc_data)
typedef derr_t (*session_allocator_t)(void**, void*, loop_t*, ssl_context_t*);

// the socket_engine and event loop, one per pipeline
struct loop_t {
    uv_loop_t uv_loop;
    uv_async_t loop_event_passer;
    uv_async_t loop_closer;
    // for pushing reads to the next engine
    queue_t read_events;
    // write reqs, for wrapping incoming write event_t's with libuv stuff
    queue_t write_wrappers;
    queue_t event_q;
    session_allocator_t sess_alloc;
    void *sess_alloc_data;
    // neighboring engine
    void *downstream;
    event_passer_t pass_down;
    event_t quitmsg;
    bool quitting;
    session_deref_t get_loop_data;
    session_iface_t session_iface;
    // error passed by loop_close
    derr_t error;
};

// per-session data struct
struct loop_data_t {
    loop_t *loop;
    uv_ptr_t uvp;
    // a pointer to the parent session
    void *session;
    queue_cb_t read_pause_qcb;
    // libuv socket
    uv_tcp_t sock;
    // the only way to pass an error from no_bufs__pause_reading():
    derr_t pausing_error;
    /* during the unpause hook, a buffer is stored here for the next call to
       the allocator */
    event_t *event_for_allocator;
    // standard engine data items
    engine_data_state_t state;
    event_t close_ev;
};

// num_write_wrappers must match the downstream engine's num_write_events
derr_t loop_init(loop_t *loop, size_t num_read_events,
                 size_t num_write_wrappers,
                 void *downstream, event_passer_t pass_down,
                 session_iface_t session_iface,
                 session_deref_t get_loop_data,
                 session_allocator_t sess_alloc,
                 void *sess_alloc_data);
void loop_free(loop_t *loop);

derr_t loop_run(loop_t *loop);

// function is an event_passer_t
void loop_pass_event(void *loop_engine, event_t *event);

void loop_close(loop_t *loop, derr_t error);

derr_t loop_add_listener(loop_t *loop, const char *addr, const char *svc,
                         uv_ptr_t *uvp);

void loop_data_start(loop_data_t *ld, loop_t *loop, void *session);
/* Not thread safe, can be called exactly once per loop_data_t.  Thread safety
   should be handled at the session level */
void loop_data_close(loop_data_t *ld);
/* Note that there is no loop_data_free().  This is intentional.  When a
   session is closed, it should call loop_data_close, which will trigger the
   libuv thread to close the libuv-related resources of the loop_data_t.  After
   that process is complete the loop_data_t has been completely cleaned up, and
   libuv will downref the session.  In the session's down_ref handler, no
   further cleanup steps are necessary in relation to the loop_data_t. */


#endif // LOOP_H
