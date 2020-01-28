#include <signal.h>

#include "sm_serve.h"
#include "logger.h"
#include "uv_util.h"
#include "jsw_atree.h"
#include "networking.h"

#define KEY "../c/test/files/ssl/good-key.pem"
#define CERT "../c/test/files/ssl/good-cert.pem"
#define DH "../c/test/files/ssl/dh_4096.pem"

uv_idle_t idle;
loop_t loop;
tlse_t tlse;
imape_t imape;

// engine interface
static void server_pass_event(struct engine_t *engine, event_t *ev){
    server_t *server = CONTAINER_OF(engine, server_t, engine);

    imap_event_t *imap_ev;

    switch(ev->ev_type){
        case EV_READ:
            imap_ev = CONTAINER_OF(ev, imap_event_t, ev);
            // server only accepts imap_resp_t's as EV_READs
            if(imap_ev->type != IMAP_EVENT_TYPE_RESP){
                LOG_ERROR("unallowed imap_cmd_t as EV_READ in server\n");
                ev->returner(ev);
                break;
            }

            // steal the imap_resp_t and return the rest of the event
            imap_resp_t *resp = imap_ev->arg.resp;
            imap_ev->arg.resp = NULL;
            ev->returner(ev);

            // queue up the response
            uv_mutex_lock(&server->ts.mutex);
            link_list_append(&server->ts.unhandled_cmds, &resp->link);
            uv_mutex_unlock(&server->ts.mutex);
            break;

        default:
            LOG_ERROR("unallowed event type (%x)\n", FU(ev->ev_type));
    }

    // trigger more work
    server_advance(server);
}

// forward declarations
static void server_advance_onthread(server_t *server);

// callable from any thread
void server_advance(server_t *server){
    // alert the loop thread to try to enqueue the worker
    int ret = uv_async_send(&server->advance_async);
    if(ret < 0){
        /* ret != 0 is only possible under some specific circumstances:
             - if the async handle is not an async type (should never happen)
             - if uv_close was called on the async handle (should never happen
               because we don't close uv_handles until all imap_sessions have
               closed, so there won't be anyone to call this function)

           Therefore, it is safe to not "properly" handle this error.  But, we
           will at least log it since we are relying on undocumented behavior.
        */
        LOG_ERROR("uv_async_send: %x\n", FUV(&ret));
        LOG_ERROR("uv_async_send should never fail!\n");
    }
}

// mgr_dn

static void session_dn_dying(manager_i *mgr, derr_t e){
    server_session_t *dn = CONTAINER_OF(mgr, server_session_t, mgr);
    server_t *server = CONTAINER_OF(dn, server_t, dn);
    printf("session dn dying\n");

    server_close(server, e);
    PASSED(e);

    // any further steps must be done on-thread
    server_advance(server);
}

static void session_dn_dead(manager_i *mgr){
    server_session_t *dn = CONTAINER_OF(mgr, server_session_t, mgr);
    server_t *server = CONTAINER_OF(dn, server_t, dn);

    uv_mutex_lock(&server->ts.mutex);
    server->ts.n_live_sessions--;
    uv_mutex_unlock(&server->ts.mutex);

    server_advance(server);
}

// server_async_cb is a uv_async_cb
static void server_async_cb(uv_async_t *async){
    async_spec_t *spec = async->data;
    server_t *server = CONTAINER_OF(spec, server_t, advance_spec);
    server_advance_onthread(server);
}

// server_async_close_cb is a async_spec_t->close_cb
static void server_async_close_cb(async_spec_t *spec){
    server_t *server = CONTAINER_OF(spec, server_t, advance_spec);

    // shutdown, part three of three: report ourselves as dead
    server->mgr->dead(server->mgr);
}

// part of the imaildir_i, meaning this can be called on- or off-thread
static void server_conn_dn_resp(maildir_conn_dn_i *conn_dn, imap_resp_t *resp){
    server_t *server = CONTAINER_OF(conn_dn, server_t, conn_dn);

    uv_mutex_lock(&server->ts.mutex);
    link_list_append(&server->ts.maildir_resps, &resp->link);
    uv_mutex_unlock(&server->ts.mutex);

    server_advance(server);
}

