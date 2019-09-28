#include "imap_engine.h"
#include "logger.h"
#include "uv_util.h"

static void imape_data_onthread_start(imape_data_t *id);

static void imape_pass_event(engine_t *imap_engine, event_t *ev){
    imape_t *imape = CONTAINER_OF(imap_engine, imape_t, engine);
    queue_append(&imape->event_q, &ev->link);
}

static derr_t imape_worker_new(imape_t *imape, imape_worker_t **out){
    derr_t e = E_OK;
    imape_worker_t *worker = malloc(sizeof(*worker));
    if(worker == NULL){
        *out = NULL;
        ORIG(&e, E_NOMEM, "unable to alloc worker");
    }

    link_init(&worker->link);
    worker->quitting = false;
    worker->imape = imape;
    worker->work_req.data = worker;

    *out = worker;

    return e;

}

static void imape_worker_free(imape_worker_t *worker){
    // currently nothing in the imape_worker_t needs cleanup
    free(worker);
}

static void worker_process_event(imape_worker_t *worker, imape_data_t *id,
        event_t *ev){
    imape_t *imape = worker->imape;
    session_t *session = id->session;
    // this should *really* never happen, so it's not critical to handle it
    if(session != ev->session){
        LOG_ERROR("mismatched session!");
    }

    /* Note: we check id->data_state == DATA_STATE_CLOSED in all cases.  The
       error handling is very similar to what the main imape_t thread does,
       except that the imap_engine's checks can only guarantee protection
       against failure-to-start sessions.  Here we actually can guarantee
       protection aginst started-then-closed sessions, because the imap_worker
       is the only place where onthread_close is ever called. */

    switch(ev->ev_type){
        case EV_READ:
            // a READ came in for the session
            if(id->data_state == DATA_STATE_CLOSED){
                ev->ev_type = EV_READ_DONE;
                imape->upstream->pass_event(imape->upstream, ev);
            }else{
                id->logic->new_event(id->logic, ev);
            }
            break;
        case EV_COMMAND:
            // a command came in for the session from the imap_controller_t
            if(id->data_state == DATA_STATE_CLOSED){
                // XXX
            }else{
                id->logic->new_event(id->logic, ev);
            }
            break;
        case EV_MAILDIR:
            // a maildir event command came in for the session
            if(id->data_state == DATA_STATE_CLOSED){
                // XXX
            }else{
                id->logic->new_event(id->logic, ev);
            }
            break;
        case EV_WRITE_DONE:
            /* a write buffer has become available.  ev->session will already
               be set. */
            if(id->data_state == DATA_STATE_CLOSED){
                /* let the imape_t return the write event to write_buffers and
                   ref_down the session */
                imape->engine.pass_event(imape->upstream, ev);
            }else{
                id->logic->new_event(id->logic, ev);
            }
            break;
        case EV_SESSION_CLOSE:
            // LOG_ERROR("imape: SESSION_CLOSE\n");
            imape_data_onthread_close(id);
            id->ref_down(ev->session, IMAPE_REF_CLOSE_EVENT);
            break;
        default:
            LOG_ERROR("unexpected event type in imap engine, ev = %x\n",
                      FP(ev));
    }
}

