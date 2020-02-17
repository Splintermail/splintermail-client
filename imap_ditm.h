#ifndef IMAP_DITM_H
#define IMAP_DITM_H

#include "libdstr/common.h"
#include "libdstr/link.h"
#include "manager.h"
#include "imap_session.h"

typedef struct {
    manager_i mgr;
    imap_session_t s;
    derr_t error;
    bool close_called;
    // parser callbacks and imap extesions
    imape_control_i ctrl;
} ditm_session_t;
DEF_CONTAINER_OF(ditm_session_t, mgr, manager_i);
DEF_CONTAINER_OF(ditm_session_t, ctrl, imape_control_i);

typedef struct {
    imap_pipeline_t *pipeline;
    ssl_context_t *cli_ctx;
    ssl_context_t *srv_ctx;
    // our manager
    manager_i *mgr;
    // sessions
    ditm_session_t up;
    ditm_session_t dn;
    // every imap_ditm_t has only one uv_work_t, so it's single threaded
    uv_work_t uv_work;
    bool executing;
    bool closed;
    bool dead;
    derr_t error;
    bool startup_completed;

    // we need an async to be able to call advance from any thread
    uv_async_t advance_async;
    async_spec_t advance_spec;

    // thread-safe components
    struct {
        uv_mutex_t mutex;
        // from downwards session
        link_t unhandled_cmds;  // imap_cmd_t->link
        link_t returned_resps;  // resp_event_t.ev->link
        // from upwards session
        link_t unhandled_resps;  // imap_resp_t->link
        link_t returned_cmds;  // cmd_event_t.ev->link
        size_t n_live_sessions;
        size_t n_unreturned_events;
    } ts;

    // tags for commands we received from below, but haven't responded to yet
    link_t unresponded_tags;

    // tags for commands we sent upwards, but haven't gotten a response yet
    link_t inflight_tags;
} imap_ditm_t;
DEF_CONTAINER_OF(imap_ditm_t, up, ditm_session_t);
DEF_CONTAINER_OF(imap_ditm_t, dn, ditm_session_t);
DEF_CONTAINER_OF(imap_ditm_t, advance_spec, async_spec_t);
DEF_CONTAINER_OF(imap_ditm_t, uv_work, uv_work_t);

// imap_ditm_cmd_ev_returner is an event_returner_t
void imap_ditm_cmd_ev_returner(event_t *ev);

// imap_ditm_resp_ev_returner is an event_returner_t
void imap_ditm_resp_ev_returner(event_t *ev);

/* Advance the state machine of the imap ditm by some non-zero amount.  This
   will only be called if imap_ditm_more_work returns true.  It is run on a
   worker thread from a pool, but it will only be executing on one thread at
   a time. */
derr_t imap_ditm_do_work(imap_ditm_t *ditm);

/* Decide if it is possible to advance the state machine or not.  This should
   not alter the imap_ditm state in any way, and so it does not have to be
   thread-safe with respect to reads, because all external-facing state
   modifications will trigger another check. */
bool imap_ditm_more_work(imap_ditm_t *ditm);

#endif // IMAP_DITM_H
