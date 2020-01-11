#include <signal.h>

#include "sm_fetch.h"
#include "logger.h"
#include "uv_util.h"
#include "hashmap.h"
#include "jsw_atree.h"
#include "crypto.h"

#define KEY "../c/test/files/ssl/good-key.pem"
#define CERT "../c/test/files/ssl/good-cert.pem"
#define DH "../c/test/files/ssl/dh_4096.pem"

uv_idle_t idle;
loop_t loop;
tlse_t tlse;
imape_t imape;
keypair_t keypair;
// fetcher has to be zeroized for proper cleanup if build_pipeline fails
fetcher_t fetcher = {0};

// engine interface
static void fetcher_pass_event(struct engine_t *engine, event_t *ev){
    fetcher_t *fetcher = CONTAINER_OF(engine, fetcher_t, engine);

    imap_event_t *imap_ev;

    switch(ev->ev_type){
        case EV_READ:
            imap_ev = CONTAINER_OF(ev, imap_event_t, ev);
            // fetcher only accepts imap_resp_t's as EV_READs
            if(imap_ev->type != IMAP_EVENT_TYPE_RESP){
                LOG_ERROR("unallowed imap_cmd_t as EV_READ in fetcher\n");
                ev->returner(ev);
                break;
            }

            // steal the imap_resp_t and return the rest of the event
            imap_resp_t *resp = imap_ev->arg.resp;
            imap_ev->arg.resp = NULL;
            ev->returner(ev);

            // queue up the response
            uv_mutex_lock(&fetcher->ts.mutex);
            link_list_append(&fetcher->ts.unhandled_resps, &resp->link);
            uv_mutex_unlock(&fetcher->ts.mutex);
            break;

        case EV_QUIT_DOWN:
            // store the QUIT event for when we are fully dead
            fetcher->quit_ev = ev;

            // make sure we are actually quitting
            // TODO: clean up the error reporting path
            fetcher_close(fetcher, E_OK);
            break;

        default:
            LOG_ERROR("unallowed event type (%x)\n", FU(ev->ev_type));
    }

    // trigger more work
    fetcher_advance(fetcher);
}

// forward declarations
static void fetcher_advance_onthread(fetcher_t *fetcher);

// callable from any thread
void fetcher_advance(fetcher_t *fetcher){
    // alert the loop thread to try to enqueue the worker
    int ret = uv_async_send(&fetcher->advance_async);
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
    fetcher_session_t *up = CONTAINER_OF(mgr, fetcher_session_t, mgr);
    fetcher_t *fetcher = CONTAINER_OF(up, fetcher_t, up);
    printf("session up dying\n");

    fetcher_close(fetcher, e);
    PASSED(e);

    // any further steps must be done on-thread
    fetcher_advance(fetcher);
}

static void session_up_dead(manager_i *mgr){
    fetcher_session_t *up = CONTAINER_OF(mgr, fetcher_session_t, mgr);
    fetcher_t *fetcher = CONTAINER_OF(up, fetcher_t, up);

    uv_mutex_lock(&fetcher->ts.mutex);
    fetcher->ts.n_live_sessions--;
    uv_mutex_unlock(&fetcher->ts.mutex);

    fetcher_advance(fetcher);
}

// fetcher_async_cb is a uv_async_cb
static void fetcher_async_cb(uv_async_t *async){
    async_spec_t *spec = async->data;
    fetcher_t *fetcher = CONTAINER_OF(spec, fetcher_t, advance_spec);
    fetcher_advance_onthread(fetcher);
}

// fetcher_async_close_cb is a async_spec_t->close_cb
static void fetcher_async_close_cb(async_spec_t *spec){
    fetcher_t *fetcher = CONTAINER_OF(spec, fetcher_t, advance_spec);

    // shutdown, part three of three: report ourselves as dead
    fetcher->mgr->dead(fetcher->mgr);

    // now we can continue the QUIT sequence by passing along the QUIT_UP
    fetcher->quit_ev->ev_type = EV_QUIT_UP;
    engine_t *upstream = &fetcher->pipeline->imape->engine;
    upstream->pass_event(upstream, fetcher->quit_ev);
    fetcher->quit_ev = NULL;
}

// part of the imaildir_i, meaning this can be called on- or off-thread
static void fetcher_conn_up_cmd(maildir_conn_up_i *conn_up, imap_cmd_t *cmd){
    fetcher_t *fetcher = CONTAINER_OF(conn_up, fetcher_t, conn_up);

    uv_mutex_lock(&fetcher->ts.mutex);
    link_list_append(&fetcher->ts.maildir_cmds, &cmd->link);
    uv_mutex_unlock(&fetcher->ts.mutex);

    fetcher_advance(fetcher);
}