// part of the imaildir_i, meaning this can be called on- or off-thread
static void server_conn_dn_release(maildir_conn_dn_i *conn_dn, derr_t error){
    derr_t e = E_OK;

    server_t *server = CONTAINER_OF(conn_dn, server_t, conn_dn);

    IF_PROP_VAR(&e, &error){
        // maildir is failing with an error
        server_close(server, e);
        PASSED(e);
    }else{
        // it's still an error if the maildir releases us before we release it
        if(server->maildir != NULL){
            TRACE_ORIG(&e, E_INTERNAL, "maildir closed unexpectedly");
            server_close(server, e);
            PASSED(e);
        }
    }

    server->maildir_has_ref = false;

    server_advance(server);
}

static derr_t server_init(server_t *server, imap_pipeline_t *p,
        ssl_context_t *ctx_srv, manager_i *mgr){
    derr_t e = E_OK;

    // start by zeroizing everything and storing static values
    *server = (server_t){
        .pipeline = p,
        .ctx_srv = ctx_srv,
        .mgr = mgr,

        .engine = {
            .pass_event = server_pass_event,
        },
    };

    server->dn.mgr = (manager_i){
        .dying = session_dn_dying,
        .dead = session_dn_dead,
    };
    server->dn.ctrl = (imape_control_i){
        // enable UIDPLUS, ENABLE, CONDSTORE, and QRESYNC
        .exts = {
            .uidplus = EXT_STATE_ON,
            .enable = EXT_STATE_ON,
            .condstore = EXT_STATE_ON,
            .qresync = EXT_STATE_ON,
        },
        .is_client = true,
    };

    server->advance_spec = (async_spec_t){
        .close_cb = server_async_close_cb,
    };

    server->conn_dn = (maildir_conn_dn_i){
        .resp = server_conn_dn_resp,
        .release = server_conn_dn_release,
    };

    link_init(&server->ts.unhandled_cmds);
    link_init(&server->ts.maildir_resps);

    jsw_ainit(&server->folders, ie_list_resp_cmp, ie_list_resp_get);

    // allocate for the path
    PROP(&e, dstr_new(&server->path, 256) );
    // right now the path is not configurable
    PROP_GO(&e, dstr_copy(&DSTR_LIT("/tmp/maildir_root"), &server->path),
            fail_path);

    // dirmgr
    string_builder_t path = SB(FD(&server->path));
    PROP_GO(&e, dirmgr_init(&server->dirmgr, path, NULL), fail_path);

    int ret = uv_mutex_init(&server->ts.mutex);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing mutex", fail_dirmgr);
    }

    /* establish the uv_async_t; if that fails we must not create off-thread
       resources or they would not be able to communicate with us */

    // allocate memory for the session, but don't start it until later
    imap_session_alloc_args_t arg_dn = {
        server->pipeline,
        &server->dn.mgr,   // manager_i
        server->ctx_srv,   // ssl_context_t
        &server->dn.ctrl,  // imape_control_i
        &server->engine,   // engine_t
        NULL, // host
        NULL, // svc
        (terminal_t){},
    };
    PROP_GO(&e, imap_session_alloc_accept(&server->dn.s, &arg_dn), fail_mutex);

    /* establish the uv_async_t as the last step. If this fails, we can't
       communicate with off-thread resources or they would not be able to
       communicate with us.  However, if something else fails, we can only
       close the uv_async_t asynchronously, so we make sure this is the last
       step that can possibly fail. */
    server->advance_async.data = &server->advance_spec;
    ret = uv_async_init(&server->pipeline->loop->uv_loop,
            &server->advance_async, server_async_cb);
    if(ret < 0){
        TRACE(&e, "uv_async_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing advance_async",
                fail_session);
    }


    // now that everything is configured, it is safe to start the sessions
    server->ts.n_live_sessions = 1;

    return e;