// the worker thread, in uv_work_cb form
static void worker_thread(uv_work_t *req){
    imape_worker_t *worker = req->data;
    imape_t *imape = worker->imape;
    // mark this worker as running
    uv_mutex_lock(&imape->workers_mutex);
    imape->running_workers++;
    uv_mutex_unlock(&imape->workers_mutex);

    while(true){
        // get a session that needs processing
        link_t *link = queue_pop_first(&imape->ready_data, true);
        // is this link the quit sentinal?
        if(link == &imape->quit_sentinal){
            // check if there is an imap_data_t behind the sentinal
            link_t *temp = queue_pop_first(&imape->ready_data, false);
            // put the sentinal back for another worker
            queue_append(&imape->ready_data, link);
            if(temp != NULL){
                // keep working
                link = temp;
            }else{
                // no more work for us to do, just quit
                break;
            }
        }

        // get the imape_data_t from the link
        imape_data_t *id = CONTAINER_OF(link, imape_data_t, link);
        id->ref_up(id->session, IMAPE_REF_WORKER);

        // process all the events
        uv_mutex_lock(&id->mutex);
        while(!link_list_isempty(&id->events)){
            link = link_list_pop_first(&id->events);
            uv_mutex_unlock(&id->mutex);

            event_t *ev = CONTAINER_OF(link, event_t, link);
            worker_process_event(worker, id, ev);

            uv_mutex_lock(&id->mutex);
        }
        uv_mutex_unlock(&id->mutex);

        // let the logic do work
        bool more_work = false;
        if(id->data_state != DATA_STATE_CLOSED){
            derr_t e = E_OK;
            IF_PROP(&e, id->logic->do_work(id->logic)){
                id->session->close(id->session, e);
                PASSED(e);
                imape_data_onthread_close(id);
            }else{
                more_work = id->logic->more_work(id->logic);
            }
        }

        // Is this thread done processing?  Or does it need more?
        uv_mutex_lock(&id->mutex);
        if(link_list_isempty(&id->events) && !more_work){
            id->work_state = IMAPE_INACTIVE;
            uv_mutex_unlock(&id->mutex);
        }else{
            id->work_state = IMAPE_WAITING;
            uv_mutex_unlock(&id->mutex);
            queue_append(&imape->ready_data, &id->link);
        }

        id->ref_down(id->session, IMAPE_REF_WORKER);
    }

    // mark this worker as not running anymore
    uv_mutex_lock(&imape->workers_mutex);
    imape->running_workers--;
    uv_cond_signal(&imape->workers_cond);
    uv_mutex_unlock(&imape->workers_mutex);
}

// This will be called on the loop thread
static void worker_exit_cb(uv_work_t *req, int status){
    // if the thread was canceled, somebody else is handling errors.
    if(status == UV_ECANCELED) return;

    // Otherwise, raise an error
    if(status != 0){
        imape_worker_t *worker = req->data;
        derr_t e = E_OK;
        TRACE_ORIG(&e, E_VALUE, "imape worker thread exiting early");
        loop_close(worker->imape->loop, e);
        PASSED(e);
    }
}

derr_t imape_init(imape_t *imape, size_t nwrite_events, engine_t *upstream,
        size_t nworkers, loop_t *loop){
    derr_t e = E_OK;

    imape->engine.pass_event = imape_pass_event;

    imape->work_req.data = imape;
    imape->upstream = upstream;
    imape->quitting = false;
    imape->nwrite_events = nwrite_events;
    imape->quit_ev = NULL;
    imape->loop = loop;
    imape->running_workers = 0;

    link_init(&imape->quit_sentinal);
    link_init(&imape->workers);

    PROP_GO(&e, queue_init(&imape->event_q), fail);
    PROP_GO(&e, queue_init(&imape->ready_data), fail_event_q);
    PROP_GO(&e, event_pool_init(&imape->write_events, nwrite_events),
             fail_ready_data);

    int ret = uv_mutex_init(&imape->workers_mutex);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing mutex", fail_writes);
    }

    ret = uv_cond_init(&imape->workers_cond);
    if(ret < 0){
        TRACE(&e, "uv_cond_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing cond", fail_writes);
    }

    // initialize all the threads
    for(size_t i = 0; i < nworkers; i++){
        imape_worker_t *worker;
        PROP_GO(&e, imape_worker_new(imape, &worker), fail_workers);
        link_list_append(&imape->workers, &worker->link);
    }

    imape->initialized = true;

    return e;

    link_t *link;
fail_workers:
    while((link = link_list_pop_first(&imape->workers)) != NULL){
        imape_worker_t *worker = CONTAINER_OF(link, imape_worker_t, link);
        imape_worker_free(worker);
    }
