#include <signal.h>

#include "common.h"
#include "logger.h"
#include "loop.h"
#include "ixs.h"
#include "ix.h"

#define KEY "../c/test/files/ssl/good-key.pem"
#define CERT "../c/test/files/ssl/good-cert.pem"
#define DH "../c/test/files/ssl/dh_4096.pem"

static loop_t loop;


static derr_t test_imap(void){
    derr_t error;
    // init OpenSSL
    PROP( ssl_library_init() );

    // initialize loop
    PROP_GO( loop_init(&loop), cu_ssl_lib);

    // initialize SSL contexts
    ssl_context_t ctx_srv;
    ssl_context_t ctx_cli;
    PROP_GO( ssl_context_new_client(&ctx_cli), cu_loop);
    PROP_GO( ssl_context_new_server(&ctx_srv, CERT, KEY, DH), cu_ctx_cli);

    // for the listener sockets
    ix_t ix_listener = { .type = IX_TYPE_LISTENER, .data = {.ssl_ctx = &ctx_srv }};

    // add listener to loop
    PROP_GO( loop_add_listener(&loop, "0.0.0.0", "12345", &ix_listener),
             cu_ctx_srv);

    // run the loop
    PROP_GO( loop_run(&loop), cu_ctx_srv);

    // the loop handles closing sockets / listeners / sessions before exiting

cu_ctx_srv:
    ssl_context_free(&ctx_srv);
cu_ctx_cli:
    ssl_context_free(&ctx_cli);
cu_loop:
    loop_free(&loop);
cu_ssl_lib:
    ssl_library_close();
    return error;
}


static bool hard_exit = false;
static void stop_loop_on_signal(int signum){
    (void) signum;
    LOG_ERROR("caught signal\n");
    if(hard_exit) exit(1);
    hard_exit = true;
    // launch an asynchronous loop abort
    loop_abort(&loop);
}


int main(void){
    // ignore SIGPIPE, required to work with OpenSSL
    // see https://mta.openssl.org/pipermail/openssl-users/2017-May/005776.html
    // (but SIGPIPE doesnt exist in windows)
    signal(SIGINT, stop_loop_on_signal);
    signal(SIGTERM, stop_loop_on_signal);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    // add logger
    logger_add_fileptr(LOG_LVL_DEBUG, stdout);

    derr_t error = test_imap();
    CATCH(E_ANY){
        return 1;
    }

    return 0;
}

