#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdlib.h>

#include <libdstr/common.h>
#include <libdstr/logger.h>
#include <ditm.h>
#include <networking.h>
#include <libdstr/fileops.h>

#include "ditm_thread.h"

#include <libdstr/win_compat.h>

// if the thread fails
static derr_t thread_error = E_OK;
// the result of just one test
static derr_t test_error = E_OK;

// path to where the test files can be found
extern const char* g_test_files;
const char* ditm_path = "_ditm_thread_ditm_dir";

static pthread_t ditm_thread;
static pthread_mutex_t ditm_mutex;
static pthread_cond_t ditm_cond;

unsigned int ditm_thread_pop_port;

static bool keep_going = true;

static bool test_start = false;
void ditm_thread_start_test(void){
    pthread_mutex_lock(&ditm_mutex);
    test_start = true;
    pthread_cond_signal(&ditm_cond);
    pthread_mutex_unlock(&ditm_mutex);
}

void ditm_thread_done(void){
    pthread_mutex_lock(&ditm_mutex);
    test_start = true;
    keep_going = false;
    pthread_cond_signal(&ditm_cond);
    pthread_mutex_unlock(&ditm_mutex);
}

static bool test_end = false;
derr_t ditm_thread_end_test(void){
    pthread_mutex_lock(&ditm_mutex);
    if(!test_end){
        // wait for ditm_thread to finish with its connection
        pthread_cond_wait(&ditm_cond, &ditm_mutex);
    }
    test_end = false;
    pthread_mutex_unlock(&ditm_mutex);
    return test_error;
}

static void* ditm_thread_func(void* arg){
    // suppress unused argument warning
    (void) arg;
    derr_t e = E_OK;
    // prepare server ssl context
    DSTR_VAR(certfile, 4096);
    DSTR_VAR(keyfile, 4096);
    DSTR_VAR(dhfile, 4096);
    PROP_GO(&e, FMT(&certfile, "%x/%x", FS(g_test_files), FS("ssl/good-cert.pem")), cleanup_1);
    PROP_GO(&e, FMT(&keyfile, "%x/%x", FS(g_test_files), FS("ssl/good-key.pem")), cleanup_1);
    PROP_GO(&e, FMT(&dhfile, "%x/%x", FS(g_test_files), FS("ssl/dh_4096.pem")), cleanup_1);
    ssl_context_t s_ctx;
    PROP_GO(&e, ssl_context_new_server(&s_ctx, certfile.data,
                                            keyfile.data,
                                            dhfile.data), cleanup_1);

    // prepare client ssl context
    ssl_context_t c_ctx;
    PROP_GO(&e, ssl_context_new_client(&c_ctx), cleanup_2);

    listener_t listener;
    PROP_GO(&e, listener_new_ssl(&listener, &s_ctx, "127.0.0.1", 1996), cleanup_3);
    // signal the main thread
    pthread_mutex_lock(&ditm_mutex);
    pthread_cond_signal(&ditm_cond);
    pthread_mutex_unlock(&ditm_mutex);

    while(keep_going){
        // wait for main thread to let us start
        pthread_mutex_lock(&ditm_mutex);
        if(!test_start){
            pthread_cond_wait(&ditm_cond, &ditm_mutex);
        }
        test_start = false;
        pthread_mutex_unlock(&ditm_mutex);
        // make sure we haven't been cancelled
        if(!keep_going) break;
        connection_t conn;
        // accept a connection
        LOG_INFO("DITM about to accept()\n");
        PROP_GO(&e, listener_accept(&listener, &conn), cleanup_4);
        LOG_INFO("DITM accepted()\n");

        // create a ditm object for this connection
        ditm_t ditm;

        PROP_GO(&e, ditm_new(&ditm, &c_ctx, &conn, "127.0.0.1", ditm_thread_pop_port,
                          ditm_path, "127.0.0.1", 443), cleanup_5);
        // kick off server loop
        PROP_GO(&e, pop_server_loop(&ditm.ps, (void*)(&ditm)), cleanup_6);

cleanup_6:
        // done with ditm, cleanup
        ditm_free(&ditm);
cleanup_5:
        connection_close(&conn);
        LOG_INFO("DITM thread connection closed\n");

        // end-of-test
        pthread_mutex_lock(&ditm_mutex);
        // save the error from this test
        test_error = e;
        e = E_OK;
        test_end = true;
        pthread_cond_signal(&ditm_cond);
        pthread_mutex_unlock(&ditm_mutex);
    }

cleanup_4:
    listener_close(&listener);
cleanup_3:
    ssl_context_free(&c_ctx);
cleanup_2:
    ssl_context_free(&s_ctx);
cleanup_1:
    thread_error = e;
    // signal the main thread in case of early exit errors
    pthread_mutex_lock(&ditm_mutex);
    pthread_cond_signal(&ditm_cond);
    pthread_mutex_unlock(&ditm_mutex);
    LOG_INFO("test_ditm exiting normally\n");
    return NULL;
}

derr_t ditm_thread_start(unsigned int pop_port){
    ditm_thread_pop_port = pop_port;
    derr_t e = E_OK;
    // prepare for the cond_wait
    pthread_cond_init(&ditm_cond, NULL);
    pthread_mutex_init(&ditm_mutex, NULL);

    // lock so the ditm server doesn't signal before we are waiting
    pthread_mutex_lock(&ditm_mutex);
    // start the server
    pthread_create(&ditm_thread, NULL, ditm_thread_func, NULL);
    // now wait for the server to be ready
    pthread_cond_wait(&ditm_cond, &ditm_mutex);
    // unlock mutex
    pthread_mutex_unlock(&ditm_mutex);
    // if the server is about to fail, don't hang waiting to connect to it
    if(thread_error.type){
        ORIG_GO(&e, thread_error.type, "DITM thread failed to start", fail);
    }
    return e;

fail:
    pthread_join(ditm_thread, NULL);
    pthread_mutex_destroy(&ditm_mutex);
    pthread_cond_destroy(&ditm_cond);
    return e;
}

derr_t ditm_thread_join(void){
    pthread_join(ditm_thread, NULL);
    pthread_mutex_destroy(&ditm_mutex);
    pthread_cond_destroy(&ditm_cond);
    return thread_error;
}
