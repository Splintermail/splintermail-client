#include <signal.h>

#include "imap_ditm.h"
#include "libdstr/logger.h"
#include "loop.h"
#include "uv_util.h"
#include "libdstr/jsw_atree.h"
#include "imap_dirmgr.h"
#include "crypto.h"

#define KEY "../c/test/files/ssl/good-key.pem"
#define CERT "../c/test/files/ssl/good-cert.pem"
#define DH "../c/test/files/ssl/dh_4096.pem"

loop_t loop;
tlse_t tlse;
imape_t imape;
keypair_t keypair;

char *g_host;
char *g_svc;

// forward declarations
static void imap_ditm_advance_onthread(imap_ditm_t *ditm);

// callable from any thread
static void imap_ditm_advance(imap_ditm_t *ditm){
    // alert the loop thread to try to enqueue the worker
    int ret = uv_async_send(&ditm->advance_async);
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

// mgr_up

static void session_up_dying(manager_i *mgr, derr_t e){
    ditm_session_t *up = CONTAINER_OF(mgr, ditm_session_t, mgr);
    imap_ditm_t *ditm = CONTAINER_OF(up, imap_ditm_t, up);
    printf("session up dying\n");

    // capture error
    MERGE_VAR(&ditm->up.error, &e, "upwards imap session died");
    ditm->up.close_called = true;

    // any further steps must be done on-thread
    imap_ditm_advance(ditm);
}

static void session_up_dead(manager_i *mgr){
    ditm_session_t *up = CONTAINER_OF(mgr, ditm_session_t, mgr);
    imap_ditm_t *ditm = CONTAINER_OF(up, imap_ditm_t, up);

    uv_mutex_lock(&ditm->ts.mutex);
    if(--ditm->ts.n_live_sessions){
        // any further steps must be done on-thread
        imap_ditm_advance(ditm);
    }
    uv_mutex_unlock(&ditm->ts.mutex);
}

// mgr_dn

static void session_dn_dying(manager_i *mgr, derr_t e){
    ditm_session_t *dn = CONTAINER_OF(mgr, ditm_session_t, mgr);
    imap_ditm_t *ditm = CONTAINER_OF(dn, imap_ditm_t, dn);
    printf("session dn dying\n");

    // capture error
    MERGE_VAR(&ditm->dn.error, &e, "downwards imap session died");
    ditm->dn.close_called = true;

    // any further steps must be done on-thread
    imap_ditm_advance(ditm);
}

static void session_dn_dead(manager_i *mgr){
    ditm_session_t *dn = CONTAINER_OF(mgr, ditm_session_t, mgr);
    imap_ditm_t *ditm = CONTAINER_OF(dn, imap_ditm_t, dn);

    uv_mutex_lock(&ditm->ts.mutex);
    if(--ditm->ts.n_live_sessions){
        // any further steps must be done on-thread
        imap_ditm_advance(ditm);
    }
    uv_mutex_unlock(&ditm->ts.mutex);
}

// safe to call from any thread
static void imap_ditm_add_cmd(imape_control_i *ctrl, imap_cmd_t *cmd){
    ditm_session_t *dn = CONTAINER_OF(ctrl, ditm_session_t, ctrl);
    imap_ditm_t *ditm = CONTAINER_OF(dn, imap_ditm_t, dn);

    uv_mutex_lock(&ditm->ts.mutex);
    link_list_append(&ditm->ts.unhandled_cmds, &cmd->link);
    uv_mutex_unlock(&ditm->ts.mutex);

    imap_ditm_advance(ditm);
}

// safe to call from any thread
static void imap_ditm_add_resp(imape_control_i *ctrl, imap_resp_t *resp){
    ditm_session_t *up = CONTAINER_OF(ctrl, ditm_session_t, ctrl);
    imap_ditm_t *ditm = CONTAINER_OF(up, imap_ditm_t, up);

    uv_mutex_lock(&ditm->ts.mutex);
    link_list_append(&ditm->ts.unhandled_resps, &resp->link);
    uv_mutex_unlock(&ditm->ts.mutex);

    imap_ditm_advance(ditm);
}

// imap_ditm_async_cb is a uv_async_cb
static void imap_ditm_async_cb(uv_async_t *async){
    async_spec_t *spec = async->data;
    imap_ditm_t *ditm = CONTAINER_OF(spec, imap_ditm_t, advance_spec);
    imap_ditm_advance_onthread(ditm);
}

// imap_ditm_async_close_cb is a async_spec_t->close_cb
static void imap_ditm_async_close_cb(async_spec_t *spec){
    imap_ditm_t *ditm = CONTAINER_OF(spec, imap_ditm_t, advance_spec);

    // shutdown, part three of three: report ourselves as dead
    ditm->mgr->dead(ditm->mgr);
}

static derr_t imap_ditm_init(imap_ditm_t *ditm, imap_pipeline_t *p,
        ssl_context_t *cli_ctx, ssl_context_t *srv_ctx, manager_i *mgr){
    derr_t e = E_OK;

    // start by zeroizing everything
    *ditm = (imap_ditm_t){0};

    ditm->pipeline = p;
    ditm->cli_ctx = cli_ctx;
    ditm->srv_ctx = srv_ctx;
    ditm->mgr = mgr;

    ditm->dn.mgr = (manager_i){
        .dying = session_dn_dying,
        .dead = session_dn_dead,
    };
    ditm->dn.ctrl = (imape_control_i){
        // start with all extensions disabled
        .exts = {0},
        .is_client = false,
        .object_cb = { .cmd = imap_ditm_add_cmd, },
    };

    ditm->up.mgr = (manager_i){
        .dying = session_up_dying,
        .dead = session_up_dead,
    };
    ditm->up.ctrl = (imape_control_i){
        // start with all extensions disabled
        .exts = {0},
        .is_client = true,
        .object_cb = { .resp = imap_ditm_add_resp, },
    };

    ditm->advance_spec = (async_spec_t){
        .close_cb = imap_ditm_async_close_cb,
    };

    ditm->error = E_OK;
    // for downwards session
    link_init(&ditm->ts.unhandled_cmds);
    link_init(&ditm->ts.returned_resps);
    // for upwards session
    link_init(&ditm->ts.unhandled_resps);
    link_init(&ditm->ts.returned_cmds);

    int ret = uv_mutex_init(&ditm->ts.mutex);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "error initializing mutex");
    }

    /* establish the uv_async_t; if that fails we must not create off-thread
       resources or they would not be able to communicate with us */
    ditm->advance_async.data = &ditm->advance_spec;
    ret = uv_async_init(&ditm->pipeline->loop->uv_loop,
            &ditm->advance_async, imap_ditm_async_cb);
    if(ret < 0){
        TRACE(&e, "uv_async_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing advance_async",
                fail_mutex);
    }

    // allocate memory for both sessions, but don't start them until later
    imap_session_alloc_args_t arg_up = {
        ditm->pipeline,
        &ditm->up.mgr,   // manager_i
        ditm->cli_ctx,   // ssl_context_t
        &ditm->up.ctrl,  // imape_control_i
        g_host,
        g_svc,
        (terminal_t){},
    };
    PROP_GO(&e, imap_session_alloc_connect(&ditm->up.s, &arg_up), fail_async);

    imap_session_alloc_args_t arg_dn = {
        ditm->pipeline,
        &ditm->dn.mgr,   // manager_i
        ditm->srv_ctx,   // ssl_context_t
        &ditm->dn.ctrl,  // imape_control_i
        g_host,
        g_svc,
        (terminal_t){},
    };
    PROP_GO(&e, imap_session_alloc_accept(&ditm->dn.s, &arg_dn), fail_sess_up);

    // now that everything is configured, it is safe to start the sessions
    ditm->ts.n_live_sessions = 2;
    imap_session_start(&ditm->dn.s);
    imap_session_start(&ditm->up.s);

    ditm->startup_completed = true;

    return e;

