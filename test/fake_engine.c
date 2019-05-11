#include <networking.h>
#include <logger.h>

#include "fake_engine.h"

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
    derr_t error;

    // allocate ssl context
    ssl_context_t ssl_ctx;
    if(ctx->use_tls){
        PROP_GO( ssl_context_new_client(&ssl_ctx), fail);
    }

    // generate all the buffers we are going to send
    LIST(dstr_t) out_bufs;
    PROP_GO( LIST_NEW(dstr_t, &out_bufs, ctx->writes_per_thread), fail_ssl);
    for(size_t i = 0; i < ctx->writes_per_thread; i++){
        dstr_t temp;
        // allocate the dstr in the list
        PROP_GO( dstr_new(&temp, 64), free_out_bufs);
        // write something into the buffer
        PROP_GO( FMT(&temp, "%x:%x\n", FU(ctx->thread_id), FU(i)), free_temp);
        // add it to the list
        PROP_GO( LIST_APPEND(dstr_t, &out_bufs, temp), free_temp);
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
        PROP_GO( connection_write(&conn, &out_bufs.data[i]), close_conn);
    }
    // read all of the buffers into a single place
    dstr_t recvd;
    PROP_GO( dstr_new(&recvd, 8192), close_conn);
    while( dstr_count(&recvd, &DSTR_LIT("\n")) < out_bufs.len){
        PROP_GO( connection_read(&conn, &recvd, NULL), free_recvd);
    }
    // now compare the buffers
    size_t compared = 0;
    for(size_t i = 0; i < out_bufs.len; i++){
        // cmp is the section of leftovers that
        dstr_t cmp = dstr_sub(&recvd, compared,
                              compared + out_bufs.data[i].len);
        if(dstr_cmp(&cmp, &out_bufs.data[i]) != 0)
            ORIG_GO(E_VALUE, "received bad response!", free_recvd);
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
    ctx->error = error;
    return NULL;
}

/////// cb_reader_writer stuff

static event_t *cb_reader_writer_send(cb_reader_writer_t *cbrw){
    derr_t error;

    // check for completion criteria
    if(cbrw->nrecvd == cbrw->nwrites){
        // TODO
        fake_session_close(cbrw->session, E_OK);
        return NULL;
    }

    // reset buffers
    cbrw->out.len = 0;
    cbrw->in.len = 0;

    PROP_GO( FMT(&cbrw->out, "cbrw%x:%x/%x\n", FU(cbrw->id),
                 FU(cbrw->nrecvd + 1), FU(cbrw->nwrites)), fail);

    // copy out into a write event (which will be freed, but not by us)
    event_t *ev = malloc(sizeof(*ev));
    if(!ev) ORIG_GO(E_NOMEM, "no memory", fail);

    event_prep(ev, NULL);
    PROP_GO( dstr_new(&ev->buffer, cbrw->out.len), fail_ev);

    dstr_copy(&cbrw->out, &ev->buffer);

    ev->session = cbrw->session;
    fake_session_ref_up_test(ev->session, FAKE_ENGINE_REF_WRITE);
    ev->ev_type = EV_WRITE;
    return ev;

fail_ev:
    free(ev);
fail:
    fake_session_close(cbrw->session, error);
    cbrw->error = error;
    return NULL;
}

event_t *cb_reader_writer_init(cb_reader_writer_t *cbrw, size_t id,
                               size_t nwrites, fake_session_t *s){
    derr_t error;
    *cbrw = (cb_reader_writer_t){0};

    // allocate the buffers
    PROP_GO( dstr_new(&cbrw->out, 256), fail);
    PROP_GO( dstr_new(&cbrw->in, 256), free_out);

    cbrw->session = s;
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
    return NULL;
}

void cb_reader_writer_free(cb_reader_writer_t *cbrw){
    dstr_free(&cbrw->in);
    dstr_free(&cbrw->out);
}

event_t *cb_reader_writer_read(cb_reader_writer_t *cbrw, dstr_t *buffer){
    // if we are exiting, just ignore this
    if(cbrw->error) return NULL;

    derr_t error;

    // append the buffer to the input
    PROP_GO( dstr_append(&cbrw->in, buffer), fail);

    // make sure we have the full line
    if(dstr_count(&cbrw->in, &DSTR_LIT("\n")) == 0){
        return NULL;
    }

    // compare buffers
    if(dstr_cmp(&cbrw->in, &cbrw->out) != 0){
        LOG_ERROR("cbrw got a bad echo: \"%x\" (expected \"%x\")\n",
                  FD(&cbrw->in), FD(&cbrw->out));
        ORIG_GO(E_VALUE, "bad echo", fail);
    }

    cbrw->nrecvd++;

    // send another
    return cb_reader_writer_send(cbrw);

fail:
    fake_session_close(cbrw->session, error);
    cbrw->error = error;
    return NULL;
}

