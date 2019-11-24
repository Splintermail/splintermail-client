#include <signal.h>

#include "common.h"
#include "logger.h"
#include "loop.h"
#include "tls_engine.h"
#include "imap_engine.h"
#include "imap_session.h"
#include "uv_util.h"
#include "hashmap.h"
#include "imap_client.h"
#include "imap_expression_print.h"
#include "jsw_atree.h"
#include "imap_utils.h"
#include "imap_dirmgr.h"

#define KEY "../c/test/files/ssl/good-key.pem"
#define CERT "../c/test/files/ssl/good-cert.pem"
#define DH "../c/test/files/ssl/dh_4096.pem"

uv_idle_t idle;
loop_t loop;
tlse_t tlse;
imape_t imape;
imap_client_spec_t client_spec;

typedef struct {
    imap_pipeline_t *pipeline;
    ssl_context_t *cli_ctx;
    imap_controller_up_t ctrlr_up;
    // user-wide state
    bool folders_set;
    hashmap_t folders;  // fc_folder_t
    // session
    imap_session_t *s;
    // dirmgr
    dstr_t path;
    dirmgr_t dirmgr;
    // list of accessors for folders we are syncing
    link_t accessors;
} fetch_controller_t;
DEF_CONTAINER_OF(fetch_controller_t, ctrlr_up, imap_controller_up_t);

static void fc_free_cmd_event(event_t *ev){
    cmd_event_t *cmd_ev = CONTAINER_OF(ev, cmd_event_t, ev);
    dstr_free(&cmd_ev->ev.buffer);
    free(cmd_ev);
}

static cmd_event_t *fc_new_cmd_event(void){
    cmd_event_t *cmd_ev = malloc(sizeof(*cmd_ev));
    if(!cmd_ev) return NULL;
    *cmd_ev = (cmd_event_t){0};

    event_prep(&cmd_ev->ev, fc_free_cmd_event, NULL);
    cmd_ev->ev.ev_type = EV_COMMAND;

    return cmd_ev;
}

typedef struct {
    dstr_t name;
    bool toggle;
    hash_elem_t *h;
} fc_folder_t;
DEF_CONTAINER_OF(fc_folder_t, h, hash_elem_t);

static void fc_logged_in(const imap_controller_up_t *ic,
        session_t *session){
    derr_t e = E_OK;

    fetch_controller_t *fc = CONTAINER_OF(ic, fetch_controller_t, ctrlr_up);
    imap_session_t *s = CONTAINER_OF(session, imap_session_t, session);

    if(!fc->folders_set){
        /* the first session without folders fetches the folder list for us.
           We should not have multiple connections in flight yet, so we don't
           have to worry about race conditions */
        cmd_event_t *cmd_ev = fc_new_cmd_event();
        if(!cmd_ev) ORIG_GO(&e, E_NOMEM, "no memory", fail);
        cmd_ev->ev.session = &s->session;
        cmd_ev->cmd_type = IMAP_CLIENT_CMD_LIST_FOLDERS;
        imap_session_send_command(s, &cmd_ev->ev);
    }else{
        printf("already have folder list, exiting\n");
        loop_close(&loop, E_OK);
    }

    return;

fail:
    loop_close(&loop, e);
    PASSED(e);
}

static void fc_uptodate(const imap_controller_up_t *ic,
        session_t *session){
    fetch_controller_t *fc = CONTAINER_OF(ic, fetch_controller_t, ctrlr_up);
    imape_data_t *id = session->id;
    (void)id;
    (void)fc;
}

static void fc_msg_recvd(const imap_controller_up_t *ic,
        session_t *session){
    fetch_controller_t *fc = CONTAINER_OF(ic, fetch_controller_t, ctrlr_up);
    imape_data_t *id = session->id;
    (void)id;
    (void)fc;
}

