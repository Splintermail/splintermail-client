#define HAVE_STRUCT_TIMESPEC
#include <pthread.h>

#include <common.h>
#include <logger.h>
#include <pop_server.h>
#include <networking.h>
#include <fileops.h>

#include "test_utils.h"

#include <win_compat.h>

// for keeping track of which hooks were called
typedef enum{
    CLEARED,
    LOGINHOOK,
    STATHOOK,
    LISTHOOK,
    RETRHOOK,
    DELEHOOK,
    RSETHOOK,
    TOPHOOK,
    UIDLHOOK,
    QUITHOOK,
} hook_t;

static hook_t called = CLEARED;
static hook_t last_called = CLEARED;
static long int g_index;
static unsigned int g_lines;

static pthread_mutex_t server_mutex;
static pthread_cond_t server_cond;

// path to where the test files can be found
static const char* g_test_files;

// some responses that our fake server will send and our test will expect to recieve
DSTR_STATIC(stat_resp, "stat_resp\r\n");
DSTR_STATIC(list_resp, "list_resp\r\n");
DSTR_STATIC(retr_resp, "retr_resp\r\n");
DSTR_STATIC(dele_resp, "dele_resp\r\n");
DSTR_STATIC(rset_resp, "rset_resp\r\n");
DSTR_STATIC(top_resp, "top_resp\r\n");
DSTR_STATIC(uidl_resp, "uidl_resp\r\n");

// easy way to return value from a thread
static derr_t thread_return;

static derr_t loginhook(void* arg, const dstr_t* username,
                        const dstr_t* password, bool* login_ok){
    (void)arg;
    (void)username;
    (void)password;
    derr_t e = E_OK;
    last_called = called;
    called = LOGINHOOK;
    *login_ok = true;
    return E_OK;
}
static derr_t stathook(void* arg){
    (void)arg;
    derr_t e = E_OK;
    last_called = called;
    called = STATHOOK;
    PROP(e, pop_server_send_dstr(((pop_server_t*)arg), &stat_resp) );
    return E_OK;
}
static derr_t listhook(void* arg, int index){
    (void)arg;
    derr_t e = E_OK;
    last_called = called;
    called = LISTHOOK;
    g_index = index;
    PROP(e, pop_server_send_dstr(((pop_server_t*)arg), &list_resp) );
    return E_OK;
}
static derr_t retrhook(void* arg, unsigned int index){
    (void)arg;
    derr_t e = E_OK;
    last_called = called;
    called = RETRHOOK;
    g_index = index;
    PROP(e, pop_server_send_dstr(((pop_server_t*)arg), &retr_resp) );
    return E_OK;
}
static derr_t delehook(void* arg, unsigned int index){
    (void)arg;
    derr_t e = E_OK;
    last_called = called;
    called = DELEHOOK;
    g_index = index;
    PROP(e, pop_server_send_dstr(((pop_server_t*)arg), &dele_resp) );
    return E_OK;
}
static derr_t rsethook(void* arg){
    (void)arg;
    derr_t e = E_OK;
    last_called = called;
    called = RSETHOOK;
    PROP(e, pop_server_send_dstr(((pop_server_t*)arg), &rset_resp) );
    return E_OK;
}
static derr_t tophook(void* arg, unsigned int index, unsigned int lines){
    (void)arg;
    derr_t e = E_OK;
    last_called = called;
    called = TOPHOOK;
    g_index = index;
    g_lines = lines;
    PROP(e, pop_server_send_dstr(((pop_server_t*)arg), &top_resp) );
    return E_OK;
}
static derr_t uidlhook(void* arg, int index){
    (void)arg;
    derr_t e = E_OK;
    last_called = called;
    called = UIDLHOOK;
    g_index = index;
    PROP(e, pop_server_send_dstr(((pop_server_t*)arg), &uidl_resp) );
    return E_OK;
}
static derr_t quithook(void* arg, bool* update_ok){
    (void)arg;
    derr_t e = E_OK;
    last_called = called;
    called = QUITHOOK;
    *update_ok = true;
    return E_OK;
}