// part of the imaildir_i, meaning this can be called on- or off-thread
static void fetcher_conn_up_synced(maildir_conn_up_i *conn_up){
    fetcher_t *fetcher = CONTAINER_OF(conn_up, fetcher_t, conn_up);
    fetcher->mailbox_synced = true;

    // we don't want to hold a folder open after it is synced, so unselect now
    derr_t e = E_OK;
    IF_PROP(&e, fetcher->maildir->unselect(fetcher->maildir) ){
        fetcher_close(fetcher, e);
    }

    fetcher_advance(fetcher);
}

// part of the imaildir_i, meaning this can be called on- or off-thread
static void fetcher_conn_up_unselected(maildir_conn_up_i *conn_up){
    fetcher_t *fetcher = CONTAINER_OF(conn_up, fetcher_t, conn_up);
    fetcher->mailbox_unselected = true;
    // we still can't close the maildir until we are onthread

    fetcher_advance(fetcher);
}

// part of the imaildir_i, meaning this can be called on- or off-thread
static void fetcher_conn_up_release(maildir_conn_up_i *conn_up, derr_t error){
    derr_t e = E_OK;

    fetcher_t *fetcher = CONTAINER_OF(conn_up, fetcher_t, conn_up);

    IF_PROP_VAR(&e, &error){
        // maildir is failing with an error
        fetcher_close(fetcher, e);
        PASSED(e);
    }else{
        // it's still an error if the maildir releases us before we release it
        if(fetcher->maildir != NULL){
            TRACE_ORIG(&e, E_INTERNAL, "maildir closed unexpectedly");
            fetcher_close(fetcher, e);
            PASSED(e);
        }
    }

    fetcher->maildir_has_ref = false;

    fetcher_advance(fetcher);
}

static derr_t fetcher_init(fetcher_t *fetcher, const char *host,
        const char *svc, const dstr_t *user, const dstr_t *pass,
        imap_pipeline_t *p, ssl_context_t *cli_ctx, manager_i *mgr){
    derr_t e = E_OK;

    // start by zeroizing everything and storing static values
    *fetcher = (fetcher_t){
        .host = host,
        .svc = svc,
        .user = user,
        .pass = pass,
        .pipeline = p,
        .cli_ctx = cli_ctx,
        .mgr = mgr,

        .engine = {
            .pass_event = fetcher_pass_event,
        },
    };

    fetcher->up.mgr = (manager_i){
        .dying = session_up_dying,
        .dead = session_up_dead,
    };
    fetcher->up.ctrl = (imape_control_i){
        // enable UIDPLUS, ENABLE, CONDSTORE, and QRESYNC
        .exts = {
            .uidplus = EXT_STATE_ON,
            .enable = EXT_STATE_ON,
            .condstore = EXT_STATE_ON,
            .qresync = EXT_STATE_ON,
        },
        .is_client = true,
    };

    fetcher->advance_spec = (async_spec_t){
        .close_cb = fetcher_async_close_cb,
    };

    fetcher->conn_up = (maildir_conn_up_i){
        .cmd = fetcher_conn_up_cmd,
        .synced = fetcher_conn_up_synced,
        .unselected = fetcher_conn_up_unselected,
        .release = fetcher_conn_up_release,
    };

    link_init(&fetcher->ts.unhandled_resps);
    link_init(&fetcher->ts.maildir_cmds);
    link_init(&fetcher->inflight_cmds);

    jsw_ainit(&fetcher->folders, ie_list_resp_cmp, ie_list_resp_get);

    // allocate for the path
    PROP(&e, dstr_new(&fetcher->path, 256) );
    // right now the path is not configurable
    PROP_GO(&e, dstr_copy(&DSTR_LIT("/tmp/maildir_root"), &fetcher->path),
            fail_path);

    // dirmgr
    PROP_GO(&e, dirmgr_init(&fetcher->dirmgr, SB(FD(&fetcher->path))),
            fail_path);

    int ret = uv_mutex_init(&fetcher->ts.mutex);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing mutex", fail_dirmgr);
    }

    /* establish the uv_async_t; if that fails we must not create off-thread
       resources or they would not be able to communicate with us */

    // allocate memory for both sessions, but don't start them until later
    imap_session_alloc_args_t arg_up = {
        fetcher->pipeline,
        &fetcher->up.mgr,   // manager_i
        fetcher->cli_ctx,   // ssl_context_t
        &fetcher->up.ctrl,  // imape_control_i
        fetcher->host,
        fetcher->svc,
        (terminal_t){},
    };
    PROP_GO(&e, imap_session_alloc_connect(&fetcher->up.s, &arg_up),
            fail_mutex);

    /* establish the uv_async_t as the last step. If this fails, we can't
       communicate with off-thread resources or they would not be able to
       communicate with us.  However, if something else fails, we can only
       close the uv_async_t asynchronously, so we make sure this is the last
       step that can possibly fail. */
    fetcher->advance_async.data = &fetcher->advance_spec;
    ret = uv_async_init(&fetcher->pipeline->loop->uv_loop,
            &fetcher->advance_async, fetcher_async_cb);
    if(ret < 0){
        TRACE(&e, "uv_async_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing advance_async",
                fail_session);
    }


    // now that everything is configured, it is safe to start the sessions
    fetcher->ts.n_live_sessions = 1;
    imap_session_start(&fetcher->up.s);

    return e;

