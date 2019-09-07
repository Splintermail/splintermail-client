#include <networking.h>
#include <logger.h>

#include "fake_engine.h"
#include "fake_imap_logic.h"

DSTR_STATIC(fe_ref_read_dstr, "read");
DSTR_STATIC(fe_ref_write_dstr, "write");
DSTR_STATIC(fe_ref_start_dstr, "start_protect");
DSTR_STATIC(fe_ref_close_dstr, "close_protect");
DSTR_STATIC(fe_ref_unknown_dstr, "unknown");

static dstr_t *fe_ref_reason_to_dstr(enum fake_engine_ref_reason_t reason){
    switch(reason){
        case FAKE_ENGINE_REF_READ: return &fe_ref_read_dstr; break;
        case FAKE_ENGINE_REF_WRITE: return &fe_ref_write_dstr; break;
        case FAKE_ENGINE_REF_START_PROTECT: return &fe_ref_start_dstr; break;
        case FAKE_ENGINE_REF_CLOSE_PROTECT: return &fe_ref_close_dstr; break;
        default: return &fe_ref_unknown_dstr; break;
    }
}

/////// reader-writer thread stuff

void *reader_writer_thread(void *arg){
    reader_writer_context_t *ctx = arg;
    derr_t e = E_OK;

    // allocate ssl context
    ssl_context_t ssl_ctx;
    if(ctx->use_tls){
        PROP_GO(&e, ssl_context_new_client(&ssl_ctx), fail);
    }

    // generate all the buffers we are going to send
    LIST(dstr_t) out_bufs;
    PROP_GO(&e, LIST_NEW(dstr_t, &out_bufs, ctx->writes_per_thread), fail_ssl);
    for(size_t i = 0; i < ctx->writes_per_thread; i++){
        dstr_t temp;
        // allocate the dstr in the list
        PROP_GO(&e, dstr_new(&temp, 64), free_out_bufs);
        // write something into the buffer
        PROP_GO(&e, FMT(&temp, "%x:%x\n", FU(ctx->thread_id), FU(i)), free_temp);
        // add it to the list
        PROP_GO(&e, LIST_APPEND(dstr_t, &out_bufs, temp), free_temp);
        continue;
    free_temp:
        dstr_free(&temp);
        goto free_out_bufs;
    }

    // check if we are the last thread ready
    pthread_mutex_lock(ctx->mutex);
    (*ctx->threads_ready)++;
    // last thread signals the others
    if(*ctx->threads_ready == ctx->num_threads){
        pthread_cond_broadcast(ctx->cond);
    }
    // other threads wait for the last one
    else{
        while(*ctx->threads_ready < ctx->num_threads){
            pthread_cond_wait(ctx->cond, ctx->mutex);
        }
    }
    pthread_mutex_unlock(ctx->mutex);

    // open a connection
    connection_t conn;
    if(ctx->use_tls){
        connection_new_ssl(&conn, &ssl_ctx, "127.0.0.1", ctx->listen_port);
    }else{
        connection_new(&conn, "127.0.0.1", ctx->listen_port);
    }
    // write all of the buffers
    for(size_t i = 0; i < out_bufs.len; i++){
        PROP_GO(&e, connection_write(&conn, &out_bufs.data[i]), close_conn);
    }
    // read all of the buffers into a single place
    dstr_t recvd;
    PROP_GO(&e, dstr_new(&recvd, 8192), close_conn);
    while( dstr_count(&recvd, &DSTR_LIT("\n")) < out_bufs.len){
        PROP_GO(&e, connection_read(&conn, &recvd, NULL), free_recvd);
    }
    // now compare the buffers
    size_t compared = 0;
    for(size_t i = 0; i < out_bufs.len; i++){
        // cmp is the section of leftovers that
        dstr_t cmp = dstr_sub(&recvd, compared,
                              compared + out_bufs.data[i].len);
        if(dstr_cmp(&cmp, &out_bufs.data[i]) != 0)
            ORIG_GO(&e, E_VALUE, "received bad response!", free_recvd);
        compared += out_bufs.data[i].len;
    }

    // done!

free_recvd:
    dstr_free(&recvd);
close_conn:
    connection_close(&conn);
free_out_bufs:
    for(size_t i = 0; i < out_bufs.len; i++){
        dstr_free(&out_bufs.data[i]);
    }
    LIST_FREE(dstr_t, &out_bufs);
fail_ssl:
    if(ctx->use_tls){
        ssl_context_free(&ssl_ctx);
    }
fail:
    ctx->error = e;
    return NULL;
}

/////// cb_reader_writer stuff

