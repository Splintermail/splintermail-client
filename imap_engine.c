#include <errno.h>
#include <stdlib.h>

#include "imap_engine.h"
#include "libdstr/libdstr.h"
#include "uv_util.h"

static void imape_data_onthread_start(imape_data_t *id);
static void imape_data_onthread_close(imape_data_t *id);
static void imape_data_read_stuff(imape_data_t *id, const dstr_t *buffer);
static void imape_data_write_stuff(imape_data_t *id);

static void imape_pass_event(engine_t *imap_engine, event_t *ev){
    imape_t *imape = CONTAINER_OF(imap_engine, imape_t, engine);
    queue_append(&imape->event_q, &ev->link);
}

static void imape_return_write_event(event_t *ev){
    imape_t *imape = ev->returner_arg;
    ev->ev_type = EV_WRITE_DONE;
    imape_pass_event(&imape->engine, ev);
}

derr_t imape_init(imape_t *imape, size_t nwrite_events, engine_t *upstream,
        engine_t *quit_downstream){
    derr_t e = E_OK;
    *imape = (imape_t){
        .engine = { .pass_event = imape_pass_event },

        .upstream = upstream,
        .quit_downstream = quit_downstream,
        .quitting = false,
        .nwrite_events = nwrite_events,
        .quit_ev = NULL,
    };

    imape->work_req.data = imape;


    PROP_GO(&e, queue_init(&imape->event_q), fail);
    PROP_GO(&e, event_pool_init(&imape->write_events, nwrite_events,
                imape_return_write_event, imape), fail_event_q);

    imape->initialized = true;

    return e;

fail_event_q:
    queue_free(&imape->event_q);
fail:
    imape->initialized = false;
    return e;
}

void imape_free(imape_t *imape){
    if(!imape->initialized) return;
    event_pool_free(&imape->write_events);
    queue_free(&imape->event_q);
    imape->initialized = false;
}

// the main engine thread, in uv_work_cb form.
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
            imape_data_onthread_start(id);
        }
        switch(ev->ev_type){
            case EV_READ:
                if(imape->quitting || id->data_state == DATA_STATE_CLOSED){
                    ev->returner(ev);
                }else{
                    // pass the contents through the parser
                    imape_data_read_stuff(id, &ev->buffer);
                    ev->returner(ev);
                }
                break;
            case EV_WRITE:
                if(imape->quitting || id->data_state == DATA_STATE_CLOSED){
                    ev->returner(ev);
                }else{
                    // the imap writer can't always write things right away
                    link_list_append(&id->unwritten, &ev->link);
                    // but do write whatever we can
                    imape_data_write_stuff(id);
                }
                break;
            case EV_WRITE_DONE:
                // LOG_ERROR("imape: WRITE_DONE\n");
                if(ev->prev){
                    // let the downstream know this event is fully written
                    ev->prev->returner(ev->prev);
                    ev->prev = NULL;
                }
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
                // pass the QUIT_DOWN along
                imape->quit_downstream->pass_event(imape->quit_downstream, ev);
                break;
            case EV_QUIT_UP:
                // LOG_ERROR("imape: QUIT_UP\n");
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
                imape_data_onthread_close(id);
                id->ref_down(ev->session, IMAPE_REF_CLOSE_EVENT);
                break;

            case EV_READ_DONE:
                LOG_ERROR("imap engine received an illegal READ_DONE event\n");
                break;
            case EV_INTERNAL:
                LOG_ERROR("imap engine received an illegal INTERNAL event\n");
                break;
            default:
                LOG_ERROR("unexpected event type in imap engine, ev = %x\n",
                          FP(ev));
        }
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
    return e;
}

static void warn_event_returner(event_t *ev){
    imape_data_t *id = ev->returner_arg;
    id->ref_down(ev->session, IMAPE_REF_WRITE);

    imap_event_t *imap_ev = CONTAINER_OF(ev, imap_event_t, ev);
    imap_resp_free(imap_ev->arg.resp);
    free(imap_ev);
}

