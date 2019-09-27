#ifndef IMAP_SESSION_H
#define IMAP_SESSION_H

#include <uv.h>

#include "engine.h"
#include "loop.h"
#include "tls_engine.h"
#include "imap_engine.h"
#include "networking.h"

struct imap_session_t;
typedef struct imap_session_t imap_session_t;

typedef struct {
    loop_t *loop;
    tlse_t *tlse;
    imape_t *imape;
} imap_pipeline_t;

struct imap_session_t {
    session_t session;
    uv_mutex_t mutex;
    int refs;
    bool closed;
    derr_t error;
    // engines
    imap_pipeline_t *pipeline;
    // engine_data elements
    loop_data_t loop_data;
    tlse_data_t tlse_data;
    imape_data_t imape_data;
    // per-reason-per-engine reference counts
    int loop_refs[LOOP_REF_MAXIMUM];
    int tlse_refs[TLSE_REF_MAXIMUM];
    int imape_refs[IMAPE_REF_MAXIMUM];
    /* session manager hook; this is to be set externally, after session_alloc
       but before session_start */
    void (*session_destroyed)(imap_session_t*, derr_t);
    void *mgr_data;
};
DEF_CONTAINER_OF(imap_session_t, session, session_t)

/* The imap session will not be freed in between imap_session_alloc_*() and
   imap_session_start(), even in the case of an asynchronous failure.  However,
   if you run into an error after imap_session_alloc_*() but before
   imap_session_start(), you will need to downref the session to clean it up */
derr_t imap_session_alloc_accept(imap_session_t **sptr, imap_pipeline_t *p,
        ssl_context_t* ssl_ctx, logic_alloc_t logic_alloc, void *alloc_data);
derr_t imap_session_alloc_connect(imap_session_t **sptr, imap_pipeline_t *p,
        ssl_context_t* ssl_ctx, const char *host, const char *service,
        logic_alloc_t logic_alloc, void *alloc_data);
void imap_session_start(imap_session_t *s);

void imap_session_ref_up(imap_session_t *session);
void imap_session_ref_down(imap_session_t *session);


#endif // IMAP_SESSION_H
