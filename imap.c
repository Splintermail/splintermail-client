#include <signal.h>

#include "common.h"
#include "logger.h"
#include "loop.h"
#include "tls_engine.h"
#include "imap_engine.h"
#include "imap_session.h"
#include "uv_util.h"

#define KEY "../c/test/files/ssl/good-key.pem"
#define CERT "../c/test/files/ssl/good-cert.pem"
#define DH "../c/test/files/ssl/dh_4096.pem"

uv_idle_t idle;
loop_t loop;
imap_client_spec_t client_spec;

// // add a oneshot action to be run at the beginning of the loop
// static void oneshot_cb(uv_prepare_t *prep){
//     derr_t e = E_OK;
//
//     // Here is where something needs to happen if we are going to test imap
//     // PROP_GO(& imape_conn_out(), done);
//
//     // this is a placeholder to avoid compiler warnings
//     PROP_GO(&e, E_OK, done);
//
//     LOG_ERROR("oneshot\n");
//
// done:
//
//     // don't run this again
//     uv_prepare_stop(prep);
//     uv_close((uv_handle_t*)prep, NULL);
//
//     CATCH(E_ANY){
//         LOG_ERROR("failed to setup outgoing connection, aborting\n");
//         loop_close(&loop, e);
//         PASSED(e);
//     }
// }
//
// static derr_t add_oneshot(loop_t *loop, uv_prepare_t *oneshot){
//     derr_t e = E_OK;
//     int ret = uv_prepare_init(&loop->uv_loop, oneshot);
//     if(ret < 0){
//         ORIG(E_UV, "error initing prepare handle");
//     }
//
//     // no user data needed
//     oneshot->data = NULL;
//
//     ret = uv_prepare_start(oneshot, oneshot_cb);
//     if(ret < 0){
//         ORIG(E_UV, "error initing prepare handle");
//     }
//
//     return e;
// }
//
// static void idle_cb(uv_idle_t *handle){
//     (void)handle;
//     //printf("idling\n");
// }

typedef struct {
    imap_pipeline_t *pipeline;
    ssl_context_t *cli_ctx;
    imap_controller_up_t ctrlr_up;
    // session
    imap_session_t *s;
} fetch_controller_t;
DEF_CONTAINER_OF(fetch_controller_t, ctrlr_up, imap_controller_up_t);

static void fetch_controller_logged_in(const imap_controller_up_t *ic,
        session_t *s){
    fetch_controller_t *fc = CONTAINER_OF(ic, fetch_controller_t, ctrlr_up);
    imape_data_t *id = s->id;
    (void)id;
    (void)fc;
    printf("logged in! exiting...\n");
    loop_close(&loop, E_OK);
}

static void fetch_controller_uptodate(const imap_controller_up_t *ic,
        session_t *s){
    fetch_controller_t *fc = CONTAINER_OF(ic, fetch_controller_t, ctrlr_up);
    imape_data_t *id = s->id;
    (void)id;
    (void)fc;
}

static void fetch_controller_msg_recvd(const imap_controller_up_t *ic,
        session_t *s){
    fetch_controller_t *fc = CONTAINER_OF(ic, fetch_controller_t, ctrlr_up);
    imape_data_t *id = s->id;
    (void)id;
    (void)fc;
}

static void session_closed(imap_session_t *s, derr_t e){
    (void)s;
    DUMP(e);
    printf("session closed, exiting\n");
    loop_close(&loop, E_OK);
}

static derr_t fetch_controller_init(fetch_controller_t *fc, imap_pipeline_t *p,
        ssl_context_t *cli_ctx){
    derr_t e = E_OK;

    fc->pipeline = p;
    fc->cli_ctx = cli_ctx;
    fc->ctrlr_up = (imap_controller_up_t){
        .logged_in = fetch_controller_logged_in,
        .uptodate = fetch_controller_uptodate,
        .msg_recvd = fetch_controller_msg_recvd,
    };

    // create an initial session
    imap_client_alloc_arg_t arg = (imap_client_alloc_arg_t){
        .spec = &client_spec,
        .controller = &fc->ctrlr_up,
    };
    PROP(&e, imap_session_alloc_connect(&fc->s, fc->pipeline, fc->cli_ctx,
                client_spec.host, client_spec.service, imap_client_logic_alloc,
                &arg) );
    fc->s->session_destroyed = session_closed;
    fc->s->mgr_data = fc;
    imap_session_start(fc->s);

    return e;
};

static void fetch_controller_free(fetch_controller_t *fc){
    /* the controller should not be freed until the loop is closed, which
       should not happen until the session has closed and freed itself, so
       there is no need to free the session at all */
    (void)fc;
}


static derr_t sm_fetch(void){
    derr_t e = E_OK;
    // init OpenSSL
    PROP(&e, ssl_library_init() );

    // set UV_THREADPOOL_SIZE
    unsigned int nworkers = 2;
    PROP(&e, set_uv_threadpool_size(nworkers + 3, nworkers + 7) );

    tlse_t tlse;
    imape_t imape;

    // initialize loop
    PROP_GO(&e, loop_init(&loop, 5, 5, &tlse.engine), cu_ssl_lib);

    // intialize TLS engine
    PROP_GO(&e, tlse_init(&tlse, 5, 5, &loop.engine, &imape.engine), cu_loop);
    PROP_GO(&e, tlse_add_to_loop(&tlse, &loop.uv_loop), cu_tlse);

    // initialize IMAP engine
    PROP_GO(&e, imape_init(&imape, 5, &tlse.engine, nworkers, &loop), cu_tlse);
    PROP_GO(&e, imape_add_to_loop(&imape, &loop.uv_loop), cu_imape);

    imap_pipeline_t pipeline = {
        .loop=&loop,
        .tlse=&tlse,
        .imape=&imape,
    };

    // initialize SSL contexts
    ssl_context_t ctx_cli;
    PROP_GO(&e, ssl_context_new_client(&ctx_cli), cu_imape);

    // initialize the imap controller
    fetch_controller_t fc;
    PROP_GO(&e, fetch_controller_init(&fc, &pipeline, &ctx_cli), cu_ctx_cli);

    // add the idle to the loop

    // uv_idle_init(&loop.uv_loop, &idle);
    // uv_idle_start(&idle, idle_cb);


    // run the loop
    PROP_GO(&e, loop_run(&loop), cu_controller);

cu_controller:
    fetch_controller_free(&fc);
cu_ctx_cli:
    ssl_context_free(&ctx_cli);
cu_imape:
    imape_free(&imape);
cu_tlse:
    tlse_free(&tlse);
cu_loop:
    loop_free(&loop);
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
        "./imap",
        "127.0.0.1",
        "993",
        "test@splintermail.com",
        "password"
    };
    if(argc != 5){
        fprintf(stderr, "usage: sm_fetch HOST PORT USERNAME PASSWORD\n");
        argc = 5;
        argv = default_args;
        //exit(1);
    }

    // grab the arguments from the command line
    client_spec.host = argv[1];
    client_spec.service = argv[2];
    DSTR_WRAP(client_spec.user, argv[3], strlen(argv[3]), true);
    DSTR_WRAP(client_spec.pass, argv[4], strlen(argv[4]), true);

    // add logger
    logger_add_fileptr(LOG_LVL_INFO, stdout);

    derr_t e = sm_fetch();
    CATCH(e, E_ANY){
        DUMP(e);
        DROP_VAR(&e);
        return 1;
    }

    return 0;
}
