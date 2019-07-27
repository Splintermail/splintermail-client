#include "imap_session.h"
#include "logger.h"

static void uv_perror(const char *prefix, int code){
    fprintf(stderr, "%s: %s\n", prefix, uv_strerror(code));
}

derr_t session_alloc(void **sptr, void *pipeline, ssl_context_t *ssl_ctx){
    derr_t e = E_OK;
    // allocate the struct
    imap_session_t *s = malloc(sizeof(*s));
    if(!s) ORIG(E_NOMEM, "no mem");
    *s = (imap_session_t){0};
    // save some important data
    s->pipeline = pipeline;
    // prepare the refs
    ret = uv_mutex_init(&s->mutex, NULL);
    if(ret < 0){
        uv_perror("uv_mutex_init", ret);
        error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG_GO(error, "error initializing mutex", fail);
    }
    s->refs = 1;
    s->closed = false;
    // prepare for session getters
    imap_pipeline_t *pipeline = data;
    s->loop = &pipeline->loop;
    s->tlse = &pipeline->tlse;
    s->ssl_ctx = ssl_ctx;
    // start engine data structs
    loop_data_start(&s->loop_data, s->loop, s);
    tlse_data_start(&s->tlse_data, s->tlse, s);
    imape_data_start(&s->imape_data, s->imape, s);
    *sptr = s;
    session_ref_down(s);
    return e;

fail:
    free(s);
    *sptr = NULL;
    return e;
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

static loop_data_t *session_get_loop_data(void *session){
    imap_session_t *s = session;
    return &s->loop_data;
}

static tlse_data_t *session_get_tlse_data(void *session){
    imap_session_t *s = session;
    return &s->tlse_data;
}

static imape_data_t *session_get_imape_data(void *session){
    imap_session_t *s = session;
    return &s->imape_data;
}

static ssl_context_t *session_get_ssl_ctx_client(void *session){
    imap_session_t *s = session;
    return s->ssl_ctx_client;
}

static ssl_context_t *session_get_ssl_ctx_server(void *session){
    imap_session_t *s = session;
    return s->ssl_ctx_server;
}