static void* server_thread(void* arg){
    (void)arg;
    derr_t e = E_OK;
    // prepare ssl context
    DSTR_VAR(certfile, 4096);
    DSTR_VAR(keyfile, 4096);
    DSTR_VAR(dhfile, 4096);
    PROP_GO(e, FMT(&certfile, "%x/%x", FS(g_test_files), FS("ssl/good-cert.pem")), early_fail);
    PROP_GO(e, FMT(&keyfile, "%x/%x", FS(g_test_files), FS("ssl/good-key.pem")), early_fail);
    PROP_GO(e, FMT(&dhfile, "%x/%x", FS(g_test_files), FS("ssl/dh_4096.pem")), early_fail);
    ssl_context_t ctx;
    PROP_GO(e, ssl_context_new_server(&ctx, certfile.data,
                keyfile.data, dhfile.data), early_fail);

    listener_t listener;
    PROP_GO(e, listener_new_ssl(&listener, &ctx, "127.0.0.1", 1996), early_fail);
early_fail:
    thread_return = e;
    // no matter what, signal the main thread
    pthread_mutex_lock(&server_mutex);
    pthread_cond_signal(&server_cond);
    pthread_mutex_unlock(&server_mutex);
    // exit with error if necessary
    if(thread_return.type) goto cleanup;


    connection_t conn;
    // accept a connection
    e = listener_accept(&listener, &conn);
    listener_close(&listener);
    PROP_GO(e, e, cleanup);

    // create a pop_server to talk over this connection
    pop_server_t ps = {conn, { loginhook, stathook, listhook,
                               retrhook, delehook, rsethook,
                               tophook, uidlhook, quithook} };
    // send server greeting
    DSTR_STATIC(response, "+OK DITM ready.\r\n");
    PROP_GO(e, pop_server_send_dstr(&ps, &response), cu_conn);


    // kick off server loop
    PROP_GO(e, pop_server_loop(&ps, (void*)&ps), cu_conn);

cu_conn:
    connection_close(&conn);

cleanup:
    ssl_context_free(&ctx);
    thread_return = e;
    return NULL;
}

#define EXPECT_RESPONSE(namestr, string){ \
    /* read from the socket */ \
    DSTR_VAR(recv, 4096); \
    PROP_GO(e, connection_read(&conn, &recv, NULL), cleanup_3); \
    /* compare to expected string */ \
    result = dstr_cmp(&string, &recv); \
    if(result != 0) \
        ORIG_GO(e, E_VALUE, "incorrect " namestr " response", cleanup_3); \
    }

#define EXPECT_HOOK_RESPONSE(namestr, hook, string){ \
    EXPECT_RESPONSE(namestr, string) \
    if(called != hook) \
        ORIG_GO(e, E_VALUE, namestr " not called", cleanup_3); \
    /* make sure it was the only hook called */ \
    if(last_called != CLEARED) \
        ORIG_GO(e, E_VALUE, "double hook call detected on" namestr, cleanup_3); \
    /* reset hooks */ \
    called = CLEARED; \
    last_called = CLEARED; \
}

