#ifndef IMAP_SESSION_H
#define IMAP_SESSION_H

#include <uv.h>
#include "engine.h"
#include "loop.h"
#include "tls_engine.h"
#include "networking.h"

typedef struct {
    uv_mutex_t mutex;
    size_t refs;
    bool closed;
    loop_t *loop;
    tlse_t *tlse;
    ssl_context_t *ssl_ctx;
    loop_data_t loop_data;
    tlse_data_t tlse_data;
    imape_data_t imape_data;
} imap_session_t;

typedef struct {
    loop_t *loop;
    tlse_t *tlse;
    imape_t *imape;
    ssl_context_t *ssl_ctx_client;
    ssl_context_t *ssl_ctx_server;
} imap_pipeline_t;

/* **sptr is where the allocated session is placed.
   *data should be an imap_pipeline_t */
derr_t imap_session_alloc(void** sptr, void *data, ssl_context_t *ssl_ctx);
void imap_session_close(void *session, derr_t error);

void imap_session_ref_up(void *session);
void imap_session_ref_down(void *session);

extern session_iface_t imap_session_iface;

#endif // IMAP_SESSION_H