fail_session:
    imap_session_free(&server->dn.s);
fail_mutex:
    uv_mutex_destroy(&server->ts.mutex);
fail_dirmgr:
    dirmgr_free(&server->dirmgr);
fail_path:
    dstr_free(&server->path);
    return e;
}

static void server_start(server_t *server){
    imap_session_start(&server->dn.s);
}

static void server_free(server_t *server){
    if(!server) return;
    // free the folders list
    jsw_anode_t *node;
    while((node = jsw_apop(&server->folders))){
        ie_list_resp_t *list = CONTAINER_OF(node, ie_list_resp_t, node);
        ie_list_resp_free(list);
    }
    // free any imap cmds or resps laying around
    link_t *link;
    while((link = link_list_pop_first(&server->ts.unhandled_cmds))){
        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
        imap_cmd_free(cmd);
    }
    while((link = link_list_pop_first(&server->ts.maildir_resps))){
        imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);
        imap_resp_free(resp);
    }
    // async must already have been closed
    imap_session_free(&server->dn.s);
    uv_mutex_destroy(&server->ts.mutex);
    dirmgr_free(&server->dirmgr);
    dstr_free(&server->path);
    return;
}

// work_cb is a uv_work_cb
static void work_cb(uv_work_t *req){
    server_t *server = CONTAINER_OF(req, server_t, uv_work);

    // all the server state machine logic falls within this call
    derr_t e = E_OK;
    IF_PROP(&e, server_do_work(server) ){
        server_close(server, e);
        PASSED(e);
    }
}

// after_work_cb is a uv_after_work_cb
static void after_work_cb(uv_work_t *req, int status){
    server_t *server = CONTAINER_OF(req, server_t, uv_work);

    // throw an error if the thread was canceled
    if(status < 0){
        derr_t e = E_OK;
        // close server with error
        TRACE(&e, "uv work request failed: %x\n", FUV(&status));
        TRACE_ORIG(&e, uv_err_type(status), "work request failed");
        server_close(server, e);
        PASSED(e);
    }

    // we are no longer executing
    server->executing = false;

    // check if we need to re-enqueue ourselves
    server_advance_onthread(server);
}

// always called from on-thread
static void server_maybe_enqueue(server_t *server){
    if(server->ts.closed) return;
    if(!server_more_work(server)) return;

    // try to enqueue work
    int ret = uv_queue_work(&server->pipeline->loop->uv_loop, &server->uv_work,
            work_cb, after_work_cb);
    if(ret < 0){
        // capture error
        derr_t e = E_OK;
        TRACE(&e, "uv_queue_work: %x\n", FUV(&ret));
        TRACE_ORIG(&e, uv_err_type(ret), "failed to enqueue work");

        server_close(server, e);
        PASSED(e);

        return;
    }

    // work is enqueued
    server->executing = true;

    return;
}

// safe to call many times from any thread
void server_close(server_t *server, derr_t error){
    uv_mutex_lock(&server->ts.mutex);
    // only execute the close sequence once
    bool do_close = !server->ts.closed;
    server->ts.closed = true;
    uv_mutex_unlock(&server->ts.mutex);

    if(!do_close){
        // secondary errors get dropped
        DROP_VAR(&error);
        return;
    }

    // TODO: clean up ownership hierarchy and error reporting
    TRACE_PROP(&error);
    loop_close(server->pipeline->loop, error);
    PASSED(error);

    // tell our owner we are dying
    server->mgr->dying(server->mgr, E_OK);

    // we can close multithreaded resources here but we can't release refs
    imap_session_close(&server->dn.s.session, E_OK);

    // everything else must be done on-thread
    server_advance(server);
}