static event_t *cb_reader_writer_send(cb_reader_writer_t *cbrw){
    derr_t e = E_OK;

    // check for completion criteria
    if(cbrw->nrecvd == cbrw->nwrites){
        // TODO
        fake_session_close(&cbrw->fake_session->session, E_OK);
        return NULL;
    }

    // reset buffers
    cbrw->out.len = 0;
    cbrw->in.len = 0;

    PROP_GO(&e, FMT(&cbrw->out, "cbrw%x:%x/%x\n", FU(cbrw->id),
                 FU(cbrw->nrecvd + 1), FU(cbrw->nwrites)), fail);

    // copy out into a write event (which will be freed, but not by us)
    event_t *ev = malloc(sizeof(*ev));
    if(!ev) ORIG_GO(&e, E_NOMEM, "no memory", fail);

    event_prep(ev);
    PROP_GO(&e, dstr_new(&ev->buffer, cbrw->out.len), fail_ev);

    dstr_copy(&cbrw->out, &ev->buffer);

    ev->session = &cbrw->fake_session->session;
    fake_session_ref_up_test(ev->session, FAKE_ENGINE_REF_WRITE);
    ev->ev_type = EV_WRITE;
    return ev;

fail_ev:
    free(ev);
fail:
    fake_session_close(&cbrw->fake_session->session, SPLIT(e));
    cbrw->error = e;
    return NULL;
}

event_t *cb_reader_writer_init(cb_reader_writer_t *cbrw, size_t id,
                               size_t nwrites, fake_session_t *s){
    derr_t e = E_OK;
    *cbrw = (cb_reader_writer_t){0};

    // allocate the buffers
    PROP_GO(&e, dstr_new(&cbrw->out, 256), fail);
    PROP_GO(&e, dstr_new(&cbrw->in, 256), free_out);

    cbrw->fake_session = s;
    cbrw->id = id;
    cbrw->nwrites = nwrites;

    // start sending the first message
    event_t *ev = cb_reader_writer_send(cbrw);
    if(!ev) goto free_in;
    return ev;

free_in:
    dstr_free(&cbrw->in);
free_out:
    dstr_free(&cbrw->out);
fail:
    DROP_VAR(&e);
    return NULL;
}

void cb_reader_writer_free(cb_reader_writer_t *cbrw){
    dstr_free(&cbrw->in);
    dstr_free(&cbrw->out);
}

event_t *cb_reader_writer_read(cb_reader_writer_t *cbrw, dstr_t *buffer){
    // if we are exiting, just ignore this
    if(cbrw->error.type != E_NONE) return NULL;

    derr_t e = E_OK;

    // append the buffer to the input
    PROP_GO(&e, dstr_append(&cbrw->in, buffer), fail);

    // make sure we have the full line
    if(dstr_count(&cbrw->in, &DSTR_LIT("\n")) == 0){
        return NULL;
    }

    // compare buffers
    if(dstr_cmp(&cbrw->in, &cbrw->out) != 0){
        TRACE(&e, "cbrw got a bad echo: \"%x\" (expected \"%x\")\n",
                FD(&cbrw->in), FD(&cbrw->out));
        ORIG_GO(&e, E_VALUE, "bad echo", fail);
    }

    cbrw->nrecvd++;

    // send another
    return cb_reader_writer_send(cbrw);

fail:
    fake_session_close(&cbrw->fake_session->session, SPLIT(e));
    cbrw->error = e;
    return NULL;
}

/////// fake session stuff

static void fake_session_ref_up(session_t *session){
    fake_session_t *s = CONTAINER_OF(session, fake_session_t, session);
    pthread_mutex_lock(&s->mutex);
    if(s->refs == 0){
        LOG_DEBUG("necromatic ref_up detected!\n");
    }
    ++s->refs;
    // LOG_DEBUG("ref_up (%x)\n", FI(s->refs));
    pthread_mutex_unlock(&s->mutex);
}

static void fake_session_ref_down(session_t *session){
    fake_session_t *s = CONTAINER_OF(session, fake_session_t, session);
    pthread_mutex_lock(&s->mutex);
    int refs = --s->refs;
    // LOG_DEBUG("ref_down (%x)\n", FI(s->refs));
    pthread_mutex_unlock(&s->mutex);

    if(refs > 0) return;

    // finish closing the imape_data
    if(s->pipeline->imape){
        imape_data_postclose(&s->imape_data);
    }

    // now the session is no longer in use, we call the session manager hook
    s->session_destroyed(s, s->error);
    PASSED(s->error);

    // free the session
    pthread_mutex_destroy(&s->mutex);
    free(s);
}