static void fc_folders(const imap_controller_up_t *ic,
        session_t *session, jsw_atree_t *folders){
    fetch_controller_t *fc = CONTAINER_OF(ic, fetch_controller_t, ctrlr_up);
    imape_data_t *id = session->id;
    (void)id;
    (void)fc;

    fc->folders_set = true;

    derr_t e = E_OK;

    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, folders);
    for(; node != NULL; node = jsw_atnext(&trav)){
        ie_list_resp_t *list = CONTAINER_OF(node, ie_list_resp_t, node);

        // verify that the separator is actually "/"
        if(list->sep != '/'){
            TRACE(&e, "Got folder separator of %x but only / is supported\n",
                    FC(list->sep));
            ORIG_GO(&e, E_RESPONSE, "invalid folder separator", fail);
        }

        // build a path to the folder
        // string_builder_t path = sb_append(&SB(FD(&fc->path)), &buf);

        DSTR_VAR(buf, 64);
        DROP_CMD( print_list_resp(&buf, list) );
        DROP_CMD( PFMT("%x\n", FD(&buf)) );
    }

    PROP_GO(&e, dirmgr_sync_folders(&fc->dirmgr, folders), fail);

    return;

fail:
    loop_close(&loop, e);
    PASSED(e);
}

static void session_closed(imap_session_t *session, derr_t e){
    (void)session;
    DUMP(e);
    printf("session closed, exiting\n");
    loop_close(&loop, E_OK);
}

static derr_t fc_init(fetch_controller_t *fc, imap_pipeline_t *p,
        ssl_context_t *cli_ctx){
    derr_t e = E_OK;

    fc->pipeline = p;
    fc->cli_ctx = cli_ctx;
    fc->ctrlr_up = (imap_controller_up_t){
        .logged_in = fc_logged_in,
        .uptodate = fc_uptodate,
        .msg_recvd = fc_msg_recvd,
        .folders = fc_folders,
    };

    fc->folders_set = false;

    // allocate for the path
    PROP(&e, dstr_new(&fc->path, 256) );
    // right now the path is not configurable
    PROP_GO(&e, dstr_copy(&DSTR_LIT("/tmp/maildir_root"), &fc->path),
            fail_path);

    // dirmgr
    PROP_GO(&e, dirmgr_init(&fc->dirmgr, SB(FD(&fc->path))), fail_path);

    // create an initial session
    imap_client_alloc_arg_t arg = (imap_client_alloc_arg_t){
        .spec = &client_spec,
        .controller = &fc->ctrlr_up,
    };
    PROP_GO(&e, imap_session_alloc_connect(&fc->s, fc->pipeline, fc->cli_ctx,
                client_spec.host, client_spec.service, imap_client_logic_alloc,
                &arg), fail_dirmgr);
    fc->s->session_destroyed = session_closed;
    fc->s->mgr_data = fc;
    imap_session_start(fc->s);

    return e;

fail_dirmgr:
    dirmgr_free(&fc->dirmgr);
fail_path:
    dstr_free(&fc->path);
    return e;
};

static void fc_free(fetch_controller_t *fc){
    /* the controller should not be freed until the loop is closed, which
       should not happen until the session has closed and freed itself, so
       there is no need to free the fc->s at all */
    dirmgr_free(&fc->dirmgr);
    dstr_free(&fc->path);
}


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
    PROP_GO(&e, imape_init(&imape, 5, &tlse.engine, nworkers, &loop), fail);
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


static derr_t sm_fetch(void){
    derr_t e = E_OK;
    // init OpenSSL
    PROP(&e, ssl_library_init() );

    imap_pipeline_t pipeline;
    PROP_GO(&e, build_pipeline(&pipeline), cu_ssl_lib);

    /* After building the pipeline, we must run the pipeline if we want to
       cleanup nicely.  That means that we can't follow the normal cleanup
       pattern, and instead we must initialize all of our variables to zero */

    ssl_context_t ctx_cli = {0};
    fetch_controller_t fc = {0};

    PROP_GO(&e, ssl_context_new_client(&ctx_cli), fail);
    PROP_GO(&e, fc_init(&fc, &pipeline, &ctx_cli), fail);

fail:
    if(is_error(e)){
        loop_close(&loop, e);
        // The loop will pass us this error back after loop_run.
        PASSED(e);
    }

    // run the loop
    PROP_GO(&e, loop_run(&loop), cu);

cu:
    fc_free(&fc);
    ssl_context_free(&ctx_cli);
    free_pipeline(&pipeline);
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
