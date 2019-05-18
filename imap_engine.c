#include "imap_engine.h"
#include "logger.h"

static void imape_data_onthread_start(imape_data_t *id, imape_t *imape,
                                      void *session);
static void imape_data_onthread_close(imape_data_t *id);

void status_type(void *data, dstr_t tag, status_type_t status,
                 status_code_t code, unsigned int code_extra,
                 dstr_t text){
    imap_data_t *id = data;
}

static imap_parse_hooks_up_t imape_hooks_up = {
};
static imap_parse_hooks_up_t imape_hooks_dn = {
};

derr_t imape_init(imape_t *imape, size_t nwrite_events,
                  session_iface_t session_iface,
                  imape_data_t *(*session_get_imape_data)(void*),
                  bool (*session_get_upwards)(void*),
                  event_passer_t pass_up, void *upstream){
    derr_t error;

    PROP_GO( queue_init(&imape->event_q), fail);
    PROP_GO( event_pool_init(&imape->write_events, nwrite_events),
             fail_event_q);

    imape->work_req.data = imape;
    imape->session_iface = session_iface;
    imape->session_get_imape_data = session_get_imape_data;
    imape->session_get_upwards = session_get_upwards;
    imape->pass_up = pass_up;
    imape->upstream = upstream;

    imape->quitting = false;
    imape->nwrite_events = nwrite_events;
    imape->quit_ev = NULL;

    imape->initialized = true;
    return E_OK;

fail_event_q:
    queue_free(&imape->event_q);
fail:
    imape->initialized = false;
    return error;
}

void imape_free(imape_t *imape){
    if(!imape->initialized) return;
    event_pool_free(&imape->write_events);
    queue_free(&imape->event_q);
}

// the main engine thread, in uv_work_cb form
static void imape_process_events(uv_work_t *req){
    imape_t *imape = req->data;
    session_iface_t iface = imape->session_iface;
    bool should_continue = true;
    while(should_continue){
        event_t *ev = queue_pop_first(&imape->event_q, true);
        imape_data_t *id;
        id = ev->session ? imape->session_get_imape_data(ev->session) : NULL;
        // has this session been initialized?
        if(id && id->data_state == DATA_STATE_PREINIT
                && ev->ev_type != EV_SESSION_CLOSE){
            // TODO: client connections start the handshake
            imape_data_onthread_start(id, imape, ev->session);
        }
        switch(ev->ev_type){
            case EV_READ:
                // LOG_ERROR("imape: READ\n");
                if(imape->quitting){
                    ev->error = E_OK;
                    ev->ev_type = EV_READ_DONE;
                    imape->pass_up(imape->upstream, ev);
                }else{
                    queue_append(&id->pending_reads, &ev->qe);
                }
                break;
            case EV_WRITE_DONE:
                // LOG_ERROR("imape: WRITE_DONE\n");
                // erase session reference
                iface.ref_down(ev->session, IMAP_REF_WRITE);
                ev->session = NULL;
                // return event to write event list
                queue_append(&imape->write_events, &ev->qe);
                // were we waiting for that WRITE_DONE before passing QUIT_UP?
                if(imape->quit_ev
                        && imape->write_events.len < imape->nwrite_events){
                    imape->pass_up(imape->upstream, imape->quit_ev);
                    imape->quit_ev = NULL;
                    should_continue = false;
                }
                break;
            case EV_QUIT_DOWN:
                // LOG_ERROR("imape: QUIT_DOWN\n");
                // enter quitting state
                imape->quitting = true;
                // get ready to pass the quit back up the pipe
                ev->ev_type = EV_QUIT_UP;
                // can we pass the quit along immediately?
                if(imape->write_events.len < imape->nwrite_events){
                    // no; store this event for later
                    imape->quit_ev = ev;
                }else{
                    // yes; pass the QUIT and exit
                    imape->pass_up(imape->upstream, ev);
                    should_continue = false;
                }
                break;
            case EV_SESSION_START:
                // LOG_ERROR("imape: SESSION_START\n");
                iface.ref_down(ev->session, IMAP_REF_START_EVENT);
                break;
            case EV_SESSION_CLOSE:
                // LOG_ERROR("imape: SESSION_CLOSE\n");
                imape_data_onthread_close(id);
                iface.ref_down(ev->session, IMAP_REF_CLOSE_EVENT);
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

void imape_pass_event(void *imape_void, event_t *ev){
    imape_t *imape = imape_void;
    queue_append(&imape->event_q, &ev->qe);
}

void imape_data_start(imape_data_t *id, imape_t *imape, void *session){
    // prepare the starting event
    event_prep(&id->start_ev, id);
    id->start_ev.ev_type = EV_SESSION_START;
    id->start_ev.error = E_OK;
    id->start_ev.buffer = (dstr_t){0};
    id->start_ev.session = session;
    imape->session_iface.ref_up(session, IMAPE_REF_START_EVENT);

    // pass the starting event
    imape_pass_event(imape, &id->start_ev);
}

static void imape_data_onthread_start(imape_data_t *id, imape_t *imape,
                                      void *session){
    derr_t error;
    id->data_state = DATA_STATE_STARTED;
    id->imape = imape;
    id->session = session;

    id->upwards = imape->session_get_upwards(session);

    // decide which hooks to use
    imap_parse_hooks_up_t hooks_up = {0};
    imap_parse_hooks_dn_t hooks_dn = {0};
    if(id->upwards){
        hooks_up = imape_hooks_up(session);
    }else{
        hooks_dn = imape_hooks_dn(session);
    }

    PROP_GO( imap_reader_init(&id->reader, hooks_dn, hooks_up, id), fail);

    PROP_GO( queue_init(&id->pending_reads), fail_reader);

    queue_cb_prep(&id->read_in_qcb);

    // lifetime reference
    imape->session_iface.ref_up(session, IMAPE_REF_LIFETIME);

    return;

fail_reader:
    imap_reader_free(id->reader);
fail:
    imape->session_iface.close(session);
    id->data_state = DATA_STATE_CLOSED;
    return;
}

/* session_close() will call imape_data_close() from any thread.  The session
   is required to call this exactly one time for every session. */
void imape_data_close(imape_data_t *id, imape_t *imape, void *session){
    (void)session;
    // prepare the closing event
    event_prep(&id->close_ev, id);
    id->close_ev.ev_type = EV_SESSION_CLOSE;
    id->close_ev.error = E_OK;
    id->close_ev.buffer = (dstr_t){0};
    id->close_ev.session = session;
    imape->session_iface.ref_up(session, IMAPE_REF_CLOSE_EVENT);

    // pass the closing event
    imape_pass_event(imape, &id->close_ev);
}

static void imape_data_onthread_close(imape_data_t *id){
    // no double closing.  This could happen if imape_data_start failed.
    if(id->data_state == DATA_STATE_CLOSED) return;
    // safe from PREINIT state
    bool exit_early = (id->data_state == DATA_STATE_PREINIT);
    // set state
    id->data_state = DATA_STATE_CLOSED;
    if(exit_early) return;

    // remove qcb's from queues
    // TODO

    // empty queues
    // TODO

    queue_free(&id->pending_reads);
    imap_reader_free(&id->reader);

    // lifetime reference
    imape->session_iface.ref_down(session, IMAPE_REF_LIFETIME);
}