fail_session:
    imap_session_free(&fetcher->up.s);
fail_mutex:
    uv_mutex_destroy(&fetcher->ts.mutex);
fail_dirmgr:
    dirmgr_free(&fetcher->dirmgr);
fail_path:
    dstr_free(&fetcher->path);
    return e;
}

static void fetcher_free(fetcher_t *fetcher){
    // free the folders list
    jsw_anode_t *node;
    while((node = jsw_apop(&fetcher->folders))){
        ie_list_resp_t *list = CONTAINER_OF(node, ie_list_resp_t, node);
        ie_list_resp_free(list);
    }
    // free any imap cmds or resps laying around
    link_t *link;
    while((link = link_list_pop_first(&fetcher->ts.unhandled_resps))){
        imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);
        imap_resp_free(resp);
    }
    while((link = link_list_pop_first(&fetcher->ts.maildir_cmds))){
        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
        imap_cmd_free(cmd);
    }
    // async must already have been closed
    imap_session_free(&fetcher->up.s);
    uv_mutex_destroy(&fetcher->ts.mutex);
    dirmgr_free(&fetcher->dirmgr);
    dstr_free(&fetcher->path);
    return;
}

// work_cb is a uv_work_cb
static void work_cb(uv_work_t *req){
    fetcher_t *fetcher = CONTAINER_OF(req, fetcher_t, uv_work);

    // all the fetcher state machine logic falls within this call
    derr_t e = E_OK;
    IF_PROP(&e, fetcher_do_work(fetcher) ){
        fetcher_close(fetcher, e);
        PASSED(e);
    }
}

// after_work_cb is a uv_after_work_cb
static void after_work_cb(uv_work_t *req, int status){
    fetcher_t *fetcher = CONTAINER_OF(req, fetcher_t, uv_work);

    // throw an error if the thread was canceled
    if(status < 0){
        derr_t e = E_OK;
        // close fetcher with error
        TRACE(&e, "uv work request failed: %x\n", FUV(&status));
        TRACE_ORIG(&e, uv_err_type(status), "work request failed");
        fetcher_close(fetcher, e);
        PASSED(e);
    }

    // we are no longer executing
    fetcher->executing = false;

    // check if we need to re-enqueue ourselves
    fetcher_advance_onthread(fetcher);
}

// always called from on-thread
static void fetcher_maybe_enqueue(fetcher_t *fetcher){
    if(fetcher->ts.closed) return;
    if(!fetcher_more_work(fetcher)) return;

    // try to enqueue work
    int ret = uv_queue_work(&fetcher->pipeline->loop->uv_loop, &fetcher->uv_work,
            work_cb, after_work_cb);
    if(ret < 0){
        // capture error
        derr_t e = E_OK;
        TRACE(&e, "uv_queue_work: %x\n", FUV(&ret));
        TRACE_ORIG(&e, uv_err_type(ret), "failed to enqueue work");

        fetcher_close(fetcher, e);
        PASSED(e);

        return;
    }

    // work is enqueued
    fetcher->executing = true;

    return;
}

// safe to call many times from any thread
void fetcher_close(fetcher_t *fetcher, derr_t error){
    uv_mutex_lock(&fetcher->ts.mutex);
    // only execute the close sequence once
    bool do_close = !fetcher->ts.closed;
    fetcher->ts.closed = true;
    uv_mutex_unlock(&fetcher->ts.mutex);

    if(!do_close){
        // secondary errors get dropped
        DROP_VAR(&error);
        return;
    }

    // TODO: clean up ownership hierarchy and error reporting
    TRACE_PROP(&error);
    loop_close(fetcher->pipeline->loop, error);
    PASSED(error);

    // tell our owner we are dying
    fetcher->mgr->dying(fetcher->mgr, E_OK);

    // we can close multithreaded resources here but we can't release refs
    imap_session_close(&fetcher->up.s.session, E_OK);

    // everything else must be done on-thread
    fetcher_advance(fetcher);
}

