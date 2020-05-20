#ifndef IMAP_SESSION_H
#define IMAP_SESSION_H

#include <uv.h>

#include "engine.h"
#include "loop.h"
#include "tls_engine.h"
#include "imap_engine.h"
#include "networking.h"
#include "manager.h"
#include "libimap/libimap.h"

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
    manager_i *mgr;
    bool upwards;
};
DEF_CONTAINER_OF(imap_session_t, session, session_t)

typedef struct {} terminal_t;

typedef struct {
    imap_pipeline_t *pipeline;
    manager_i *mgr;
    ssl_context_t* ssl_ctx;
    imape_control_i *imap_control;
    engine_t *downstream;
    // only for connect sessions:
    const char *host;
    const char *service;
    // included to allow for strong type checking
    terminal_t terminal;
} imap_session_alloc_args_t;

/* To destroy a session that you have allocated but not started, you must use
   imap_session_free().  After starting, you must wait until the dead call. */
derr_t imap_session_alloc_accept(imap_session_t *s,
        const imap_session_alloc_args_t *args);
derr_t imap_session_alloc_connect(imap_session_t *s,
        const imap_session_alloc_args_t *args);

// imap_session_start marks the beginning of possible asynchronous errors
void imap_session_start(imap_session_t *s);

void imap_session_ref_up(imap_session_t *s);
void imap_session_ref_down(imap_session_t *s);

// pass an event wrapped in a cmd_event_t or resp_event_t to the imape_data
void imap_session_send_event(imap_session_t *s, event_t *ev);

// should be called by the session manager AFTER the "dying" hook
void imap_session_free(imap_session_t *s);

void imap_session_close(session_t *session, derr_t error);

#endif // IMAP_SESSION_H
