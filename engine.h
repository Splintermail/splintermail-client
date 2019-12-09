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

    // events which come to the imap engine from outside the pipeline
    EV_COMMAND,
    EV_MAILDIR,

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

    // A guarantee that says "I'm done sending READ events."
    EV_QUIT_DOWN,

    /* A guarantee that says "I'm done sending READ events, I have sent all
       my READ_DONE events, I have received all of my WRITE_DONE events, and
       I'm done sending WRITE events too." */
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

struct event_t;
typedef struct event_t event_t;

typedef void (*event_returner_t)(event_t *ev);

struct event_t {
    dstr_t buffer; // for passing buffers.  Comes first to accommodate libuv.
    event_type_t ev_type;
    session_t *session; // points to the session_interface.  Changes each time.
    link_t link; // for holding in queues
    queue_cb_t qcb; // for waiting on another buffer
    event_returner_t returner;
    void *returner_arg;
};
DEF_CONTAINER_OF(event_t, link, link_t);
DEF_CONTAINER_OF(event_t, qcb, queue_cb_t);

// Does not set session, or callbacks, or init the buffer.
void event_prep(event_t *ev, event_returner_t returner, void *returner_arg);

typedef struct engine_t {
    void (*pass_event)(struct engine_t*, event_t*);
} engine_t;

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
derr_t event_pool_init(queue_t *pool, size_t nevents,
        event_returner_t returner, void *returner_arg);

/* General interface for two-phase shutdown.  This is useful for hierarchical
   ownership structures in multithreaded situations.  The general rules are:
    1. On error, tell your children to die, and report "dying" to your manager
    2. After you report "dying", wait for all children to report "dead"
    3. Wait for any floating references to be released
    4. Report "dead" to your manager
    5. As your manager frees you, you also free your children

   Corrolaries are:
    1. Hiting an error, your child saying "dying", or your manager saying "die"
       are often equivalent (unless you can gracefully handle a child dying).
    2. Your child releasing a reference or reporting "dead" are equivalent.
    3. It doesn't make sense for your manager to hold a reference of yours...
    4. ... instead, your manager should keep its own reference count.

   Conclusions are:
    1. Most engine_data_t's already behave similar to this, except that only
       one of them (the imape_data_t) actually needs the final free step.

   Alternate solution:
       object is freed on the last ref-down, but its owner also has a
       reference.  The "dying" call happens the same, but for its owner that
       just means "downref the object when you'd like to".  So it's more of a
       "dying" call in one direction and an "allowed to die" in the other.

       This is how GObject reference counts work.  My concern is:
         - This will lead to a necessity of reference counting everywhere
         - This will make callback-based objects hard to write.

       I think these are valid concerns: I think the fetch_controller would
       have to give a reference to the imap_client, and the imap client would
       have to downref the fetch controller when it was done making calls in
       order to be sure that the imap_client can't make one last call into a
       freed fetch_controller.  The strategy I've outlined is designed such
       that an object's lifetime must envelop the lifetimes of all the objects
       it owns, and I'm comfortable with that limitation.  Perhaps such a
       limitation is invalid for a cross-language codebase like GObject, but
       it should be fine in my C library. It will save me from having to deal
       with bidirectional reference counts when I prefer hierarchical object
       layouts anyway.

       Update: I don't think these are valid concerns anymore.  But there is
       one thing which is not well addressed here: how do you tell your peers
       that you are dying?  That case is not handled at all in the framework
       outlined above.  In a way, all of the GObject items are peers of each
       other, which makes for a nice symmetry.  But in that case, how does
       error handling work?

   Possible types of calls:
     - I'm dying and I'm entrusting this error to you
     - I'm dying, but you I'm not duplicating my error for the likes of you
     - I'm your parent object and you must die now

   Possible solutions:
     - You could SPLIT your error for every "dying" call, and free the original
       when your refs go to zero.  That fixes the symmetry problem but it would
       result in weird stack traces.  I guess you could special case it so that
       if you only had one referrer you didn't SPLIT at all.
     - You could not send your error at all, but your manager could take you
       instead.  This fixes the arbitrary SPLITs in the stack traces, but it
       doesn't address the problem of legitmate needs for SPLITs.  This is sort
       of a pull strategy for errors, but I think the push strategy is more
       flexible here.
           The problem with the push strategy is that the child object tells
           the parent object when to die, when it should be the parent who
           decides if it can handle the error or not.  A "dying" call that
           sends errors to all the right people seems best still.

     -  But with a generic pushing strategy then you have the two-sided
        interfaces everywhere, because for every call to "dying", the caller
        needs to know which callee gets which error, and the callee needs to
        know which referenced object is dying, so it can decide what to do.

   Conclusion

       I think that the current strategy in the imap_session, which is
       essentially a once-off error handling flow (all the actors are named,
       each one knows the rules for shutting down the system), is probably the
       most scalable.  Any error handling framework that was general enough to
       handle any situation would probably be uselessly complicated.

       The manager interface is really good for hierarchichal error flows, and
       maybe we introduce one additional flow, which can be an accessor error
       flow, where a shared resource should BROADCAST its error to every
       accessor.
*/

struct manager_i;
typedef struct manager_i manager_i;

struct manager_i {
    // report "dying" when you start failing
    void (*dying)(manager_i*, derr_t);
    // report "dead" when you are ready to be freed
    void (*dead)(manager_i*);
};

#endif // ENGINE_H
