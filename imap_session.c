#include "imap_session.h"
#include "logger.h"
#include "uv_util.h"

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

    // finish closing the imape_data
    if(s->pipeline->imape){
        imape_data_postclose(&s->imape_data);
    }

    // now the session is no longer in use, we call the session manager hook
    s->session_destroyed(s, s->error);
    PASSED(s->error);

    // free the session
    uv_mutex_destroy(&s->mutex);
    free(s);
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


static void imap_session_close(session_t *session, derr_t error){
    imap_session_t *s = CONTAINER_OF(session, imap_session_t, session);
    MERGE_VAR(&s->error, &error, "session_close error");
    uv_mutex_lock(&s->mutex);
    bool do_close = !s->closed;
    s->closed = true;
    uv_mutex_unlock(&s->mutex);

    if(!do_close) return;

    /* make sure every engine_data has a chance to pass a close event; a slow
       session without standing references must be protected */
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
static derr_t imap_session_do_alloc(imap_session_t **sptr, imap_pipeline_t *p,
        ssl_context_t* ssl_ctx, const char *host, const char *service,
        logic_alloc_t logic_alloc, void *alloc_data){
    derr_t e = E_OK;

    bool upwards = (host != NULL);

    // allocate the struct
    imap_session_t *s = malloc(sizeof(*s));
    if(!s) ORIG(&e, E_NOMEM, "no mem");
    *s = (imap_session_t){0};

    // session prestart
    int ret = uv_mutex_init(&s->mutex);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing mutex", fail);
    }
    s->refs = 1;
    s->closed = false;
    s->pipeline = p;
    s->session.ld = &s->loop_data;
    s->session.td = &s->tlse_data;
    s->session.id = &s->imape_data;
    s->session.close = imap_session_close;

    // per-engine prestart
    if(s->pipeline->loop){
        loop_data_prestart(&s->loop_data, s->pipeline->loop, &s->session, host,
                service, imap_session_ref_up_loop, imap_session_ref_down_loop);
    }
    if(s->pipeline->tlse){
        tlse_data_prestart(&s->tlse_data, s->pipeline->tlse, &s->session,
                imap_session_ref_up_tlse, imap_session_ref_down_tlse, ssl_ctx,
                upwards);
    }
    if(s->pipeline->imape){
        imape_data_prestart(&s->imape_data, s->pipeline->imape, &s->session,
                upwards, imap_session_ref_up_imape,
                imap_session_ref_down_imape,
                logic_alloc, alloc_data);
    }

    *sptr = s;

    return e;

fail:
    free(s);
    *sptr = NULL;
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
    imap_session_ref_down(s);
}

// to allocate new sessions (when loop.c only know about a single child struct)
derr_t imap_session_alloc_accept(imap_session_t **sptr, imap_pipeline_t *p,
        ssl_context_t* ssl_ctx, logic_alloc_t logic_alloc, void *alloc_data){
    derr_t e = E_OK;
    PROP(&e, imap_session_do_alloc(sptr, p, ssl_ctx, NULL, NULL, logic_alloc,
                alloc_data) );
    return e;
}

derr_t imap_session_alloc_connect(imap_session_t **sptr, imap_pipeline_t *p,
        ssl_context_t* ssl_ctx, const char *host, const char *service,
        logic_alloc_t logic_alloc, void *alloc_data){
    derr_t e = E_OK;
    PROP(&e, imap_session_do_alloc(sptr, p, ssl_ctx, host, service,
                logic_alloc, alloc_data) );
    return e;
}
