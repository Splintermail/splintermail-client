#include "imap_session.h"
#include "libdstr/libdstr.h"
#include "libuvthread/libuvthread.h"

void imap_session_ref_up(imap_session_t *s){
    uv_mutex_lock(&s->mutex);
    ++s->refs;
    // LOG_DEBUG("ref_up (%x)\n", FI(s->refs));
    uv_mutex_unlock(&s->mutex);
}

void imap_session_ref_down(imap_session_t *s){
    uv_mutex_lock(&s->mutex);
    int refs = --s->refs;
    // LOG_DEBUG("ref_down (%x)\n", FI(s->refs));
    uv_mutex_unlock(&s->mutex);

    if(refs > 0) return;

    // signal to the manager that we are dead, manager will free us later
    s->mgr->dead(s->mgr, s);
}

void imap_session_free(imap_session_t *s){

    // loop_data does not need freeing

    // tlse_data does not need freeing

    // imape_data does not need freeing

    // free our own resources
    uv_mutex_destroy(&s->mutex);
}

// only called by loop thread
static void imap_session_ref_up_loop(session_t *session, int reason){
    imap_session_t *s = CONTAINER_OF(session, imap_session_t, session);
    s->loop_refs[reason]++;
    imap_session_ref_up(s);
}
static void imap_session_ref_down_loop(session_t *session, int reason){
    imap_session_t *s = CONTAINER_OF(session, imap_session_t, session);
    s->loop_refs[reason]--;
    for(size_t i = 0; i < LOOP_REF_MAXIMUM; i++){
        if(s->loop_refs[i] < 0){
            LOG_ERROR("negative loop ref of type %x!\n",
                      FD(loop_ref_reason_to_dstr(i)));
        }
    }
    imap_session_ref_down(s);
}

// only called by tlse thread
static void imap_session_ref_up_tlse(session_t *session, int reason){
    imap_session_t *s = CONTAINER_OF(session, imap_session_t, session);
    s->tlse_refs[reason]++;
    imap_session_ref_up(s);
}
static void imap_session_ref_down_tlse(session_t *session, int reason){
    imap_session_t *s = CONTAINER_OF(session, imap_session_t, session);
    s->tlse_refs[reason]--;
    for(size_t i = 0; i < TLSE_REF_MAXIMUM; i++){
        if(s->tlse_refs[i] < 0){
            LOG_ERROR("negative tlse ref of type %x!\n",
                      FD(tlse_ref_reason_to_dstr(i)));
        }
    }
    imap_session_ref_down(s);
}

// only called by imape thread
static void imap_session_ref_up_imape(session_t *session, int reason){
    imap_session_t *s = CONTAINER_OF(session, imap_session_t, session);
    uv_mutex_lock(&s->mutex);
    s->imape_refs[reason]++;
    uv_mutex_unlock(&s->mutex);
    imap_session_ref_up(s);
}
static void imap_session_ref_down_imape(session_t *session, int reason){
    imap_session_t *s = CONTAINER_OF(session, imap_session_t, session);
    uv_mutex_lock(&s->mutex);
    s->imape_refs[reason]--;
    for(size_t i = 0; i < IMAPE_REF_MAXIMUM; i++){
        if(s->imape_refs[i] < 0){
            LOG_ERROR("negative imape ref of type %x!\n",
                      FD(imape_ref_reason_to_dstr(i)));
        }
    }
    uv_mutex_unlock(&s->mutex);
    imap_session_ref_down(s);
}


void imap_session_close(session_t *session, derr_t error){
    imap_session_t *s = CONTAINER_OF(session, imap_session_t, session);
    uv_mutex_lock(&s->mutex);
    bool do_close = !s->closed;
    s->closed = true;
    uv_mutex_unlock(&s->mutex);

    if(!do_close){
        /* TODO: find some way to gather secondary errors, since we already
                 passed the first one as soon as we saw it */
        DROP_VAR(&error);
        return;
    }

    // signal to the manager that we are dying
    s->mgr->dying(s->mgr, s, error);
    PASSED(error);

    /* make sure every engine_data has a chance to pass a close event; a slow
       engine without standing references must be protected */
    imap_session_ref_up(s);
    if(s->pipeline->loop){
        loop_data_close(&s->loop_data);
    }
    if(s->pipeline->tlse){
        tlse_data_close(&s->tlse_data);
    }
    if(s->pipeline->imape){
        imape_data_close(&s->imape_data);
    }
    imap_session_ref_down(s);
}


// to allocate new sessions (when loop.c only know about a single child struct)
static derr_t imap_session_do_alloc(imap_session_t *s,
        const imap_session_alloc_args_t *args, bool upwards){
    derr_t e = E_OK;

    // zeroize session
    *s = (imap_session_t){ .upwards = upwards };

    // session prestart
    int ret = uv_mutex_init(&s->mutex);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "error initializing mutex");
    }
    // startup protection
    s->refs = 1;
    s->closed = false;
    s->pipeline = args->pipeline;
    s->session.ld = &s->loop_data;
    s->session.td = &s->tlse_data;
    s->session.id = &s->imape_data;
    s->session.close = imap_session_close;
    s->mgr = args->mgr;

    // per-engine prestart
    if(s->pipeline->loop){
        loop_data_prestart(&s->loop_data, s->pipeline->loop, &s->session,
                args->host, args->service, imap_session_ref_up_loop,
                imap_session_ref_down_loop);
    }
    if(s->pipeline->tlse){
        tlse_data_prestart(&s->tlse_data, s->pipeline->tlse, &s->session,
                imap_session_ref_up_tlse, imap_session_ref_down_tlse,
                args->ssl_ctx, upwards);
    }
    if(s->pipeline->imape){
        imape_data_prestart(&s->imape_data, s->pipeline->imape, &s->session,
                imap_session_ref_up_imape, imap_session_ref_down_imape,
                args->imap_control, args->downstream);
    }

    return e;
}

void imap_session_start(imap_session_t *s){
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
    // end of startup protection
    imap_session_ref_down(s);
}

// to allocate new sessions (when loop.c only know about a single child struct)
derr_t imap_session_alloc_accept(imap_session_t *s,
        const imap_session_alloc_args_t *args){
    derr_t e = E_OK;
    PROP(&e, imap_session_do_alloc(s, args, false) );
    return e;
}

derr_t imap_session_alloc_connect(imap_session_t *s,
        const imap_session_alloc_args_t *args){
    derr_t e = E_OK;
    PROP(&e, imap_session_do_alloc(s, args, true) );
    return e;
}

void imap_session_send_event(imap_session_t *s, event_t *ev){
    engine_t *engine = &s->pipeline->imape->engine;
    engine->pass_event(engine, ev);
}
