#ifndef IMAP_ENGINE_H
#define IMAP_ENGINE_H

#include <uv.h>

struct imape_worker_t;
typedef struct imape_worker_t imape_worker_t;
struct imape_t;
typedef struct imape_t imape_t;
struct imape_data_t;
typedef struct imape_data_t imape_data_t;
struct imap_logic_t;
typedef struct imap_logic_t imap_logic_t;

#include "engine.h"
#include "common.h"
#include "imap_client.h"
// TODO: find a way to avoid this include
#include "loop.h"

/*

The imap engine has a multi-threaded execution model, where each imape_data is
processed by a single worker thread at a time.  This avoids complexity while
offering decent performance.  Complexity is avoided by allowing the
imape_data_t to essentially be operated by a single thread.  Additionally,
asynchronous file IO is not necessary, but you still don't block the whole
engine when just one session is doing a read or write.

There is one oddity that arises from the multi-threaded architecture: since the
imape_t at quit-time blocks waiting for all of the write buffers to be
returned, the easiest way to correctly return a write buffer not needed by a
worker is to pass it back to the main imape_t thread.

Each imap_data_t is fully bidirectional; that is, if a write is blocked by
network throughput, reads can still be processed, and vice-versa.  The thing
that enables the bidirectionality is the fact that the imap parser returns
a tagged-union object for every line of imap grammar.  Each message that comes
in can be stored neatly in memory and processed at a later time.

Being bidirectional is critical for an IMAP server because you can't expect
clients to behave nicely; the client might be trying to write a large email
over the network to you and it might ignore you if you are also blindly writing
a large email over the network to it.  Additionally, for a client to speak to
servers other than LITERAL+ servers, it is necessary to be able to process
reads in the middle of a write, while you wait for the imap synchronization
message.

--------

Here's the general idea (threads not illustrated):

       EVENT                     EVENT
       QUEUE                     QUEUE
     ___|____                  ____|___                    ____________
    |   v    | READ --------> |    v   | <-- control msgs |            |
    |        | <--- READ_DONE |        |                  |    IMAP    |
    |  TLS   |                |  IMAP  | callback fns --> | CONTROLLER |
    | ENGINE | <------- WRITE | ENGINE | <--- fn ret vals |____________|
    |________| WRITE_DONE --> |        |                   _________
                              |        | <--- file events |         |
                              |        |                  |   IMAP  |
                              |        | callback fns --> | MAILDIR |
                              |________| <--- fn ret vals |_________|

The dual mechanisms for communicating to the imap engine (callback function
return values and events) is due to the synchronous processing of bison
callbacks; the imape_data processing needs immediate responses to function
calls, but asynchronous events are still possible in other cases.

-------

Threading plan:

 1) imape receives an event for an imape_data_t
 2) imape_t locks that imape_data_t's mutex
 3) imape_t inserts the packet into the imape_data_t
 4) if imape_data_t is inactive, set it active and push it to pendings
    else imape_data_t is active; no extra steps required.
 5) if imape_data_t is inactive, set it to active and push it to pendings

*/

struct imape_t {
    bool initialized;
    // generic engine stuff
    engine_t engine;
    uv_work_t work_req;
    queue_t event_q;
    queue_t write_events;
    // upstream engine, to which we pass write and read_done events
    engine_t *upstream;
    // for handling quitting state
    bool quitting;
    event_t *quit_ev;
    size_t nwrite_events;

    // worker system:
    link_t workers; // imape_worker_t->link
    queue_t ready_data; // imape_data_t->link, or quit_sentinal
    link_t quit_sentinal; // imape_t->ready_data
    uv_mutex_t workers_mutex;
    uv_cond_t workers_cond;
    size_t running_workers;
    // only used for loop_close() if worker threads die early.
    loop_t *loop;
};
DEF_CONTAINER_OF(imape_t, engine, engine_t);