// fail_mutex:
    uv_mutex_destroy(&imape->workers_mutex);
fail_writes:
    event_pool_free(&imape->write_events);
fail_ready_data:
    queue_free(&imape->ready_data);
fail_event_q:
    queue_free(&imape->event_q);
fail:
    imape->initialized = false;
    return e;
}

void imape_free(imape_t *imape){
    if(!imape->initialized) return;
    link_t *link;
    while((link = link_list_pop_first(&imape->workers)) != NULL){
        imape_worker_t *worker = CONTAINER_OF(link, imape_worker_t, link);
        imape_worker_free(worker);
    }
    uv_mutex_destroy(&imape->workers_mutex);
    event_pool_free(&imape->write_events);
    queue_free(&imape->ready_data);
    queue_free(&imape->event_q);
    imape->initialized = false;
}

// a helper function for passing events in a thread-safe way
// (put the imape_data_t work_state logic in one place)
void imape_add_event_to_imape_data(imape_t *imape, imape_data_t *id,
        event_t *ev){
    uv_mutex_lock(&id->mutex);
    // add event to list
    link_list_append(&id->events, &ev->link);
    // does this event affect the state?
    if(id->work_state == IMAPE_INACTIVE){
        id->work_state = IMAPE_WAITING;
        uv_mutex_unlock(&id->mutex);
        queue_append(&imape->ready_data, &id->link);
    }else{
        uv_mutex_unlock(&id->mutex);
    }
}

/* the main engine thread, in uv_work_cb form.  It really just passes events to
   per-session queues within imape_data_t structs and interacts with worker
   threads. */
static void imape_process_events(uv_work_t *req){
    imape_t *imape = req->data;
    bool should_continue = true;
    while(should_continue){
        link_t *link = queue_pop_first(&imape->event_q, true);
        event_t *ev = CONTAINER_OF(link, event_t, link);
        imape_data_t *id = ev->session ? ev->session->id : NULL;
        // has this session been initialized?
        if(id && id->data_state == DATA_STATE_PREINIT
                && ev->ev_type != EV_SESSION_CLOSE){
            /* onthread start can be safely run from the main thread because
               the main thread can guarantee that no worker threads have access
               to the imape_data_t yet */
            imape_data_onthread_start(id);
        }

        /* Note: we check id->data_state == DATA_STATE_CLOSED in all cases.
           This is to detect failure-to-start sessions, not to detect if a
           previously-working session has closed.  That check has to be done
           inside a mutex by an imape_worker_t. */

        switch(ev->ev_type){
            case EV_READ:
                // LOG_ERROR("imape: READ\n");
                if(imape->quitting || id->data_state == DATA_STATE_CLOSED){
                    ev->ev_type = EV_READ_DONE;
                    imape->upstream->pass_event(imape->upstream, ev);
                }else{
                    // READs are handled by the imape_worker_t's
                    imape_add_event_to_imape_data(imape, id, ev);
                }
                break;
            case EV_COMMAND:
                if(imape->quitting || id->data_state == DATA_STATE_CLOSED){
                    // XXX
                    // id->controller->pass_event(id->controller, ev);
                }else{
                    imape_add_event_to_imape_data(imape, id, ev);
                }
                break;
            case EV_MAILDIR:
                if(imape->quitting || id->data_state == DATA_STATE_CLOSED){
                    // XXX
                    // imap_maildir_pass_event(id->maildir, ev);
                }else{
                    imape_add_event_to_imape_data(imape, id, ev);
                }
                break;
            case EV_WRITE_DONE:
                // LOG_ERROR("imape: WRITE_DONE\n");
                // erase session reference
                id->ref_down(ev->session, IMAPE_REF_WRITE);
                ev->session = NULL;
                // return event to write event list
                queue_append(&imape->write_events, &ev->link);
                // were we waiting for that WRITE_DONE before passing QUIT_UP?
                if(imape->quit_ev
                        && imape->write_events.len == imape->nwrite_events){
                    imape->upstream->pass_event(imape->upstream, imape->quit_ev);
                    imape->quit_ev = NULL;
                    should_continue = false;
                }
                break;
            case EV_QUIT_DOWN:
                // LOG_ERROR("imape: QUIT_DOWN\n");
                // enter quitting state
                imape->quitting = true;
                imape_worker_t *worker;
                // put each worker in the quitting state to drop unstarted work
                LINK_FOR_EACH(worker, &imape->workers, imape_worker_t, link){
                    worker->quitting = true;
                }
                // post the quit_sentinal, to wake up inactive workers
                queue_append(&imape->ready_data, &imape->quit_sentinal);
                /* we can't send EV_QUIT_UP until all workers have agreed to
                   stop sending events (i.e., they have exited) */
                uv_mutex_lock(&imape->workers_mutex);
                while(imape->running_workers > 0){
                    uv_cond_wait(&imape->workers_cond, &imape->workers_mutex);
                }
                uv_mutex_unlock(&imape->workers_mutex);
                // still not done until we have all of our write buffers back
                ev->ev_type = EV_QUIT_UP;
                if(imape->write_events.len < imape->nwrite_events){
                    // store this event for later
                    imape->quit_ev = ev;
                }else{
                    imape->upstream->pass_event(imape->upstream, ev);
                    should_continue = false;
                }
                break;
            case EV_SESSION_START:
                // LOG_ERROR("imape: SESSION_START\n");
                id->ref_down(ev->session, IMAPE_REF_START_EVENT);
                break;
            case EV_SESSION_CLOSE:
                /* onthread close must be run on a worker thread to ensure it
                   is the only thread operating on it at a time */
                imape_add_event_to_imape_data(imape, id, ev);
                break;

            case EV_QUIT_UP:
                LOG_ERROR("imap engine received an illegal QUIT_UP\n");
                break;
            case EV_READ_DONE:
                LOG_ERROR("imap engine received an illegal READ_DONE\n");
                break;
            case EV_WRITE:
                LOG_ERROR("imap engine received an illegal WRITE\n");
                break;
            default:
                LOG_ERROR("unexpected event type in imap engine, ev = %x\n",
                          FP(ev));
        }
    }
}