static derr_t warn_invalid_command(imape_data_t *id){
    derr_t e = E_OK;

    /* create an untagged BAD reply to indicate that we couldn't identify the
       source of the problem (since we don't yet have a way to get the tag from
       a partially parsed command) */

    DSTR_STATIC(msg, "failure parsing command");
    ie_dstr_t *text = ie_dstr_new(&e, &msg, KEEP_RAW);
    ie_st_resp_t *st_resp = ie_st_resp_new(&e, NULL, IE_ST_BAD, NULL, text);
    imap_resp_arg_t arg = {.status_type=st_resp};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);
    CHECK_GO(&e, fail);

    imap_event_t *imap_ev = malloc(sizeof(*imap_ev));
    if(!imap_ev) ORIG_GO(&e, E_NOMEM, "nomem", fail_resp);
    *imap_ev = (imap_event_t){
        .type = IMAP_EVENT_TYPE_RESP,
        .arg = { .resp = resp },
    };

    event_prep(&imap_ev->ev, warn_event_returner, id);
    imap_ev->ev.session = id->session;
    imap_ev->ev.ev_type = EV_WRITE;
    id->ref_up(id->session, IMAPE_REF_WRITE);

    imape_pass_event(&id->imape->engine, &imap_ev->ev);

    return e;

fail_resp:
    imap_resp_free(resp);

fail:
    return e;
}

// pass a buffer through the imap reader
static void imape_data_read_stuff(imape_data_t *id, const dstr_t *buffer){
    derr_t e = E_OK;

    // check for EOF
    if(buffer->len == 0){
        ORIG_GO(&e, E_CONN, "Received unexpected EOF", fail);

    }

    LOG_INFO("recv: %x", FD(buffer));
    derr_t e2 = imap_read(&id->reader, buffer);
    // imap clients should fail immediately on syntax errors
    if(id->control->is_client){
        PROP_GO(&e, e2, fail);
    // imap servers should issue warnings to the client
    }else CATCH(e2, E_PARAM){
        derr_t e3 = warn_invalid_command(id);
        CATCH(e3, E_ANY){
            // merge the new error with the original error
            MERGE_VAR(&e2, &e3, "failure to warn about invalid command\n");
            // rethrow the original error
            RETHROW_GO(&e, &e2, E_PARAM, fail);
        }
        DROP_VAR(&e2);
    } else PROP_GO(&e, e2, fail);

    return;

fail:
    id->session->close(id->session, e);
    PASSED(e);
    imape_data_onthread_close(id);
    return;
}

// we use the callback form, queue_pop_first_cb(), to request write events
static void id_prewait_cb(queue_cb_t *qcb){
    /* upref the session while we wait for a write event.  When we receive one,
       we will just reuse this reference */
    imape_data_t *id = CONTAINER_OF(qcb, imape_data_t, write_qcb);
    id->ref_up(id->session, IMAPE_REF_WRITE);
    // mark that we have already requested a write
    id->write_requested = true;
}

static void id_new_data_cb(queue_cb_t *qcb, link_t *link){
    // dereference args
    imape_data_t *id = CONTAINER_OF(qcb, imape_data_t, write_qcb);
    event_t *ev = CONTAINER_OF(link, event_t, link);
    // mark session (upref happened already, in prewait)
    ev->session = id->session;
    id->write_requested = false;
    // use it!
    id->write_ev = ev;
    imape_data_write_stuff(id);
}

