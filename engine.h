#ifndef ENGINE_H
#define ENGINE_H

#include "queue.h"

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
    EV_READ,

    // an empty buffer passed back from downstream
    EV_READ_DONE,

    // a filled buffer from downstream
    EV_WRITE,

    // an empty buffer passed back from upstream.  Might carry an error.
    EV_WRITE_DONE,

    /* First half of the quit sequence.  This must start with the upstream-most
       engine (the socket engine).

       After sending this event it is not allowed to push additional EV_READ or
       EV_WRITE events.  Thus, after seeing this message, an engine can know it
       will receive no more EV_READ events, and any EV_WRITE events can be
       returned without processing.

       It is possible for a deadlock to form in the following condition:
         - An engine receiving the EV_QUIT_DOWN has all of its reads in flight
         - All engines downstream also have all of thier reads in flight, as
           well as all of their writes in flight
         - The engine receiving the EV_QUIT_DOWN waits for a read buffer to be
           but does not return any EV_WRITE_DONEs from its queue

       Therefore, this should be the order of things:
         - Engine X sees EV_QUIT_DOWN from upstream
         - X passes EV_QUIT_DOWN to downstream ASAP, possibly immediately
         - X constantly pops events from its queue:
           - EV_READ should not happen
           - EV_READ_DONE events may trigger sending the EV_QUIT_DOWN (once)
           - EV_WRITE events trigger EV_WRITE_DONE immediately
           - EV_WRITE_DONE events are ignored
           - EV_QUIT_UP triggers an EV_QUIT_UP message.  EV_QUIT_UP should be
             passed back via the buffer that EV_QUIT_DOWN was sent.
         - X exits.  It is now safe to free X: all of its own events have been
           returned, and it has returned all other events to its upstream and
           downstream neighbor engines.
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
} event_type_t;

typedef struct {
    dstr_t buffer; // for passing buffers.  Comes first to accomadate libuv.
    derr_t error; // only for passing errors
    event_type_t ev_type;
    void *data; // points to parent struct, if any.  Does not change.
    void *session; // points to some session-level data.  Changes each time.
    queue_elem_t qe; // for holding in queues
    queue_cb_t qcb; // for waiting on another buffer
} event_t;

// Does not init the dstr or set callbacks.
static inline void event_prep(event_t *ev, void *parent_struct){
    ev->error = E_OK;
    ev->data = parent_struct;
    ev->session = NULL;
    queue_elem_prep(&ev->qe, ev);
    queue_cb_prep(&ev->qcb, ev);
}

// pass an event to an engine
typedef void (*event_passer_t)(void*, event_t*);

// dereference engine-specific session data from an event->session
typedef void *(*session_deref_t)(void*);

// the generic session "interface", identical API for each engine
typedef struct {
    void (*ref_up)(void*);
    void (*ref_down)(void*);
    void (*close)(void*, derr_t error);
    void (*lock)(void*);
    void (*unlock)(void*);
    // non-error events should not be processed for invalid sessions
    bool (*is_invalid)(void*);
    // no events at all should be processed for complete sessions
    bool (*is_complete)(void*);
} session_iface_t;


/* This state will be initialized in the session allocator to PREINIT.  The
   state can only move forward.  It is possible for an engine to recieve a
   SESSION_CLOSE event before a SESSION_START event, in which case the
   SESSION_START should be mostly ignored (likely downref, no more) */
typedef enum {
    DATA_STATE_PREINIT = 0,
    DATA_STATE_STARTED,
    DATA_STATE_CLOSED,
} engine_data_state_t;

/*
Wait, we have too many data structs.  Where do they all go?

    Engine structs:
        loop_t
        tlse_t
        imape_t

    Session structs:
        ixs_t {
            uv_conn_t; // which connection is ours
            tlse_data_t; // state of the TLS session
            imape_data_t; // state of the IMAP session
            ...
        }

    Event structs:
        event_t {
            void *session; // points to a session associated with the event
            ...
            queue_elem_t qe; // each event is able to wait for the next step
        }

OK, passing an event touches all three:
  - The event is an EVENT STRUCT
  - The event.data is a SESSION STRUCT
  - The event is passed into a queue within an ENGINE STRUCT

What about buffer-freed-callbacks?  How will those work?
  - The queue element for waiting is built into the session and so the callback
    knows about the SESSION STRUCT
  - The session has a pointer to the ENGINE STRUCT
  - The callback itself returns the EVENT STRUCT as well

Conclusion:
  - Engines can be initialized with pointers to their neighboring engines
  - Events carry *aux which points to a session
  - event_t's do not need reference counts, only one engine owns them at a time
  - The event_t's *session counts as a session reference
*/

#endif // ENGINE_H