void imape_data_prestart(imape_data_t *id, imape_t *imape, session_t *session,
        bool upwards, ref_fn_t ref_up, ref_fn_t ref_down,
        logic_alloc_t logic_alloc, void *alloc_data){
    id->imape = imape;
    id->session = session;
    id->upwards = upwards;
    id->ref_up = ref_up;
    id->ref_down = ref_down;
    id->logic_alloc = logic_alloc;
    id->alloc_data = alloc_data;
}

void imape_data_start(imape_data_t *id){
    // prepare the starting event
    event_prep(&id->start_ev);
    id->start_ev.ev_type = EV_SESSION_START;
    id->start_ev.buffer = (dstr_t){0};
    id->start_ev.session = id->session;
    id->ref_up(id->session, IMAPE_REF_START_EVENT);

    // pass the starting event
    id->imape->engine.pass_event(&id->imape->engine, &id->start_ev);
}

static void imape_data_onthread_start(imape_data_t *id){
    derr_t e = E_OK;

    id->data_state = DATA_STATE_STARTED;

    // The mutex is not closed until imape_data_postclose()
    id->mutex_initialized = false;
    int ret = uv_mutex_init(&id->mutex);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing mutex", fail);
    }
    id->mutex_initialized = true;

    PROP_GO(&e, id->logic_alloc(&id->logic, id->alloc_data, id),
            fail_mutex);

    // Multithreaded worker considerations

    id->work_state = IMAPE_INACTIVE;
    link_init(&id->events);
    link_init(&id->link);

    // lifetime reference
    id->ref_up(id->session, IMAPE_REF_LIFETIME);

    return;

fail_mutex:
    uv_mutex_destroy(&id->mutex);
    id->mutex_initialized = false;
