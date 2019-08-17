#include "imap_session.h"
#include "logger.h"

static void uv_perror(const char *prefix, int code){
    fprintf(stderr, "%s: %s\n", prefix, uv_strerror(code));
}

static derr_t session_alloc(imap_session_t **sptr, imap_pipeline_t *pipeline,
        ssl_context_t *ssl_ctx, bool upwards){
    derr_t e = E_OK;
    // allocate the struct
    imap_session_t *s = malloc(sizeof(*s));
    if(!s) ORIG(E_NOMEM, "no mem");
    *s = (imap_session_t){0};

    // session prestart
    int ret = uv_mutex_init(&s->mutex, NULL);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing mutex", fail);
    }
    s->refs = 1;
    s->closed = false;
    s->pipeline = pipeline;
    s->session.ld = &s->loop_data;
    s->session.td = &s->tlse_data;
    s->session.id = &s->imape_data;
    s->ssl_ctx = ssl_ctx;

    // per-engine prestart
    loop_data_prestart(&s->loop_data, s->pipeline->loop, &s->session,
            imap_session_ref_up_loop, imap_session_ref_down_loop);

    tlse_data_prestart(&s->tlse_data, s->pipeline->tlse, &s->session,
            fake_session_ref_up_tlse, fake_session_ref_down_tlse, ssl_ctx,
            upwards);

    *sptr = s;
    return e;

fail:
    free(s);
    *sptr = NULL;
    return e;
}

derr_t imap_connect_up(imap_session_t **sptr, const imap_client_spec_t *spec);

void imap_session_start(imap_session_t *s){
    // per-engine start
    loop_data_start(&s->loop_data, s->loop, s);
    tlse_data_start(&s->tlse_data, s->tlse, s);
    imape_data_start(&s->imape_data, s->imape, s);
    session_ref_down(s);
}

static void imap_session_ref_up(void *session){
    imap_session_t *s = session;
    uv_mutex_lock(&s->mutex);
    s->refs++;
    uv_mutex_unlock(&s->mutex);
}

static void imap_session_ref_down(void *session){
    imap_session_t *s = session;
    uv_mutex_lock(&s->mutex);
    int refs = --s->refs;
    uv_mutex_unlock(&s->mutex);

    if(refs > 0) return;

    // finish closing the imape_data
    if(s->pipeline->imape){
        imape_data_postclose(&s->imape_data);
    }

    // now the session is no longer in use, we call the session manager hook
    if(s->session_destroyed){
        s->session_destroyed(s, s->error);
    }

    // free the session
    pthread_mutex_destroy(&s->mutex);
    free(s);
}

// only called by loop thread
void imap_session_ref_up_loop(void *session, int reason){
    imap_session_t *s = session;
    s->loop_refs[reason]++;
    imap_session_ref_up(session);
}
void imap_session_ref_down_loop(void *session, int reason){
    imap_session_t *s = session;
    s->loop_refs[reason]--;
    for(size_t i = 0; i < LOOP_REF_MAXIMUM; i++){
        if(s->loop_refs[i] < 0){
            LOG_ERROR("negative loop ref of type %x!\n",
                      FD(loop_ref_reason_to_dstr(reason)));
        }
    }
    imap_session_ref_down(session);
}

// only called by tlse thread
void imap_session_ref_up_tlse(void *session, int reason){
    imap_session_t *s = session;
    s->tlse_refs[reason]++;
    imap_session_ref_up(session);
}
void imap_session_ref_down_tlse(void *session, int reason){
    imap_session_t *s = session;
    s->tlse_refs[reason]--;
    for(size_t i = 0; i < TLSE_REF_MAXIMUM; i++){
        if(s->tlse_refs[i] < 0){
            LOG_ERROR("negative tlse ref of type %x!\n",
                      FD(tlse_ref_reason_to_dstr(reason)));
        }
    }
    imap_session_ref_down(session);
}

// only called by imape thread
void imap_session_ref_up_imape(void *session, int reason){
    imap_session_t *s = session;
    s->imape_refs[reason]++;
    imap_session_ref_up(session);
}
void imap_session_ref_down_imape(void *session, int reason){
    imap_session_t *s = session;
    s->imape_refs[reason]--;
    for(size_t i = 0; i < IMAPE_REF_MAXIMUM; i++){
        if(s->imape_refs[i] < 0){
            LOG_ERROR("negative imape ref of type %x!\n",
                      FD(imape_ref_reason_to_dstr(reason)));
        }
    }
    imap_session_ref_down(session);
}

void session_close(void *session, derr_t error){
    (void)error;
    imap_session_t *s = session;
    uv_mutex_lock(&s->mutex);
    bool do_close = !s->closed;
    s->closed = true;
    uv_mutex_unlock(&s->mutex);

    if(!do_close) return;

    /* make sure every engine_data has a chance to pass a close event; a slow
       session without standing references must be protected */
    imap_session_ref_up(session);
    loop_data_close(&s->loop_data, s->loop, s);
    tlse_data_close(&s->tlse_data, s->tlse, s);
    imape_data_close(&s->imape_data, s->imape, s);
    imap_session_ref_down(session);

    if(error) loop_close(s->loop, error);
}