/////// fake session stuff

session_iface_t fake_session_iface_tlse = {
    .ref_up = fake_session_ref_up_tlse,
    .ref_down = fake_session_ref_down_tlse,
    .close = fake_session_close,
};

session_iface_t fake_session_iface_loop = {
    .ref_up = fake_session_ref_up_loop,
    .ref_down = fake_session_ref_down_loop,
    .close = fake_session_close,
};

static void fake_session_ref_up(void *session){
    fake_session_t *s = session;
    pthread_mutex_lock(&s->mutex);
    ++s->refs;
    // LOG_DEBUG("ref_up (%x)\n", FI(s->refs));
    pthread_mutex_unlock(&s->mutex);
}

static void fake_session_ref_down(void *session){
    fake_session_t *s = session;
    pthread_mutex_lock(&s->mutex);
    int refs = --s->refs;
    // LOG_DEBUG("ref_down (%x)\n", FI(s->refs));
    pthread_mutex_unlock(&s->mutex);

    if(refs > 0) return;

    // now the session is no longer in use, we call the session manager hook
    if(s->session_destroyed){
        s->session_destroyed(s, s->error);
    }

    // free the session
    pthread_mutex_destroy(&s->mutex);
    free(s);
}

// only called by loop thread
void fake_session_ref_up_loop(void *session, int reason){
    fake_session_t *s = session;
    s->loop_refs[reason]++;
    fake_session_ref_up(session);
}
void fake_session_ref_down_loop(void *session, int reason){
    fake_session_t *s = session;
    s->loop_refs[reason]--;
    for(size_t i = 0; i < LOOP_REF_MAXIMUM; i++){
        if(s->loop_refs[i] < 0){
            LOG_ERROR("negative loop ref of type %x!\n",
                      FD(loop_ref_reason_to_dstr(reason)));
        }
    }
    fake_session_ref_down(session);
}

// only called by tlse thread
void fake_session_ref_up_tlse(void *session, int reason){
    fake_session_t *s = session;
    s->tlse_refs[reason]++;
    fake_session_ref_up(session);
}
void fake_session_ref_down_tlse(void *session, int reason){
    fake_session_t *s = session;
    s->tlse_refs[reason]--;
    for(size_t i = 0; i < TLSE_REF_MAXIMUM; i++){
        if(s->tlse_refs[i] < 0){
            LOG_ERROR("negative tlse ref of type %x!\n",
                      FD(tlse_ref_reason_to_dstr(reason)));
        }
    }
    fake_session_ref_down(session);
}

// only for use on test thread (fake engine and callbacks)
void fake_session_ref_up_test(void *session, int reason){
    fake_session_t *s = session;
    s->test_refs[reason]++;
    fake_session_ref_up(session);
}
void fake_session_ref_down_test(void *session, int reason){
    fake_session_t *s = session;
    s->test_refs[reason]--;
    for(size_t i = 0; i < FAKE_ENGINE_REF_MAXIMUM; i++){
        if(s->test_refs[i] < 0){
            LOG_ERROR("negative test ref of type %x!\n",
                      FD(fe_ref_reason_to_dstr(reason)));
        }
    }
    fake_session_ref_down(session);
}


// to allocate new sessions (when loop.c only know about a single child struct)
static derr_t fake_session_do_alloc(void **sptr, void *fake_pipeline,
                                    ssl_context_t* ssl_ctx){
    // allocate the struct
    fake_session_t *s = malloc(sizeof(*s));
    if(!s) ORIG(E_NOMEM, "no mem");
    *s = (fake_session_t){0};

    // prepare the refs
    pthread_mutex_init(&s->mutex, NULL);
    s->refs = 1;
    s->test_refs[FAKE_ENGINE_REF_START_PROTECT] = 1;
    s->closed = false;

    // prepare the session callbacks and such
    s->pipeline = fake_pipeline;
    s->ssl_ctx = ssl_ctx;

    // get an id
    pthread_mutex_lock(s->pipeline->mutex);
    s->id = s->pipeline->nsessions++;
    pthread_mutex_unlock(s->pipeline->mutex);

    // init the engine_data elements
    if(s->pipeline->loop){
        loop_data_start(&s->loop_data, s->pipeline->loop, s);
    }
    if(s->pipeline->tlse){
        tlse_data_start(&s->tlse_data, s->pipeline->tlse, s);
    }
    fake_session_ref_down_test(s, FAKE_ENGINE_REF_START_PROTECT);
    *sptr = s;

    return E_OK;
}