// only called by loop thread
void fake_session_ref_up_loop(session_t *session, int reason){
    fake_session_t *s = CONTAINER_OF(session, fake_session_t, session);
    s->loop_refs[reason]++;
    fake_session_ref_up(session);
}
void fake_session_ref_down_loop(session_t *session, int reason){
    fake_session_t *s = CONTAINER_OF(session, fake_session_t, session);
    s->loop_refs[reason]--;
    for(size_t i = 0; i < LOOP_REF_MAXIMUM; i++){
        if(s->loop_refs[i] < 0){
            LOG_ERROR("negative loop ref of type %x!\n",
                      FD(loop_ref_reason_to_dstr(i)));
        }
    }
    fake_session_ref_down(session);
}

// only called by tlse thread
void fake_session_ref_up_tlse(session_t *session, int reason){
    fake_session_t *s = CONTAINER_OF(session, fake_session_t, session);
    s->tlse_refs[reason]++;
    fake_session_ref_up(session);
}
void fake_session_ref_down_tlse(session_t *session, int reason){
    fake_session_t *s = CONTAINER_OF(session, fake_session_t, session);
    s->tlse_refs[reason]--;
    for(size_t i = 0; i < TLSE_REF_MAXIMUM; i++){
        if(s->tlse_refs[i] < 0){
            LOG_ERROR("negative tlse ref of type %x!\n",
                      FD(tlse_ref_reason_to_dstr(i)));
        }
    }
    fake_session_ref_down(session);
}

// only called by imape thread
void fake_session_ref_up_imape(session_t *session, int reason){
    fake_session_t *s = CONTAINER_OF(session, fake_session_t, session);
    pthread_mutex_lock(&s->mutex);
    s->imape_refs[reason]++;
    pthread_mutex_unlock(&s->mutex);
    fake_session_ref_up(session);
}
void fake_session_ref_down_imape(session_t *session, int reason){
    fake_session_t *s = CONTAINER_OF(session, fake_session_t, session);
    pthread_mutex_lock(&s->mutex);
    s->imape_refs[reason]--;
    for(size_t i = 0; i < IMAPE_REF_MAXIMUM; i++){
        if(s->imape_refs[i] < 0){
            LOG_ERROR("negative imape ref of type %x!\n",
                      FD(imape_ref_reason_to_dstr(i)));
        }
    }
    pthread_mutex_unlock(&s->mutex);
    fake_session_ref_down(session);
}

// only for use on test thread (fake engine and callbacks)
void fake_session_ref_up_test(session_t *session, int reason){
    fake_session_t *s = CONTAINER_OF(session, fake_session_t, session);
    s->test_refs[reason]++;
    fake_session_ref_up(session);
}
void fake_session_ref_down_test(session_t *session, int reason){
    fake_session_t *s = CONTAINER_OF(session, fake_session_t, session);
    s->test_refs[reason]--;
    for(size_t i = 0; i < FAKE_ENGINE_REF_MAXIMUM; i++){
        if(s->test_refs[i] < 0){
            LOG_ERROR("negative test ref of type %x!\n",
                      FD(fe_ref_reason_to_dstr(i)));
        }
    }
    fake_session_ref_down(session);
}


// to allocate new sessions (when loop.c only know about a single child struct)
static derr_t fake_session_do_alloc(fake_session_t **sptr, fake_pipeline_t *fp,
        ssl_context_t* ssl_ctx, const char *host, const char* service){
    derr_t e = E_OK;

    bool upwards = (host != NULL);

    // allocate the struct
    fake_session_t *s = malloc(sizeof(*s));
    if(!s) ORIG(&e, E_NOMEM, "no mem");
    *s = (fake_session_t){0};

    // session prestart
    pthread_mutex_init(&s->mutex, NULL);
    s->refs = 1;
    s->test_refs[FAKE_ENGINE_REF_START_PROTECT] = 1;
    s->closed = false;
    s->pipeline = fp;
    s->session.ld = &s->loop_data;
    s->session.td = &s->tlse_data;
    s->session.id = &s->imape_data;
    s->session.close = fake_session_close;

    // per-engine prestart
    if(s->pipeline->loop){
        loop_data_prestart(&s->loop_data, s->pipeline->loop, &s->session, host,
                service, fake_session_ref_up_loop, fake_session_ref_down_loop);
    }
    if(s->pipeline->tlse){
        tlse_data_prestart(&s->tlse_data, s->pipeline->tlse, &s->session,
                fake_session_ref_up_tlse, fake_session_ref_down_tlse, ssl_ctx,
                upwards);
    }
    if(s->pipeline->imape){
        imape_data_prestart(&s->imape_data, s->pipeline->imape, &s->session,
                upwards, fake_session_ref_up_imape,
                fake_session_ref_down_imape,
                fake_imap_logic_init, NULL);
    }

    *sptr = s;

    return e;
}