// only safe to call once
static void server_close_onthread(server_t *server){
    /* now that we are onthread, it is safe to release refs for things we don't
       own ourselves */
    if(server->maildir){
        // extract the maildir from server so it is clear we are done
        maildir_i *maildir = server->maildir;
        server->maildir = NULL;
        // now make the very last call into the maildir_i
        dirmgr_close_dn(&server->dirmgr, maildir, &server->conn_dn);
    }
}

// must be called on-thread
static void server_advance_onthread(server_t *server){
    // if a worker is executing, do nothing; we'll check again in work_done()
    if(server->executing) return;

    if(server->ts.closed && !server->closed_onthread){
        server->closed_onthread = true;
        server_close_onthread(server);
    }

    // shutdown, part two of three: close the async
    if(server->dead){
        uv_close((uv_handle_t*)&server->advance_async, async_handle_close_cb);
        // end of the line for this object
        return;
    }

    // shutdown, part one of three: make the final call to server_advance
    if(server->closed_onthread && server->ts.n_live_sessions == 0
            && !server->maildir_has_ref){
        /* because these counts are thread-safe and we have locked the mutex
           and the count decrementers call async_send inside the same mutex,
           we know that there can be no more calls to async_send now.  That
           does not guarantee that there can't be some unprocessed async_sends
           floating around somewhere, and since I'm not sure what happens if
           we call uv_close on a uv_async_t with unprocessed async_sends, I am
           going to call async_send one more time here, and when that send is
           processed, we will know that no unprocessed sends remain. */
        server->dead = true;
        server_advance(server);
        return;
    }

    // if there is work to do, enqueue some work
    server_maybe_enqueue(server);
}

////////////////


static derr_t build_pipeline(imap_pipeline_t *pipeline, engine_t *quit_engine){
    derr_t e = E_OK;

    // set UV_THREADPOOL_SIZE
    unsigned int nworkers = 2;
    PROP(&e, set_uv_threadpool_size(nworkers + 3, nworkers + 7) );

    // initialize loop
    PROP(&e, loop_init(&loop, 5, 5, &tlse.engine) );

    // intialize TLS engine
    PROP_GO(&e, tlse_init(&tlse, 5, 5, &loop.engine, &imape.engine), fail);
    PROP_GO(&e, tlse_add_to_loop(&tlse, &loop.uv_loop), fail);

    // initialize IMAP engine
    PROP_GO(&e, imape_init(&imape, 5, &tlse.engine, quit_engine), fail);
    PROP_GO(&e, imape_add_to_loop(&imape, &loop.uv_loop), fail);

    *pipeline = (imap_pipeline_t){
        .loop=&loop,
        .tlse=&tlse,
        .imape=&imape,
    };

    return e;

fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("fatal error: failed to construct pipeline\n");
    exit(1);
}

static void free_pipeline(imap_pipeline_t *pipeline){
    imape_free(pipeline->imape);
    tlse_free(pipeline->tlse);
    loop_free(pipeline->loop);
}

// server_mgr_t really only exists for the EV_QUIT_UP/EV_QUIT_DOWN sequence
typedef struct {
    uv_mutex_t mutex;
    link_t mgd;
    size_t n_servers;
    // implement the engine_t interface just for supporting the QUIT sequence
    engine_t engine;
    engine_t *upstream;
    event_t *quit_ev;
} server_mgr_t;
DEF_CONTAINER_OF(server_mgr_t, engine, engine_t);

// managed_srv_t exists to provide unique manager_i*'s to every server_t
typedef struct {
    server_t server;
    // interface provided to server_t
    manager_i mgr;
    // backpointer to server_mgr_t
    server_mgr_t *server_mgr;
    // for storage in server_mgr_t
    link_t link;
} managed_srv_t;
DEF_CONTAINER_OF(managed_srv_t, mgr, manager_i);
DEF_CONTAINER_OF(managed_srv_t, link, link_t);

static void server_mgr_send_quit_up(server_mgr_t *server_mgr){
    server_mgr->quit_ev->ev_type = EV_QUIT_UP;
    engine_t *upstream = server_mgr->upstream;
    upstream->pass_event(upstream, server_mgr->quit_ev);
    server_mgr->quit_ev = NULL;
}