fail_sess_up:
    imap_session_free(&ditm->up.s);
fail_async:
    /* if we started the uv_async_t, we have to return E_OK, but trigger the
       dying and dead calls, since we have to close the uv_async_t in a cb */
    MERGE_VAR(&ditm->error, &e, "imap_ditm failed mid-initialization");
    ditm->startup_completed = false;
    ditm->dead = true;
    imap_ditm_advance(ditm);
    return E_OK;

fail_mutex:
    uv_mutex_destroy(&ditm->ts.mutex);
    // if all we allocated was the mutex, we can just return an error
    return e;
}

static void imap_ditm_free(imap_ditm_t *ditm){
    // free the sessions
    imap_session_free(&ditm->dn.s);
    imap_session_free(&ditm->up.s);
    uv_mutex_destroy(&ditm->ts.mutex);
    // async must already have been closed
    return;
}

// work_cb is a uv_work_cb
static void work_cb(uv_work_t *req){
    imap_ditm_t *ditm = CONTAINER_OF(req, imap_ditm_t, uv_work);

    // all the ditm state machine logic falls within this call
    IF_PROP(&ditm->error, imap_ditm_do_work(ditm) ){}
}

// after_work_cb is a uv_after_work_cb
static void after_work_cb(uv_work_t *req, int status){
    imap_ditm_t *ditm = CONTAINER_OF(req, imap_ditm_t, uv_work);

    // throw an error if the thread was canceled
    if(status < 0){
        derr_t e = E_OK;
        // close ditm with error
        TRACE(&e, "uv work request failed: %x\n", FUV(&status));
        TRACE_ORIG(&e, uv_err_type(status), "work request failed");
        MERGE_VAR(&ditm->error, &e, "imap_ditm work request failed");
    }

    // we are no longer executing
    ditm->executing = false;

    // check if we need to re-enqueue ourselves
    imap_ditm_advance_onthread(ditm);
}

