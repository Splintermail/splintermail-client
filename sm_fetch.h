#ifndef SM_FETCH_H
#define SM_FETCH_H

#include "libdstr/libdstr.h"
#include "manager.h"
#include "imap_session.h"
#include "libimaildir/libimaildir.h"

/* fetcher is only responsible for navigating until just before the SELECTED
   state, everything after that is the responsibility of the imaildir_t */
typedef enum {
    // general states
    PREGREET = 0,   // before receiving the greeting
    PREAUTH,        // after login
    AUTHENTICATED,  // imap rfc "authenticated" state
    LISTING,        // after sending LIST, before receiving response

    SELECTED,       // in this state, filter things the maildir doesn't want
} imap_client_state_t;
const dstr_t *imap_client_state_to_dstr(imap_client_state_t state);

typedef struct {
    manager_i mgr;
    imap_session_t s;
    // parser callbacks and imap extesions
    imape_control_i ctrl;
} fetcher_session_t;
DEF_CONTAINER_OF(fetcher_session_t, mgr, manager_i);
DEF_CONTAINER_OF(fetcher_session_t, ctrl, imape_control_i);

typedef struct {
    const char *host;
    const char *svc;
    const dstr_t *user;
    const dstr_t *pass;
    imap_pipeline_t *pipeline;
    ssl_context_t *cli_ctx;
    dirmgr_t dirmgr;
    dstr_t path;
    // participate in proper shutdown sequence as an engine
    engine_t engine;
    // our manager
    manager_i *mgr;
    // sessions
    fetcher_session_t up;
    // every fetcher_t has only one uv_work_t, so it's single threaded
    uv_work_t uv_work;
    bool executing;
    bool closed_onthread;
    bool dead;
    event_t *quit_ev;

    // we need an async to be able to call advance from any thread
    uv_async_t advance_async;
    async_spec_t advance_spec;

    // thread-safe components
    struct {
        uv_mutex_t mutex;
        bool closed;
        // from upwards session
        link_t unhandled_resps;  // imap_resp_t->link
        // from maildir
        link_t maildir_cmds;  // imap_cmd_t->link
        size_t n_live_sessions;
        size_t n_unreturned_events;
    } ts;

    // commands we sent upwards, but haven't gotten a response yet
    link_t inflight_cmds;  // imap_cmd_cb_t->link

    imap_client_state_t imap_state;
    bool saw_capas;
    // accumulated LIST response
    jsw_atree_t folders;
    bool listed;
    // for walking through the list of folders, synchronizing each one
    jsw_atrav_t folders_trav;

    size_t tag;

    // the interface we feed to the imaildir for server communication
    maildir_conn_up_i conn_up;
    maildir_i *maildir;
    bool maildir_has_ref;
    bool mailbox_synced;
    bool mailbox_unselected;

} fetcher_t;
DEF_CONTAINER_OF(fetcher_t, up, fetcher_session_t);
DEF_CONTAINER_OF(fetcher_t, advance_spec, async_spec_t);
DEF_CONTAINER_OF(fetcher_t, uv_work, uv_work_t);
DEF_CONTAINER_OF(fetcher_t, conn_up, maildir_conn_up_i);
DEF_CONTAINER_OF(fetcher_t, engine, engine_t);

/* Advance the state machine of the fetch controller by some non-zero amount.
   This will only be called if fetcher_more_work returns true.  It is
   run on a worker thread from a pool, but it will only be executing on one
   thread at a time. */
derr_t fetcher_do_work(fetcher_t *fetcher);

/* Decide if it is possible to advance the state machine or not.  This should
   not alter the fetcher state in any way, and so it does not have to
   be thread-safe with respect to reads, because all external-facing state
   modifications will trigger another check. */
bool fetcher_more_work(fetcher_t *fetcher);

// safe to call many times from any thread
void fetcher_close(fetcher_t *fetcher, derr_t error);

// safe to call many times from any thread
void fetcher_advance(fetcher_t *fetcher);

#endif // SM_FETCH_H