static void managed_srv_free(managed_srv_t **mgd){
    if(!*mgd) return;
    server_free(&(*mgd)->server);
    free(*mgd);
    *mgd = NULL;
    return;
}

static void server_dying(manager_i *mgr, derr_t e){
    managed_srv_t *mgd = CONTAINER_OF(mgr, managed_srv_t, mgr);
    server_mgr_t *server_mgr = mgd->server_mgr;
    // TODO: this error handling is so fucking broken...
    DROP_VAR(&e);
    LOG_ERROR("server dying...\n");

    uv_mutex_lock(&server_mgr->mutex);
    // one less server active
    server_mgr->n_servers--;
    link_remove(&mgd->link);
    // were we waiting for this moment to finish the QUIT sequence?
    if(server_mgr->quit_ev && server_mgr->n_servers == 0){
        server_mgr_send_quit_up(server_mgr);
    }
    uv_mutex_unlock(&server_mgr->mutex);
}

static void server_dead(manager_i *mgr){
    managed_srv_t *mgd = CONTAINER_OF(mgr, managed_srv_t, mgr);
    managed_srv_free(&mgd);
}

static derr_t server_mgr_new_mgd(server_mgr_t *server_mgr, imap_pipeline_t *p,
        ssl_context_t *ctx_srv, managed_srv_t **out){
    derr_t e = E_OK;
    *out = NULL;

    managed_srv_t *mgd = malloc(sizeof(*mgd));
    if(!mgd) ORIG(&e, E_NOMEM, "nomem");
    *mgd = (managed_srv_t){
        .mgr = {
            .dying = server_dying,
            .dead = server_dead,
        },
        .server_mgr = server_mgr,
    };

    link_init(&mgd->link);

    PROP_GO(&e, server_init(&mgd->server, p, ctx_srv, &mgd->mgr), fail);

    // append managed to the server_mgr's list
    uv_mutex_lock(&server_mgr->mutex);
    // detect late-starters and cancel them (shouldn't be possible, but still.)
    if(server_mgr->quit_ev){
        server_free(&mgd->server);
        ORIG_GO(&e, E_DEAD, "server_mgr is quitting", fail);
    }
    link_list_append(&server_mgr->mgd, &mgd->link);
    server_mgr->n_servers++;
    uv_mutex_unlock(&server_mgr->mutex);

    // now it is safe to start the server
    server_start(&mgd->server);

    *out = mgd;

    return e;

fail:
    free(mgd);
    return e;
}

// part of engine_t; only for receiving the QUIT_DOWN
static void server_mgr_pass_event(engine_t *engine, event_t *ev){
    server_mgr_t *server_mgr = CONTAINER_OF(engine, server_mgr_t, engine);
    // only one event should ever get passed
    if(ev->ev_type != EV_QUIT_DOWN){
        LOG_ERROR("server_mgr_t only accepts EV_QUIT_DOWN events\n");
        ev->returner(ev);
        return;
    }

    uv_mutex_lock(&server_mgr->mutex);
    server_mgr->quit_ev = ev;
    if(server_mgr->n_servers == 0){
        server_mgr_send_quit_up(server_mgr);
    }
    uv_mutex_unlock(&server_mgr->mutex);
}

static derr_t server_mgr_init(server_mgr_t *server_mgr, engine_t *upstream){
    derr_t e = E_OK;

    *server_mgr = (server_mgr_t){
        .engine = {
            .pass_event = server_mgr_pass_event,
        },
        .upstream = upstream,
        .n_servers = 0,
    };

    link_init(&server_mgr->mgd);

    int ret = uv_mutex_init(&server_mgr->mutex);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "error initializing mutex");
    }

    return e;
}

static void server_mgr_free(server_mgr_t *server_mgr){
    if(!server_mgr) return;
    uv_mutex_destroy(&server_mgr->mutex);
}


