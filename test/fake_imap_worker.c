#include <uv.h>

#include <libdstr/libdstr.h>
#include <libuvthread/libuvthread.h>
#include <engine.h>

#include "fake_imap_worker.h"

typedef struct {
    // this implements the imape_worker_t interface
    imape_worker_t imape_worker;
    // right now there is nothing else
} imap_worker_t;
DEF_CONTAINER_OF(imap_worker_t, imape_worker, imape_worker_t)


static void worker_free(imape_worker_t *worker){
    // currently nothing to clean up
    (void)worker;
}


static void worker_process_event(imape_worker_t *worker, imape_data_t *id,
        event_t *ev){
    derr_t e = E_OK;
    imape_t *imape = worker->imape;
    session_t *session = id->session;
    // this should *really* never happen, so it's not critical to handle it
    if(session != ev->session){
        LOG_ERROR("mismatched session!");
    }
    link_t *link;
    event_t *write_ev;
    switch(ev->ev_type){
        case EV_READ:
            // echo the message back for now
            link = queue_pop_first(&imape->write_events, true);
            write_ev = CONTAINER_OF(link, event_t, link);
            /* Now that we have taken the event from write_events, it's never
               thread-safe put it back from the worker thread, unless we were
               to signal the main thread to check to possibly send the QUIT_UP.
               So instead, if there is an error, we will just pass the event
               back to the main thread as a WRITE_DONE.  That is why we are
               setting the session and doing a ref_up right now. */
            write_ev->session = session;
            id->ref_up(session, IMAPE_REF_WRITE);
            IF_PROP(&e, dstr_copy(&ev->buffer, &write_ev->buffer)){
                // have the main thread put the event back
                write_ev->ev_type = EV_WRITE_DONE;
                imape_pass_event(imape, write_ev);
                // close the session
                session->close(session, e);
                PASSED(e);
                imape_data_onthread_close(id);
            }else{
                // send the write
                write_ev->ev_type = EV_WRITE;
                imape->pass_up(imape->upstream, write_ev);
            }
            // respond with READ_DONE
            ev->ev_type = EV_READ_DONE;
            imape->pass_up(imape->upstream, ev);
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

derr_t fake_imap_worker_new(imape_t *imape, imape_worker_t **imape_worker){
    (void)imape;
    derr_t e = E_OK;
    imap_worker_t *worker = malloc(sizeof(*worker));
    if(worker == NULL){
        ORIG(&e, E_NOMEM, "unable to alloc worker");
    }

    // set up the interface
    worker->imape_worker.process_event = worker_process_event;
    worker->imape_worker.free = worker_free;

    // imape is responsible for the remaining members of the interface

    // return the interface
    *imape_worker = &worker->imape_worker;

    return e;

}