static derr_t test_pop_server(void){
    derr_t e = E_OK;

    pthread_cond_init(&server_cond, NULL);
    pthread_mutex_init(&server_mutex, NULL);
    pthread_t thread;

    // lock so the server doesn't signal before we are waiting
    pthread_mutex_lock(&server_mutex);
    // start the server
    pthread_create(&thread, NULL, server_thread, NULL);
    // now wait the server to be ready
    pthread_cond_wait(&server_cond, &server_mutex);
    // unlock mutex
    pthread_mutex_unlock(&server_mutex);

    // prepare the SSL context
    ssl_context_t ctx;
    PROP_GO(e, ssl_context_new_client(&ctx), cleanup_1);

    // now connect to the server
    connection_t conn;
    PROP_GO(e, connection_new_ssl(&conn, &ctx, "127.0.0.1", 1996), cleanup_2);

    // now begin a list of tests
    int result;
    // server greeting
    {
        DSTR_STATIC(expect, "+OK DITM ready.\r\n");
        EXPECT_RESPONSE("greeting", expect);
    }
    // loginhook
    {
        // USER command
        DSTR_STATIC(username_string, "USER username\r\n");
        PROP_GO(e, connection_write(&conn, &username_string), cleanup_3);
        DSTR_STATIC(expect, "+OK\r\n");
        EXPECT_RESPONSE("user", expect);
        // PASS command
        DSTR_STATIC(password_string, "PASS pass word\r\n");
        PROP_GO(e, connection_write(&conn, &password_string), cleanup_3);
        DSTR_STATIC(expect2, "+OK Logged in.\r\n");
        EXPECT_HOOK_RESPONSE("login", LOGINHOOK, expect2);
    }
    // stathook
    {
        DSTR_STATIC(temp, "STAT\r\n");
        PROP_GO(e, connection_write(&conn, &temp), cleanup_3);
        EXPECT_HOOK_RESPONSE("stat", STATHOOK, stat_resp);
    }
    // listhook
    {
        DSTR_STATIC(temp, "LIST\r\n");
        PROP_GO(e, connection_write(&conn, &temp), cleanup_3);
        EXPECT_HOOK_RESPONSE("list", LISTHOOK, list_resp);
    }
    // listhook with index
    {
        DSTR_STATIC(temp, "LIST 1\r\n");
        PROP_GO(e, connection_write(&conn, &temp), cleanup_3);
        EXPECT_HOOK_RESPONSE("list_1", LISTHOOK, list_resp);
    }
    // retrhook
    {
        DSTR_STATIC(temp, "RETR 1\r\n");
        PROP_GO(e, connection_write(&conn, &temp), cleanup_3);
        EXPECT_HOOK_RESPONSE("retr", RETRHOOK, retr_resp);
    }
    // delehook
    {
        DSTR_STATIC(temp, "DELE 1\r\n");
        PROP_GO(e, connection_write(&conn, &temp), cleanup_3);
        EXPECT_HOOK_RESPONSE("dele", DELEHOOK, dele_resp);
    }
    // rsethook
    {
        DSTR_STATIC(temp, "RSET\r\n");
        PROP_GO(e, connection_write(&conn, &temp), cleanup_3);
        EXPECT_HOOK_RESPONSE("rset", RSETHOOK, rset_resp);
    }
    // tophook
    {
        DSTR_STATIC(temp, "TOP 1 2\r\n");
        PROP_GO(e, connection_write(&conn, &temp), cleanup_3);
        EXPECT_HOOK_RESPONSE("top", TOPHOOK, top_resp);
    }
    // uidlhook with index
    {
        DSTR_STATIC(temp, "UIDL\r\n");
        PROP_GO(e, connection_write(&conn, &temp), cleanup_3);
        EXPECT_HOOK_RESPONSE("udil", UIDLHOOK, uidl_resp);
    }
    {
        DSTR_STATIC(temp, "UIDL 1\r\n");
        PROP_GO(e, connection_write(&conn, &temp), cleanup_3);
        EXPECT_HOOK_RESPONSE("udil_1", UIDLHOOK, uidl_resp);
    }
    // quithook
    {
        DSTR_STATIC(temp, "QUIT\r\n");
        PROP_GO(e, connection_write(&conn, &temp), cleanup_3);
        DSTR_STATIC(expect, "+OK Goodbye, my love.\r\n");
        EXPECT_HOOK_RESPONSE("update", QUITHOOK, expect);
    }

cleanup_3:
    connection_close(&conn);
cleanup_2:
    ssl_context_free(&ctx);
cleanup_1:
    pthread_join(thread, NULL);
    pthread_mutex_destroy(&server_mutex);
    pthread_cond_destroy(&server_cond);
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, &g_test_files, LOG_LVL_DEBUG);

    PROP_GO(e, ssl_library_init(), test_fail);

    PROP_GO(e, test_pop_server(), test_fail);

    printf("PASS\n");
    ssl_library_close();
    return 0;

test_fail:
    DUMP(e);
    DROP(e);
    printf("FAIL\n");
    ssl_library_close();
    return 1;
}