fail:
    id->data_state = DATA_STATE_CLOSED;
    id->session->close(id->session, e);
    PASSED(e);
    return;
}

/* session_close() will call imape_data_close() from any thread.  The session
   is required to call this exactly one time for every session. */
void imape_data_close(imape_data_t *id){
    // prepare the closing event
    event_prep(&id->close_ev);
    id->close_ev.ev_type = EV_SESSION_CLOSE;
    id->close_ev.buffer = (dstr_t){0};
    id->close_ev.session = id->session;
    id->ref_up(id->session, IMAPE_REF_CLOSE_EVENT);

    // pass the closing event
    id->imape->engine.pass_event(&id->imape->engine, &id->close_ev);
}

void imape_data_onthread_close(imape_data_t *id){
    // no double closing.  This could happen if imape_data_start failed.
    if(id->data_state == DATA_STATE_CLOSED) return;
    // safe from PREINIT state
    bool exit_early = (id->data_state == DATA_STATE_PREINIT);
    // set state
    id->data_state = DATA_STATE_CLOSED;
    if(exit_early) return;

    // don't close the mutex until after the last downref, in *_postclose()

    id->logic->free(id->logic);

    // lifetime reference
    id->ref_down(id->session, IMAPE_REF_LIFETIME);
}

void imape_data_postclose(imape_data_t *id){
    // the last resource, which can't be freed until after ALL events
    if(id->mutex_initialized){
        uv_mutex_destroy(&id->mutex);
        id->mutex_initialized = false;
    }
}

derr_t imape_add_to_loop(imape_t *imape, uv_loop_t *loop){
    derr_t e = E_OK;
    // add main thread to loop
    int ret = uv_queue_work(loop, &imape->work_req, imape_process_events, NULL);
    if(ret < 0){
        TRACE(&e, "uv_queue_work: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "error adding imap engine to uv loop");
    }
    // add worker threads to loop
    imape_worker_t *worker;
    size_t started_workers = 0;
    LINK_FOR_EACH(worker, &imape->workers, imape_worker_t, link){
        ret = uv_queue_work(loop, &worker->work_req, worker_thread,
                worker_exit_cb);
        if(ret < 0){
            TRACE(&e, "uv_queue_work: %x\n", FUV(&ret));
            ORIG_GO(&e, uv_err_type(ret), "error adding worker to uv loop",
                    fail_workers);
        }
        started_workers++;
    }
    return e;

fail_workers:
    LINK_FOR_EACH(worker, &imape->workers, imape_worker_t, link){
        // only cancel the workers we started
        if(started_workers == 0) break;
        uv_cancel_work(&worker->work_req);
        started_workers--;
    }
//fail_main:
    uv_cancel_work(&imape->work_req);
    return e;
}

DSTR_STATIC(imape_ref_read_dstr, "read");
DSTR_STATIC(imape_ref_write_dstr, "write");
DSTR_STATIC(imape_ref_start_event_dstr, "start_event");
DSTR_STATIC(imape_ref_close_event_dstr, "close_event");
DSTR_STATIC(imape_ref_lifetime_dstr, "lifetime");
DSTR_STATIC(imape_ref_worker_dstr, "worker");
DSTR_STATIC(imape_ref_unknown_dstr, "unknown");

dstr_t *imape_ref_reason_to_dstr(enum imape_ref_reason_t reason){
    switch(reason){
        case IMAPE_REF_READ: return &imape_ref_read_dstr; break;
        case IMAPE_REF_WRITE: return &imape_ref_write_dstr; break;
        case IMAPE_REF_START_EVENT: return &imape_ref_start_event_dstr; break;
        case IMAPE_REF_CLOSE_EVENT: return &imape_ref_close_event_dstr; break;
        case IMAPE_REF_LIFETIME: return &imape_ref_lifetime_dstr; break;
        case IMAPE_REF_WORKER: return &imape_ref_worker_dstr; break;
        default: return &imape_ref_unknown_dstr;
    }
}