void fake_session_start(fake_session_t *s){
    // per-engine start
    if(s->pipeline->loop){
        loop_data_start(&s->loop_data);
    }
    if(s->pipeline->tlse){
        tlse_data_start(&s->tlse_data);
    }
    if(s->pipeline->imape){
        imape_data_start(&s->imape_data);
    }
    fake_session_ref_down_test(&s->session, FAKE_ENGINE_REF_START_PROTECT);
}

// to allocate new sessions (when loop.c only know about a single child struct)
derr_t fake_session_alloc_accept(fake_session_t **sptr, fake_pipeline_t *fp,
                                 ssl_context_t* ssl_ctx){
    derr_t e = E_OK;
    PROP(&e, fake_session_do_alloc(sptr, fp, ssl_ctx, NULL, NULL) );
    return e;
}

derr_t fake_session_alloc_connect(fake_session_t **sptr, fake_pipeline_t *fp,
         ssl_context_t* ssl_ctx, const char *host, const char *service){
    derr_t e = E_OK;
    PROP(&e, fake_session_do_alloc(sptr, fp, ssl_ctx, host, service) );
    return e;
}

void fake_session_close(session_t *session, derr_t error){
    fake_session_t *s = CONTAINER_OF(session, fake_session_t, session);
    MERGE_VAR(&s->error, &error, "session_close error");
    pthread_mutex_lock(&s->mutex);
    bool do_close = !s->closed;
    s->closed = true;
    pthread_mutex_unlock(&s->mutex);

    if(!do_close) return;

    /* make sure every engine_data has a chance to pass a close event; a slow
       session without standing references must be protected */
    fake_session_ref_up_test(&s->session, FAKE_ENGINE_REF_CLOSE_PROTECT);
    if(s->pipeline->loop){
        loop_data_close(&s->loop_data);
    }
    if(s->pipeline->tlse){
        tlse_data_close(&s->tlse_data);
    }
    if(s->pipeline->imape){
        imape_data_close(&s->imape_data);
    }
    fake_session_ref_down_test(&s->session, FAKE_ENGINE_REF_CLOSE_PROTECT);
}

/////// fake engine stuff

static void fake_engine_pass_event(engine_t *engine, event_t *ev){
    fake_engine_t *fake_engine = CONTAINER_OF(engine, fake_engine_t, engine);
    queue_append(&fake_engine->event_q, &ev->link);
}

derr_t fake_engine_init(fake_engine_t *fake_engine){
    derr_t e = E_OK;
    PROP(&e, queue_init(&fake_engine->event_q) );
    fake_engine->engine.pass_event = fake_engine_pass_event;
    return e;
}

void fake_engine_free(fake_engine_t *fake_engine){
    queue_free(&fake_engine->event_q);
}

derr_t fake_engine_run(fake_engine_t *fe, engine_t *upstream,
        void (*handle_read)(void*, event_t*),
        void (*handle_write_done)(void*, event_t*), bool (*quit_ready)(void*),
        void *cb_data){
    derr_t e = E_OK;
    // process incoming events from the upstream engine
    event_t *quit_ev = NULL;
    link_t *link;
    while((link = queue_pop_first(&fe->event_q, true))){
        event_t *ev = CONTAINER_OF(link, event_t, link);
        switch(ev->ev_type){
            case EV_READ:
                //LOG_ERROR("got read\n");
                // check for EOF
                handle_read(cb_data, ev);
                // return buffer
                ev->ev_type = EV_READ_DONE;
                upstream->pass_event(upstream, ev);
                break;
            case EV_QUIT_DOWN:
                // check if we need to wait for write events to be returned
                if(quit_ready(cb_data)){
                    ev->ev_type = EV_QUIT_UP;
                    upstream->pass_event(upstream, ev);
                    return e;
                }else{
                    quit_ev = ev;
                }
                break;
            case EV_WRITE_DONE:
                handle_write_done(cb_data, ev);
                // check for quitting condition
                if(quit_ev && quit_ready(cb_data)){
                    quit_ev->ev_type = EV_QUIT_UP;
                    upstream->pass_event(upstream, quit_ev);
                    return e;
                }
                break;
            // other events should not happen
            case EV_READ_DONE:
            case EV_WRITE:
            case EV_QUIT_UP:
            default:
                ORIG(&e, E_VALUE,
                        "unexpected event type from upstream engine\n");
        }
    }
    ORIG(&e, E_VALUE, "unexpected \t from fake_engine");
}