typedef struct {
    server_mgr_t *server_mgr;

    imap_pipeline_t *pipeline;
    ssl_context_t *ctx_srv;

    listener_spec_t lspec;
} server_lspec_t;
DEF_CONTAINER_OF(server_lspec_t, lspec, listener_spec_t);

static derr_t conn_recvd(listener_spec_t *lspec, session_t **session){
    derr_t e = E_OK;

    server_lspec_t *l = CONTAINER_OF(lspec, server_lspec_t, lspec);

    managed_srv_t *mgd;
    PROP(&e, server_mgr_new_mgd(l->server_mgr, l->pipeline, l->ctx_srv,
                &mgd) );

    *session = &mgd->server.dn.s.session;

    return e;
}

static derr_t sm_serve(char *host, char *svc, char *key, char *cert, char *dh){
    derr_t e = E_OK;

    // init OpenSSL
    PROP(&e, ssl_library_init() );

    // init ssl context
    ssl_context_t ctx_srv;
    PROP_GO(&e, ssl_context_new_server(&ctx_srv, cert, key, dh), cu_ssl_lib);

    // init server_mgr, which coordinates QUIT sequence for all servers
    server_mgr_t server_mgr;
    PROP_GO(&e, server_mgr_init(&server_mgr, &imape.engine), cu_ssl_ctx);

    imap_pipeline_t pipeline;
    PROP_GO(&e, build_pipeline(&pipeline, &server_mgr.engine), cu_server_mgr);

    /* After building the pipeline, we must run the pipeline if we want to
       cleanup nicely.  That means that we can't follow the normal cleanup
       pattern, and instead we must initialize all of our variables to zero
       (that is, if we had any variables right here) */

    // add the lspec to the loop
    server_lspec_t server_lspec = {
        .server_mgr = &server_mgr,
        .pipeline = &pipeline,
        .ctx_srv=&ctx_srv,
        .lspec = {
            .addr = host,
            .svc = svc,
            .conn_recvd = conn_recvd,
        },
    };
    PROP_GO(&e, loop_add_listener(&loop, &server_lspec.lspec), fail);

fail:
    if(is_error(e)){
        loop_close(&loop, e);
        // The loop will pass us this error back after loop_run.
        PASSED(e);
    }

    // run the loop
    PROP_GO(&e, loop_run(&loop), cu);

cu:
    free_pipeline(&pipeline);
cu_server_mgr:
    server_mgr_free(&server_mgr);
cu_ssl_ctx:
    ssl_context_free(&ctx_srv);
cu_ssl_lib:
    ssl_library_close();
    return e;

}


static bool hard_exit = false;
static void stop_loop_on_signal(int signum){
    (void) signum;
    LOG_ERROR("caught signal\n");
    if(hard_exit) exit(1);
    hard_exit = true;
    // launch an asynchronous loop abort
    // uv_idle_stop(&idle);
    loop_close(&loop, E_OK);
}


int main(int argc, char **argv){
    // ignore SIGPIPE, required to work with OpenSSL
    // see https://mta.openssl.org/pipermail/openssl-users/2017-May/005776.html
    // (but SIGPIPE doesnt exist in windows)
    signal(SIGINT, stop_loop_on_signal);
    signal(SIGTERM, stop_loop_on_signal);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    char *default_args[] = {
        "./sm_serve",
        "127.0.0.1",
        "1993",
        KEY,
        CERT,
        DH,
    };
    if(argc != 6){
        fprintf(stderr, "usage: sm_serve HOST PORT KEY CERT DH\n");
        if(argc != 1){
            exit(1);
        }
        argc = sizeof(default_args)/sizeof(*default_args);
        argv = default_args;
    }

    // add logger
    logger_add_fileptr(LOG_LVL_INFO, stdout);

    derr_t e = sm_serve(argv[1], argv[2], argv[3], argv[4], argv[5]);
    CATCH(e, E_ANY){
        DUMP(e);
        DROP_VAR(&e);
        return 1;
    }

    return 0;
}

