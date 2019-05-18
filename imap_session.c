#include "imap_session.h"
#include "logger.h"

static void uv_perror(const char *prefix, int code){
    fprintf(stderr, "%s: %s\n", prefix, uv_strerror(code));
}

derr_t session_alloc(void** sptr, void* data, ssl_context_t *ssl_ctx){
    derr_t error;
    imap_pipeline_t *pipeline = data;
    // allocate the struct
    imap_session_t *s = malloc(sizeof(*s));
    if(!s) ORIG(E_NOMEM, "no mem");
    *s = (imap_session_t){0};
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
    test_context_t *test_ctx = data;
    s->loop = &test_ctx->loop;
    s->tlse = &test_ctx->tlse;
    s->ssl_ctx = ssl_ctx;
    // start engine data structs
    loop_data_start(&s->loop_data, s->loop, s);
    tlse_data_start(&s->tlse_data, s->tlse, s);
    imape_data_start(&s->imape_data, s->imape, s);
    *sptr = s;
    session_ref_down(s);
    return E_OK;

fail:
    free(s);
    *sptr = NULL;
    return error;
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
    session_ref_up(session);
    loop_data_close(&s->loop_data, s->loop, s);
    tlse_data_close(&s->tlse_data, s->tlse, s);
    imape_data_close(&s->imape_data, s->imape, s);
    session_ref_down(session);

    if(error) loop_close(s->loop, error);
}

void imap_session_ref_up(void *session){
    imap_session_t *s = session;
    uv_mutex_lock(&s->mutex);
    s->refs++;
    uv_mutex_unlock(&s->mutex);
}

void imap_session_ref_down(void *session){
    imap_session_t *s = session;
    uv_mutex_lock(&s->mutex);
    size_t refs = --s->refs;
    uv_mutex_unlock(&s->mutex);

    if(refs > 0) return;

    // free the session
    uv_mutex_destroy(&s->mutex);
    free(s);
}



static loop_data_t *session_get_loop_data(void *session){
    imap_session_t *s = session;
    return &s->loop_data;
}

static tlse_data_t *session_get_tlse_data(void *session){
    imap_session_t *s = session;
    return &s->tlse_data;
}

static ssl_context_t *session_get_ssl_ctx_client(void *session){
    imap_session_t *s = session;
    return s->ssl_ctx_client;
}

static ssl_context_t *session_get_ssl_ctx_server(void *session){
    imap_session_t *s = session;
    return s->ssl_ctx_server;
}