// always called from on-thread
static void imap_ditm_maybe_enqueue(imap_ditm_t *ditm){
    if(ditm->closed) return;
    if(!imap_ditm_more_work(ditm)) return;

    // try to enqueue work
    int ret = uv_queue_work(&ditm->pipeline->loop->uv_loop, &ditm->uv_work,
            work_cb, after_work_cb);
    if(ret < 0){
        // capture error
        TRACE(&ditm->error, "uv_queue_work: %x\n", FUV(&ret));
        TRACE_ORIG(&ditm->error, uv_err_type(ret), "failed to enqueue work");

        // close both sessions
        imap_session_close(&ditm->up.s.session, E_OK);
        imap_session_close(&ditm->dn.s.session, E_OK);

        // advance again, (this time with an error)
        imap_ditm_advance_onthread(ditm);

        return;
    }

    // work is enqueued
    ditm->executing = true;

    return;
}

// must be called on-thread
static void imap_ditm_advance_onthread(imap_ditm_t *ditm){
    // if a worker is executing, do nothing; we'll check again in work_done()
    if(ditm->executing) return;

    // catch any errors produced on the ditm itself
    if(is_error(ditm->error)){
        if(!ditm->closed){
            ditm->closed = true;
            ditm->mgr->dying(ditm->mgr, ditm->error);
            PASSED(ditm->error);
            // close both sessions, except in the special startup error case
            if(ditm->startup_completed == true){
                imap_session_close(&ditm->up.s.session, E_OK);
                imap_session_close(&ditm->dn.s.session, E_OK);
            }
        }else{
            DROP_VAR(&ditm->error);
        }
    }

    // gather a dying event from the downwards session
    if(ditm->dn.close_called){
        if(!ditm->closed){
            ditm->closed = true;
            ditm->mgr->dying(ditm->mgr, ditm->dn.error);
            PASSED(ditm->dn.error);
            imap_session_close(&ditm->up.s.session, E_OK);
        }else{
            DROP_VAR(&ditm->dn.error);
        }
        ditm->dn.close_called = false;
    }

    // gather a dying event from the upwards session
    if(ditm->up.close_called){
        if(!ditm->closed){
            ditm->closed = true;
            ditm->mgr->dying(ditm->mgr, ditm->up.error);
            PASSED(ditm->up.error);
            imap_session_close(&ditm->dn.s.session, E_OK);
        }else{
            DROP_VAR(&ditm->up.error);
        }
        ditm->up.close_called = false;
    }

    // shutdown, part two of three: close the async
    if(ditm->dead){
        uv_close((uv_handle_t*)&ditm->advance_async, async_handle_close_cb);
        // end of the line for this object
        return;
    }

    // shutdown, part one of three: make the final call to imap_ditm_advance
    uv_mutex_lock(&ditm->ts.mutex);
    if(ditm->closed && ditm->ts.n_live_sessions == 0
            && ditm->ts.n_unreturned_events == 0){
        /* because these counts are thread-safe and we have locked the mutex
           and the count decrementers call async_send inside the same mutex,
           we know that there can be no more calls to async_send now.  That
           does not guarantee that there can't be some unprocessed async_sends
           floating around somewhere, and since I'm not sure what happens if
           we call uv_close on a uv_async_t with unprocessed async_sends, I am
           going to call async_send one more time here, and when that send is
           processed, we will know that no unprocessed sends remain. */
        ditm->dead = true;
        imap_ditm_advance(ditm);
        uv_mutex_unlock(&ditm->ts.mutex);
        return;
    }
    uv_mutex_unlock(&ditm->ts.mutex);

    // if there is work to do, enqueue some work
    imap_ditm_maybe_enqueue(ditm);
}