// to allocate new sessions (when loop.c only know about a single child struct)
derr_t fake_session_alloc_accept(void **sptr, void *fake_pipeline,
                                 ssl_context_t* ssl_ctx){
    PROP( fake_session_do_alloc(sptr, fake_pipeline, ssl_ctx) );

    // get the new session
    fake_session_t *s = *sptr;

    // now the session is allocated, we call the session manager hook
    if(s->pipeline->fake_session_accepted){
        s->pipeline->fake_session_accepted(s);
    }

    return E_OK;
}

derr_t fake_session_alloc_connect(void **sptr, void *fake_pipeline,
                                  ssl_context_t* ssl_ctx){
    PROP( fake_session_do_alloc(sptr, fake_pipeline, ssl_ctx) );
    return E_OK;
}

void fake_session_close(void *session, derr_t error){
    fake_session_t *s = session;
    if(!s->error) s->error = error;
    pthread_mutex_lock(&s->mutex);
    bool do_close = !s->closed;
    s->closed = true;
    pthread_mutex_unlock(&s->mutex);

    if(!do_close) return;

    /* make sure every engine_data has a chance to pass a close event; a slow
       session without standing references must be protected */
    fake_session_ref_up_test(s, FAKE_ENGINE_REF_CLOSE_PROTECT);
    if(s->pipeline->loop){
        loop_data_close(&s->loop_data, s->pipeline->loop, s);
    }
    if(s->pipeline->tlse){
        tlse_data_close(&s->tlse_data, s->pipeline->tlse, s);
    }
    fake_session_ref_down_test(s, FAKE_ENGINE_REF_CLOSE_PROTECT);
}

loop_data_t *fake_session_get_loop_data(void *session){
    fake_session_t *s = session;
    return &s->loop_data;
}

tlse_data_t *fake_session_get_tlse_data(void *session){
    fake_session_t *s = session;
    return &s->tlse_data;
}

ssl_context_t *fake_session_get_ssl_ctx(void *session){
    fake_session_t *s = session;
    return s->ssl_ctx;
}

bool fake_session_get_upwards(void *session){
    fake_session_t *s = session;
    // right now the tests only accept()
    (void)s;
    return false;
}

/////// fake engine stuff

derr_t fake_engine_init(fake_engine_t *fake_engine){
    PROP( queue_init(&fake_engine->event_q) );
    return E_OK;
}

void fake_engine_free(fake_engine_t *fake_engine){
    queue_free(&fake_engine->event_q);
}

void fake_engine_pass_event(void *engine, event_t *ev){
    fake_engine_t *fake_engine = engine;
    queue_append(&fake_engine->event_q, &ev->qe);
}

bool fake_engine_run(fake_engine_t *fe,
                     event_passer_t pass_up, void *upstream,
                     void (*handle_read)(void*, event_t*),
                     void (*handle_write_done)(void*, event_t*),
                     bool (*quit_ready)(void*),
                     void *cb_data){
    // process incoming events from the upstream engine
    event_t *ev;
    event_t *quit_ev = NULL;
    while(true){
        if(!(ev = queue_pop_first(&fe->event_q, true))) break;
        switch(ev->ev_type){
            case EV_READ:
                //LOG_ERROR("got read\n");
                // check for EOF
                handle_read(cb_data, ev);
                // return buffer
                ev->ev_type = EV_READ_DONE;
                pass_up(upstream, ev);
                break;
            case EV_QUIT_DOWN:
                // check if we need to wait for write events to be returned
                if(quit_ready(cb_data)){
                    ev->ev_type = EV_QUIT_UP;
                    pass_up(upstream, ev);
                    return true;
                }else{
                    quit_ev = ev;
                }
                break;
            case EV_WRITE_DONE:
                handle_write_done(cb_data, ev);
                // check for quitting condition
                if(quit_ev && quit_ready(cb_data)){
                    quit_ev->ev_type = EV_QUIT_UP;
                    pass_up(upstream, quit_ev);
                    return true;
                }
                break;
            // other events should not happen
            case EV_READ_DONE:
            case EV_WRITE:
            case EV_QUIT_UP:
            default:
                LOG_ERROR("unexpected event type from upstream engine\n");
                return false;
        }
    }
    return true;
}