// write as many commands or responses as possible before returning
static void imape_data_write_stuff(imape_data_t *id){
    derr_t e = E_OK;

    // make sure there's something to be written
    while(!link_list_isempty(&id->unwritten)){

        // get something to write to
        if(id->write_ev == NULL && !id->write_requested){
            queue_cb_set(&id->write_qcb, id_prewait_cb, id_new_data_cb);
            link_t *link = queue_pop_first_cb(&id->imape->write_events,
                    &id->write_qcb);
            // do we need to set the session?
            if(link != NULL){
                id->write_ev = CONTAINER_OF(link, event_t, link);
                id->write_ev->session = id->session;
                id->ref_up(id->session, IMAPE_REF_WRITE);
            }
        }

        if(id->write_ev == NULL) return;

        // write some of whatever it is we need to write
        link_t *link = id->unwritten.next;
        event_t *ev_in = CONTAINER_OF(link, event_t, link);
        imap_event_t *imap_ev = CONTAINER_OF(ev_in, imap_event_t, ev);
        size_t want = 0;
        id->write_ev->buffer.len = 0;
        if(id->control->is_client){
            // invalid syntax here means we have an internal bug
            NOFAIL_GO(&e, E_PARAM, imap_cmd_write(imap_ev->arg.cmd,
                        &id->write_ev->buffer, &id->write_skip, &want,
                        &id->control->exts), fail);
        }else{
            NOFAIL_GO(&e, E_PARAM, imap_resp_write(imap_ev->arg.resp,
                        &id->write_ev->buffer, &id->write_skip, &want,
                        &id->control->exts), fail);
        }

        // did we finish writing that thing?
        if(want == 0){
            id->write_skip = 0;
            link_remove(link);
            // call ev_in->returner when this write_ev returns as WRITE_DONE
            id->write_ev->prev = ev_in;
        }

        // send the write event
        id->write_ev->ev_type = EV_WRITE;
        {
            LOG_INFO("send: %x", FD(&id->write_ev->buffer));
        }
        id->imape->upstream->pass_event(id->imape->upstream, id->write_ev);

        // done with event
        id->write_ev = NULL;
    }

    return;

fail:
    id->session->close(id->session, e);
    PASSED(e);
    imape_data_onthread_close(id);
    return;
}

static void imap_ev_returner(event_t *ev){
    // downref session
    imape_data_t *id = ev->returner_arg;
    id->ref_down(id->session, IMAPE_REF_READ);

    // free imap object
    imap_event_t *imap_ev = CONTAINER_OF(ev, imap_event_t, ev);
    switch(imap_ev->type){
        case IMAP_EVENT_TYPE_CMD:
            imap_cmd_free(imap_ev->arg.cmd);
            break;
        case IMAP_EVENT_TYPE_RESP:
            imap_resp_free(imap_ev->arg.resp);
            break;
    }

    // free backing memory
    free(imap_ev);
}

static derr_t imap_event_new(imap_event_t **out, imap_event_type_e type,
        imap_event_arg_u arg, imape_data_t *id){
    derr_t e = E_OK;

    imap_event_t *imap_ev = malloc(sizeof(*imap_ev));
    if(!imap_ev) ORIG(&e, E_NOMEM, "nomem");
    *imap_ev = (imap_event_t){
        .arg = arg,
        .type = type,
    };

    event_prep(&imap_ev->ev, imap_ev_returner, id);
    imap_ev->ev.session = id->session;
    id->ref_up(id->session, IMAPE_REF_READ);
    imap_ev->ev.ev_type = EV_READ;

    *out = imap_ev;

    return e;
}

// parser callback for servers

static void cmd_cb(void *cb_data, derr_t error, imap_cmd_t *cmd){
    imape_data_t *id = cb_data;
    derr_t e = E_OK;

    PROP_VAR_GO(&e, &error, fail);

    // prepare the imap event
    imap_event_t *imap_ev;
    imap_event_arg_u arg = { .cmd = cmd };

    PROP_GO(&e, imap_event_new(&imap_ev, IMAP_EVENT_TYPE_CMD, arg, id), fail);

    // pass the event downstream
    id->downstream->pass_event(id->downstream, &imap_ev->ev);

    return;

fail:
    imap_cmd_free(cmd);
    id->session->close(id->session, e);
    PASSED(e);
}

static void need_plus_cb(void *cb_data){
    imape_data_t *id = cb_data;
    derr_t e = E_OK;

    // prepare the imap command
    imap_cmd_arg_t cmd_arg = {0};
    imap_cmd_t *cmd = imap_cmd_new(&e, NULL, IMAP_CMD_PLUS, cmd_arg);
    CHECK_GO(&e, fail);

    // prepare the imap event
    imap_event_t *imap_ev;
    imap_event_arg_u arg = { .cmd = cmd };

    PROP_GO(&e, imap_event_new(&imap_ev, IMAP_EVENT_TYPE_CMD, arg, id), fail);

    // pass the event downstream
    id->downstream->pass_event(id->downstream, &imap_ev->ev);

    return;

fail:
    id->session->close(id->session, e);
    PASSED(e);
};

static imap_parser_cb_t imape_parser_cmd_cb = {
    .cmd=cmd_cb, .need_plus=need_plus_cb,
};

// parser callback for clients

