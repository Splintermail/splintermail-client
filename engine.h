#ifndef ENGINE_H
#define ENGINE_H

#include "queue.h"
#include "common.h"

/* This header defines some interfaces by which engines pass events to each
   other.  Hence the name "engine.h". */

/*
The NEW new and improved modular pipelining:

       EVENT                       EVENT                       EVENT
       QUEUE                       QUEUE                       QUEUE
     ____|_____                  ____|____                  _____|______
    |    v     | READ --------> |    v    | READ --------> |     v      |
    |          | <--- READ_DONE |  SOME   | <--- READ_DONE |            |
    | UPSTREAM |                | GENERIC |                | DOWNSTREAM |
    |  ENGINE  | <------- WRITE | ENGINE  | <------- WRITE |   ENGINE   |
    |__________| WRITE_DONE --> |_________| WRITE_DONE --> |____________|

    Notes:
      - An engine provides one `event_passer_t`-type function to other engines
      - READ, READ_DONE, WRITE, WRITE_DONE, and errors are passed as event_t's
*/

typedef enum {
    // a filled buffer from upstream.  Might carry an error.
    EV_READ = 10,

    // an empty buffer passed back from downstream
    EV_READ_DONE,

    // a filled buffer from downstream
    EV_WRITE,

    // an empty buffer passed back from upstream.  Might carry an error.
    EV_WRITE_DONE,

    /* First half of the quit sequence.  This must start with the upstream-most
       engine (the socket engine).

       After sending this event it is not allowed to push additional EV_READ or
       EV_WRITE events.  Thus, after seeing QUIT_DOWN, an engine can know it
       will receive no more EV_READ events.  After sending QUIT_DOWN, any
       EV_WRITE events can be returned without processing.

       This should be the order of things:
         - Engine X sees EV_QUIT_DOWN from upstream
         - X passes EV_QUIT_DOWN to downstream, reusing the same event_t
         - X constantly pops events from its queue:
           - EV_READ should not happen
           - EV_READ_DONE events are ignored (just return-to-pool)
           - EV_WRITE events trigger EV_WRITE_DONE immediately
           - EV_WRITE_DONE events are ignored (just return-to-pool)
           - EV_QUIT_UP triggers an EV_QUIT_UP message if all of X's write
             events are accounted for (the downstream node sending QUIT_UP
             will have sent back all of the read events already). If
             EV_QUIT_UP cannot be sent yet it should be stored and sent after
             the final READ_DONE event.
         - X exits.  It is now safe to free X: all of its own events have been
           returned, and it has returned all other events to its upstream and
           downstream neighbor engines.

        Also, QUIT_DOWN is its own special event_t, which is allocated at the
        beginning of the program, and it is passed along all the way down the
        pipe and back up it (as a QUIT_UP).  That simplifies the quit sequence
        a lot.
    */
    EV_QUIT_DOWN,

    // second half of the quit sequence.
    EV_QUIT_UP,

    /* This is like a reminder, in case no normal events come in, to trigger
       an engine to look at a session.  Things like starting TCP connections
       or starting TLS handshakes might only work with this trigger, though if
       any normal events for this session are received before this event,
       those events should trigger the engine data initialization and this
       event should be ignored.  Not all engine data init()s will generate a
       SESSION_START, but any pipeline should have at least one node which
       needs a SESSION_START event to guarantee correct behavior, because all
       sessions either need a connection accept()'ed or they need somebody to
       pass the first data packet.  Since those things likely have to happen
       on-thread, SESSION_START is the trigger required. */
    EV_SESSION_START,

    /* Triggers an engine to mark the session as closed and to free all
       resources this engine's data struct.  After the last ref_down the
       whole session will be freed.  Normal events recieved after a
       SESSION_CLOSE should not be processed (since the engine doesn't have
       the state in memory required to process them anymore). */
    EV_SESSION_CLOSE,

    /* SESSION_START and SESSION_CLOSE gotchas every engine should handle:
        1. normal events trigger engine data initialization if they are
           received by an engine data in DATA_STATE_PREINIT.
        2. normal events are ignored if they are recieved by an engine data in
           DATA_STATE_CLOSED
        3. SESSION_START should be ignored if it is received by an engine data
           in DATA_STATE_STARTED or DATA_STATE_CLOSED
        4. An engine_data_onthread_close() should be almost a noop in the case
           where the session is in the DATA_STATE_PREINIT when it is called.
        5. An engine should not set its data to DATA_STATE_CLOSED during error
           handling directly; it should typically make a call to its own
           engine_data_onthread_close().  It should also make a call to the
           session_close(), in which case engine_data_onthread_close() may be
           called later, and it should be idempotent.  That means:
            - engine_data_close() can be called from off-thread and the session
              protects it from being double called, but
            - engine_data_onthread_close() can not be called from off-thread
              and it *must* be safe from double calls.

    */
} event_type_t;

typedef struct session_t {
    void *ld;
    void *td;
    void *id;
    void (*close)(struct session_t*, derr_t error);
} session_t;

typedef void (*ref_fn_t)(session_t*, int reason);

typedef struct {
    dstr_t buffer; // for passing buffers.  Comes first to accomadate libuv.
    event_type_t ev_type;
    session_t *session; // points to the session_interface.  Changes each time.
    queue_elem_t qe; // for holding in queues
    queue_cb_t qcb; // for waiting on another buffer
} event_t;

// Does not set session, init the dstr, or set callbacks.
void event_prep(event_t *ev);

// pass an event to an engine
typedef void (*event_passer_t)(void*, event_t*);

/* This state will be initialized in the session allocator to PREINIT.  The
   state can only move forward.  It is possible for an engine to recieve a
   SESSION_CLOSE event before a SESSION_START event, in which case the
   SESSION_START should be mostly ignored (likely downref, no more) */
typedef enum {
    DATA_STATE_PREINIT = 0,
    DATA_STATE_STARTED,
    DATA_STATE_CLOSED,
} engine_data_state_t;

// free all the events in a pool and then call queue_free
void event_pool_free(queue_t *pool);

// call queue_init(), allocate/append a bunch of events
derr_t event_pool_init(queue_t *pool, size_t nevents);

#endif // ENGINE_H