// only safe to call once
static void fetcher_close_onthread(fetcher_t *fetcher){
    /* now that we are onthread, it is safe to release refs for things we don't
       own ourselves */
    if(fetcher->maildir){
        // extract the maildir from fetcher so it is clear we are done
        maildir_i *maildir = fetcher->maildir;
        fetcher->maildir = NULL;
        // now make the very last call into the maildir_i
        dirmgr_close_up(&fetcher->dirmgr, maildir, &fetcher->conn_up);
    }
}

// must be called on-thread
static void fetcher_advance_onthread(fetcher_t *fetcher){
    // if a worker is executing, do nothing; we'll check again in work_done()
    if(fetcher->executing) return;

    if(fetcher->ts.closed && !fetcher->closed_onthread){
        fetcher->closed_onthread = true;
        fetcher_close_onthread(fetcher);
    }

    // shutdown, part two of three: close the async
    if(fetcher->dead){
        uv_close((uv_handle_t*)&fetcher->advance_async, async_handle_close_cb);
        // end of the line for this object
        return;
    }

    // shutdown, part one of three: make the final call to fetcher_advance
    if(fetcher->closed_onthread && fetcher->ts.n_live_sessions == 0
            && fetcher->ts.n_unreturned_events == 0
            && !fetcher->maildir_has_ref
            && fetcher->quit_ev){
        /* because these counts are thread-safe and we have locked the mutex
           and the count decrementers call async_send inside the same mutex,
           we know that there can be no more calls to async_send now.  That
           does not guarantee that there can't be some unprocessed async_sends
           floating around somewhere, and since I'm not sure what happens if
           we call uv_close on a uv_async_t with unprocessed async_sends, I am
           going to call async_send one more time here, and when that send is
           processed, we will know that no unprocessed sends remain. */
        fetcher->dead = true;
        fetcher_advance(fetcher);
        return;
    }

    // if there is work to do, enqueue some work
    fetcher_maybe_enqueue(fetcher);
}

////////////////


static derr_t build_pipeline(imap_pipeline_t *pipeline, fetcher_t *fetcher){
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
    PROP_GO(&e, imape_init(&imape, 5, &tlse.engine, &fetcher->engine), fail);
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


static void fetcher_dying(manager_i *mgr, derr_t e){
    (void)mgr;
    // TODO: this error handling is so fucking broken...
    DROP_VAR(&e);
    LOG_ERROR("fetcher dying...\n");
}

static void fetcher_dead(manager_i *mgr){
    (void)mgr;
    // fetcher is freed after loop shuts down
    LOG_ERROR("fetcher dead...\n");
}

static manager_i fetcher_mgr = {
    .dying = fetcher_dying,
    .dead = fetcher_dead,
};


static derr_t sm_fetch(char *host, char *svc, char *user, char *pass,
        char *keyfile){
    derr_t e = E_OK;

    // wrap commandline arguments
    dstr_t d_user;
    DSTR_WRAP(d_user, user, strlen(user), true);
    dstr_t d_pass;
    DSTR_WRAP(d_pass, pass, strlen(pass), true);

    // init OpenSSL
    PROP(&e, ssl_library_init() );

    PROP_GO(&e, keypair_load(&keypair, keyfile), cu_ssl_lib);

    imap_pipeline_t pipeline;
    PROP_GO(&e, build_pipeline(&pipeline, &fetcher), cu_keypair);

    /* After building the pipeline, we must run the pipeline if we want to
       cleanup nicely.  That means that we can't follow the normal cleanup
       pattern, and instead we must initialize all of our variables to zero */

    ssl_context_t ctx_cli = {0};

    PROP_GO(&e, ssl_context_new_client(&ctx_cli), fail);
    PROP_GO(&e, fetcher_init(&fetcher, host, svc, &d_user, &d_pass, &pipeline,
                &ctx_cli, &fetcher_mgr), fail);

fail:
    if(is_error(e)){
        loop_close(&loop, e);
        // The loop will pass us this error back after loop_run.
        PASSED(e);
    }

    // run the loop
    PROP_GO(&e, loop_run(&loop), cu);

cu:
    fetcher_free(&fetcher);
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

    derr_t e = sm_fetch(argv[1], argv[2], argv[3], argv[4], argv[5]);
    CATCH(e, E_ANY){
        DUMP(e);
        DROP_VAR(&e);
        return 1;
    }

    return 0;
}