static void resp_cb(void *cb_data, derr_t error, imap_resp_t *resp){
    imape_data_t *id = cb_data;
    derr_t e = E_OK;

    PROP_VAR_GO(&e, &error, fail);

    // prepare the imap event
    imap_event_t *imap_ev;
    imap_event_arg_u arg = { .resp = resp };

    PROP_GO(&e, imap_event_new(&imap_ev, IMAP_EVENT_TYPE_RESP, arg, id), fail);

    // pass the event downstream
    id->downstream->pass_event(id->downstream, &imap_ev->ev);

    return;

fail:
    imap_resp_free(resp);
    id->session->close(id->session, e);
    PASSED(e);
}
static imap_parser_cb_t imape_parser_resp_cb = {.resp=resp_cb};

void imape_data_prestart(imape_data_t *id, imape_t *imape, session_t *session,
        ref_fn_t ref_up, ref_fn_t ref_down, imape_control_i *control,
        engine_t *downstream){
    *id = (imape_data_t){
        .imape = imape,
        .session = session,
        .ref_up = ref_up,
        .ref_down = ref_down,
        .control = control,
        .downstream = downstream,
    };
}

void imape_data_start(imape_data_t *id){
    // prepare the starting event
    event_prep(&id->start_ev, NULL, NULL);
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

    link_init(&id->unwritten);
    queue_cb_prep(&id->write_qcb);

    bool is_client = id->control->is_client;

    // choose the right parser callback
    imap_parser_cb_t parser_cb =
        is_client ? imape_parser_resp_cb : imape_parser_cmd_cb;

    PROP_GO(&e,
        imap_reader_init(
            &id->reader, &id->control->exts, parser_cb, id, is_client
        ),
    fail);

    // lifetime reference
    id->ref_up(id->session, IMAPE_REF_LIFETIME);

    return;

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
    event_prep(&id->close_ev, NULL, NULL);
    id->close_ev.ev_type = EV_SESSION_CLOSE;
    id->close_ev.buffer = (dstr_t){0};
    id->close_ev.session = id->session;
    id->ref_up(id->session, IMAPE_REF_CLOSE_EVENT);

    // pass the closing event
    id->imape->engine.pass_event(&id->imape->engine, &id->close_ev);
}

static void imape_data_onthread_close(imape_data_t *id){
    // no double closing.  This could happen if imape_data_start failed.
    if(id->data_state == DATA_STATE_CLOSED) return;
    // safe from PREINIT state
    bool exit_early = (id->data_state == DATA_STATE_PREINIT);
    // set state
    id->data_state = DATA_STATE_CLOSED;
    if(exit_early) return;

    // let go of write_ev
    if(id->write_ev){
        imape_return_write_event(id->write_ev);
    }

    // release unwritten events
    link_t *link;
    while((link = link_list_pop_first(&id->unwritten)) != NULL){
        event_t *ev = CONTAINER_OF(link, event_t, link);
        ev->returner(ev);
    }

    // close reader
    imap_reader_free(&id->reader);

    // lifetime reference
    id->ref_down(id->session, IMAPE_REF_LIFETIME);
}

DSTR_STATIC(imape_ref_read_dstr, "read");
DSTR_STATIC(imape_ref_write_dstr, "write");
DSTR_STATIC(imape_ref_start_event_dstr, "start_event");
DSTR_STATIC(imape_ref_close_event_dstr, "close_event");
DSTR_STATIC(imape_ref_lifetime_dstr, "lifetime");
DSTR_STATIC(imape_ref_unknown_dstr, "unknown");

dstr_t *imape_ref_reason_to_dstr(enum imape_ref_reason_t reason){
    switch(reason){
        case IMAPE_REF_READ: return &imape_ref_read_dstr; break;
        case IMAPE_REF_WRITE: return &imape_ref_write_dstr; break;
        case IMAPE_REF_START_EVENT: return &imape_ref_start_event_dstr; break;
        case IMAPE_REF_CLOSE_EVENT: return &imape_ref_close_event_dstr; break;
        case IMAPE_REF_LIFETIME: return &imape_ref_lifetime_dstr; break;
        case IMAPE_REF_MAXIMUM:
        default: return &imape_ref_unknown_dstr;
    }
}
