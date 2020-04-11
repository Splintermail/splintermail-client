#ifndef SM_SERVE_H
#define SM_SERVE_H

#include "libdstr/libdstr.h"
#include "manager.h"
#include "imap_session.h"
#include "refs.h"
#include "libimaildir/libimaildir.h"

/* server is only responsible for navigating until just before the SELECTED
   state, everything after that is the responsibility of the imaildir_t */
typedef enum {
    // general states
    PREAUTH = 0,    // before login
    AUTHENTICATED,  // imap rfc "authenticated" state
    SELECTED,       // in this state, filter things the maildir doesn't want
} imap_server_state_t;
const dstr_t *imap_server_state_to_dstr(imap_server_state_t state);

typedef struct {
    manager_i mgr;
    imap_session_t s;
    // parser callbacks and imap extesions
    imape_control_i ctrl;
} server_session_t;
DEF_CONTAINER_OF(server_session_t, mgr, manager_i);
DEF_CONTAINER_OF(server_session_t, ctrl, imape_control_i);

// a callback with a condition check
struct pause_t;
typedef struct pause_t pause_t;

struct pause_t {
    // you can't call run until ready returns true
    bool (*ready)(pause_t*);
    // you must call either run or cancel, but not both
    derr_t (*run)(pause_t**);
    void (*cancel)(pause_t**);
};

typedef struct {
    imap_pipeline_t *pipeline;
    ssl_context_t *ctx_srv;
    dirmgr_t dirmgr;
    dstr_t path;
    // participate in proper shutdown sequence as an engine
    engine_t engine;
    // our manager
    manager_i *mgr;
    // sessions
    server_session_t dn;
    // every server_t has only one uv_work_t, so it's single threaded
    uv_work_t uv_work;
    bool greeting_sent;
    bool executing;
    bool closed_onthread;
    bool dead;

    // we need an async to be able to call advance from any thread
    uv_async_t advance_async;
    async_spec_t advance_spec;

    // thread-safe components
    struct {
        uv_mutex_t mutex;
        bool closed;
        // from downwards session
        link_t unhandled_cmds;  // imap_cmd_t->link
        // from maildir
        link_t maildir_resps;  // imap_resp_t->link
        size_t n_live_sessions;
    } ts;

    imap_server_state_t imap_state;
    bool saw_capas;

    // the interface we feed to the imaildir for client communication
    maildir_conn_dn_i conn_dn;
    maildir_dn_i *maildir_dn;
    bool maildir_has_ref;

    // pause is for delaying actions until some future time
    pause_t *pause;
    // if non-NULL, we're waiting on some tagged response to be passed out
    ie_dstr_t *await_tag;
} server_t;
DEF_CONTAINER_OF(server_t, dn, server_session_t);
DEF_CONTAINER_OF(server_t, advance_spec, async_spec_t);
DEF_CONTAINER_OF(server_t, uv_work, uv_work_t);
DEF_CONTAINER_OF(server_t, conn_dn, maildir_conn_dn_i);
DEF_CONTAINER_OF(server_t, engine, engine_t);

/* Advance the state machine of the serve controller by some non-zero amount.
   This will only be called if server_more_work returns true.  It is
   run on a worker thread from a pool, but it will only be executing on one
   thread at a time. */
derr_t server_do_work(server_t *server);

/* Decide if it is possible to advance the state machine or not.  This should
   not alter the server state in any way, and so it does not have to
   be thread-safe with respect to reads, because all external-facing state
   modifications will trigger another check. */
bool server_more_work(server_t *server);

void server_close_maildir_onthread(server_t *server);

// safe to call many times from any thread
void server_close(server_t *server, derr_t error);

// safe to call many times from any thread
void server_advance(server_t *server);

#endif // SM_SERVE_H