void imap_ditm_cmd_ev_returner(event_t *ev){
    imap_ditm_t *ditm = ev->returner_arg;

    uv_mutex_lock(&ditm->ts.mutex);
    link_list_append(&ditm->ts.returned_cmds, &ev->link);
    uv_mutex_unlock(&ditm->ts.mutex);
}

void imap_ditm_resp_ev_returner(event_t *ev){
    imap_ditm_t *ditm = ev->returner_arg;

    uv_mutex_lock(&ditm->ts.mutex);
    link_list_append(&ditm->ts.returned_resps, &ev->link);
    uv_mutex_unlock(&ditm->ts.mutex);
}

//// application startup and teardown

static derr_t build_pipeline(imap_pipeline_t *pipeline){
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
    PROP_GO(&e, imape_init(&imape, 5, &tlse.engine), fail);
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

static imap_ditm_t ditm = {0};

static void ditm_dying(manager_i *mgr, derr_t e){
    (void)mgr;
    LOG_ERROR("ditm dying, closing loop...\n");
    loop_close(&loop, e);
}

static void ditm_dead(manager_i *mgr){
    (void)mgr;
    imap_ditm_free(&ditm);
}

static manager_i ditm_mgr = {
    .dying = ditm_dying,
    .dead = ditm_dead,
};

static derr_t main_imap_ditm(char *host, char *svc, char *user, char *pass,
        char *keyfile){
    derr_t e = E_OK;

    // process commandline arguments
    g_host = host;
    g_svc = svc;
    (void)user;
    (void)pass;

    // init OpenSSL
    PROP(&e, ssl_library_init() );

    PROP_GO(&e, keypair_load(&keypair, keyfile), cu_ssl_lib);
    (void)keypair;

    imap_pipeline_t pipeline;
    PROP_GO(&e, build_pipeline(&pipeline), cu_keypair);

    /* After building the pipeline, we must run the pipeline if we want to
       cleanup nicely.  That means that we can't follow the normal cleanup
       pattern, and instead we must initialize all of our variables to zero */

    ssl_context_t ctx_cli = {0};
    ssl_context_t ctx_srv = {0};

    PROP_GO(&e, ssl_context_new_client(&ctx_cli), fail);
    PROP_GO(&e, ssl_context_new_server(&ctx_srv, CERT, KEY, DH), fail);
    PROP_GO(&e, imap_ditm_init(&ditm, &pipeline, &ctx_cli, &ctx_srv,
                &ditm_mgr), fail);

fail:
    if(is_error(e)){
        loop_close(&loop, e);
        // The loop will pass us this error back after loop_run.
        PASSED(e);
    }

    // run the loop
    PROP_GO(&e, loop_run(&loop), cu);

cu:
    imap_ditm_free(&ditm);
    ssl_context_free(&ctx_cli);
    free_pipeline(&pipeline);
cu_keypair:
    keypair_free(&keypair);
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
        "./sm_fetch",
        "127.0.0.1",
        "993",
        "test@splintermail.com",
        "password",
        "../c/test/files/key_tool/key_m.pem",
    };
    if(argc != 6){
        fprintf(stderr, "usage: sm_fetch HOST PORT USERNAME PASSWORD KEYFILE\n");
        if(argc != 1){
            exit(1);
        }
        argc = sizeof(default_args)/sizeof(*default_args);
        argv = default_args;
    }

    // add logger
    logger_add_fileptr(LOG_LVL_INFO, stdout);

    derr_t e = main_imap_ditm(argv[1], argv[2], argv[3], argv[4], argv[5]);
    CATCH(e, E_ANY){
        DUMP(e);
        DROP_VAR(&e);
        return 1;
    }

    return 0;
}