/* imape_worker_t is an interface like session_t.  It gives us a neat
   separation between the multithreaded part of the imape_t and the actual imap
   operations. */
struct imape_worker_t {
    imape_t *imape;
    bool quitting;
    uv_work_t work_req;
    link_t link;
};
DEF_CONTAINER_OF(imape_worker_t, link, link_t);

typedef enum {
    IMAPE_INACTIVE,
    IMAPE_WAITING,
    IMAPE_ACTIVE,
} imape_work_state_t;

/* The interface provided by either the client or the server logic.  In all
   cases, the imap_logic_t is responsible for returning *ev. */
struct imap_logic_t {
    // a READ came in for the session
    derr_t (*read)(imap_logic_t *logic, event_t *ev);
    // a command came in for the session from the imap_controller_t
    derr_t (*command)(imap_logic_t *logic, event_t *ev);
    // a maildir event command came in for the session
    derr_t (*maildir)(imap_logic_t *logic, event_t *ev);
    // a write buffer has become available.  ev->session will already be set.
    derr_t (*write_buffer)(imap_logic_t *logic, event_t *ev);
    void (*free)(imap_logic_t *logic);
};

/* TODO: come up with a cleaner interface for the imap_logic_t, or maybe
         abadon the interface and just pass in imape_data_t* and dereference
         these things from there.
   - the engine_t* is needed to return unneeded write events to the imape_t via
     its event queue (so that the imape_t is always aware of write_events
     during the quit sequence)
   - the session_t is only needed for closing the session
   - the queue_t is needed for grabbing write events from the imape_t
*/
typedef derr_t (*logic_alloc_t)(imap_logic_t**, void*, imape_data_t *id);

struct imape_data_t {
    // prestart stuff
    session_t *session;
    imape_t *imape;
    bool upwards;
    ref_fn_t ref_up;
    ref_fn_t ref_down;
    logic_alloc_t logic_alloc;
    void *alloc_data;
    // generic per-engine data stuff
    engine_data_state_t data_state;
    event_t start_ev;
    event_t close_ev;
    // IMAP-engine-specific stuff
    imap_logic_t *imap_logic;

    // Multithreaded worker considerations
    uv_mutex_t mutex;
    bool mutex_initialized;
    imape_work_state_t work_state;
    link_t events; // event_t->link
    link_t link; // imape_t->ready_data
};
DEF_CONTAINER_OF(imape_data_t, link, link_t);

derr_t imape_init(imape_t *imape, size_t nwrite_events, engine_t *upstream,
        size_t nworkers, loop_t *loop);
void imape_free(imape_t *imape);
derr_t imape_add_to_loop(imape_t *imape, uv_loop_t *loop);

void imape_data_prestart(imape_data_t *id, imape_t *imape, session_t *session,
        bool upwards, ref_fn_t ref_up, ref_fn_t ref_down,
        logic_alloc_t logic_alloc, void *alloc_data);
void imape_data_start(imape_data_t *id);
void imape_data_close(imape_data_t *id);
/* with a multi-threaded engine, since SESSION_CLOSE is not guaranteed to be
   the last event for a session, onthread_close can't free mutexes (because the
   main thread will need those mutexes to even check if onthread_close has
   run), so we will introduce a new function which only runs after all session
   refs are freed */
void imape_data_postclose(imape_data_t *id);

// expose onthread_close for the worker
void imape_data_onthread_close(imape_data_t *id);

enum imape_ref_reason_t {
    IMAPE_REF_READ = 0,
    IMAPE_REF_WRITE,
    IMAPE_REF_START_EVENT,
    IMAPE_REF_CLOSE_EVENT,
    IMAPE_REF_LIFETIME,
    IMAPE_REF_WORKER,
    IMAPE_REF_MAXIMUM
};

dstr_t *imape_ref_reason_to_dstr(enum imape_ref_reason_t reason);

#endif // IMAP_ENGINE_H
