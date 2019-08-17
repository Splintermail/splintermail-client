#ifndef IMAP_ENGINE_H
#define IMAP_ENGINE_H

#include <uv.h>

struct imape_worker_t;
typedef struct imape_worker_t imape_worker_t;
struct imape_t;
typedef struct imape_t imape_t;
struct imape_data_t;
typedef struct imape_data_t imape_data_t;

#include "engine.h"
#include "common.h"
#include "imap_read.h"
// TODO: find a way to avoid this include
#include "loop.h"

/*

The imap engine has a multi-threaded execution model, where each imape_data is
processed by a worker thread.  Primarily this is to avoid complexity.  The
general flow of events for a single imape_data_t is:
 1) EV_READs come in from the tls engine
 2) data is tokenized and parsed by the bison code
 3) when a complete message is received, bison makes a callback
 4) that callback does not return control until is is done processing and the
    next event can be handled

The blocking behavior of step 4 has some important consequences:
  - No need to keep a queue of bison callbacks; we only handle 1 at a time
  - No need to have an event struct for each bison callback

Wait, is that actually very important?  What are the consequences of allowing
for asynchronous bison callback completion?
  - Any non-decryption tasks which are slow need their own thread pool, too
  - imape_data is more complicated (but imape is simpler)
  - imap_maildir might not have to be multithreaded (if all access to it is
    from the imape thread)
  - bison callbacks could be completed out-of-order, and would have to be
    reordered, especially for server-side callbacks (which have to respond to
    the user in-order)

Ok, in summary, the asynchronous bison callback processing looks like a lot
more complexity with no real wins; I'm going with synchronous processing, and
sticking to the multi-threaded imape.

-> Wait, is all this really true?  With the multithreaded imap engine, each
imape_data_t would have a queue of pending reads.  With the async bison
callback processing, you could do the same thing; instead of queueing bison
commands you would just store up reads in a queue of pending reads instead of
trying to process them early. Then you would only have one external work task
out at a time, so you don't need to reorder things there either.

What about filesystem access?  I suspect that it is not thread-safe to do the
libuv file system calls from off of the loop thread (such as from on the imape
thread).  That would mean that I would have to refactor the code to be all
single threaded: tls_engine would run on the loop thread, and its
tlse_pass_event function would have to send an async_t to the loop thread to
call tlse_process_events from the main thread.  With a similar change in the
imap_engine, the imap engine could safely call libuv's async filesystem calls.

 -> Eh, I wouldn't have to run the tls thread on the same place actually.  Just
    the imap engine.

I don't know.  Right now I feel like I could rewrite the code without libuv
without too much trouble; only loop.c really depends on it.  Using libuv's
async filesystem calls would be a serious commitment to libuv.

I think I will write it non-async first, because it will be much simpler, and
if I need to rewrite it as async, I can do that.  But I don't have a really
solid reason why this needs to be non-async.

Maybe I should write it totally single-threaded now, and upgrade to fully async
later?  Yes, then I can do some profiling to see how expensive each operation
actually is, so I can know if I do go async how expensive each part of the
processing is.

OK, realistically, are you going to do that?  No.  Just write the multithreaded
imape_t.

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

// the interface provided by either the client or the server logic.
struct imap_logic_t;
typedef struct imap_logic_t imap_logic_t;
struct imap_logic_t {
    void (*handle_read_event)(imap_logic_t *logic, const event_t *ev);
    void (*handle_command_event)(imap_logic_t *logic, const event_t *ev);
    void (*handle_maildir_event)(imap_logic_t *logic, const event_t *ev);
    void (*free)(imap_logic_t *logic);
};

struct imape_data_t {
    // prestart stuff
    session_t *session;
    imape_t *imape;
    bool upwards;
    ref_fn_t ref_up;
    ref_fn_t ref_down;
    derr_t (*imap_logic_init)(imape_data_t *id, imap_logic_t **logic);
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
        derr_t (*imap_logic_init)(imape_data_t*, imap_logic_t**));
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
