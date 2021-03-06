#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdlib.h>

#include <common.h>
#include <logger.h>
#include <ditm.h>
#include <networking.h>
#include <fileops.h>

#include "fake_pop_server.h"
#include "fake_api_server.h"
#include "test_utils.h"
#include "dummy_key_tool.h"
#include "ditm_thread.h"

#include <win_compat.h>

// path to where the test files can be found
const char* g_test_files;
static char* expect_inj_msg_subj;

DSTR_STATIC(multi_line_end, "\r\n.\r\n");
DSTR_STATIC(line_end, "\r\n");

#define EXPECT_RESPONSE(namestr, exp){ \
    int result; \
    /* read from the socket */ \
    recv->len = 0; \
    size_t count = 0; \
    while(count == 0){ \
        PROP( connection_read(conn, recv, NULL) ); \
        count = dstr_count(recv, &line_end); \
    } \
    /* compare to expected string */ \
    result = dstr_cmp(exp, recv); \
    if(result != 0){ \
        LOG_ERROR("expected: %x\n" \
                  "but got:  %x\n", FD(exp), FD(recv)); \
        ORIG(E_VALUE, "incorrect " namestr " response" ); \
    } \
}

#define EXPECT_MULTI_RESPONSE(namestr, exp_1, exp_body, exact_match){ \
    int result; \
    /* read from the socket */ \
    recv->len = 0; \
    while(true){ \
        PROP( connection_read(conn, recv, NULL) ); \
        /* make sure we haven't gotten an error */ \
        if(dstr_count(recv, &DSTR_LIT("-ERR")) > 0){ \
            /* if we have an error, wait for the end of the line */ \
            if(dstr_count(recv, &line_end) > 0){ \
                LOG_ERROR("during %x:\n" \
                          "got error:  %x", FS(namestr), FD(recv)); \
                ORIG(E_VALUE, "incorrect response" ); \
            } \
        }else{ \
            if(dstr_count(recv, &multi_line_end) > 0){ \
                break; \
            } \
        } \
    } \
    /* pop3 decode */ \
    buffer->len = 0; \
    bool found_end; \
    PROP( pop3_decode(recv, buffer, &found_end) ); \
    /* get substrings for first line and body */ \
    LIST_PRESET(dstr_t, line_end_pattern, DSTR_LIT("\r\n")); \
    char* pos = dstr_find(buffer, &line_end_pattern, NULL, NULL); \
    if(!pos) ORIG(E_VALUE, "first line not found");\
    pos += line_end_pattern.data[0].len; \
    dstr_t first = dstr_sub(buffer, 0, (uintptr_t)pos - (uintptr_t)buffer->data); \
    dstr_t body = dstr_sub(buffer, (uintptr_t)pos - (uintptr_t)buffer->data, 0); \
    /* compare to expected first line */ \
    if(exp_1 != NULL){ \
        result = dstr_cmp(exp_1, &first); \
        if(result != 0){ \
            LOG_ERROR("during %x:\n" \
                      "expected: %x" \
                      "but got:  %x", FS(namestr), FD(exp_1), FD(&first)); \
            ORIG(E_VALUE, "incorrect response" ); \
        } \
    } \
    /* compare to expected body */ \
    if(exact_match){ \
        result = dstr_cmp(&body, exp_body); \
        if(result != 0){ \
            LOG_ERROR("during %x:\n" \
                      "expected: %x" \
                      "but got:  %x", FS(namestr), FD(exp_body), FD(&body)); \
            LOG_ERROR("length %x vs %x\n", FU((exp_body)->len), FU(body.len)); \
            ORIG(E_VALUE, "incorrect response" ); \
        } \
    }else{ \
        size_t has_match = dstr_count(&body, exp_body); \
        if(has_match == 0){ \
            LOG_ERROR("during %x:\n" \
                      "expected match: %x\n" \
                      "but got:  %x", FS(namestr), FD(exp_body), FD(&body)); \
            ORIG(E_VALUE, "incorrect response" ); \
        } \
    } \
}

typedef derr_t(*test_func)(dstr_t*, dstr_t*, connection_t*);

static derr_t do_test(test_func test, ssl_context_t* ctx,
                      bool need_fps, derr_t ditm_expect_exit,
                      derr_t fps_expect_exit){
    derr_t error;
    dstr_t recv;
    PROP( dstr_new(&recv, 4096) );
    dstr_t buffer;
    PROP_GO( dstr_new(&buffer, 4096), cu_recv);
    connection_t conn;
    // tell servers to accept another connection
    ditm_thread_start_test();
    if(need_fps) fps_start_test();

    // run the test
    PROP_GO( connection_new_ssl(&conn, ctx, "127.0.0.1", 1996), cu_buffer);
    PROP_GO( test(&recv, &buffer, &conn), cu_conn);

    // make sure DITM exited how we expected
    error = ditm_thread_end_test();
    CATCH(ditm_expect_exit){
        LOG_ERROR("We expected that error\n");
        error = E_OK;
    }else CATCH(E_ANY){
        LOG_ERROR("Expected ditm error %x but got %x\n",
                  FD(error_to_dstr(ditm_expect_exit)),
                  FD(error_to_dstr(error)));
        RETHROW_GO(E_VALUE, cu_conn);
    }

    // make sure FPS exited without error
    if(need_fps){
        error = fps_end_test();
        CATCH(fps_expect_exit){
            LOG_ERROR("We expected that error\n");
            error = E_OK;
        }else CATCH(E_ANY){
            LOG_ERROR("Expected fps error %x but got %x\n",
                      FD(error_to_dstr(fps_expect_exit)),
                      FD(error_to_dstr(error)));
            RETHROW_GO(E_VALUE, cu_conn);
        }
    }

cu_conn:
    connection_close(&conn);
cu_buffer:
    dstr_free(&buffer);
cu_recv:
    dstr_free(&recv);
    if(error){
        LOG_INFO("---- FAIL ----\n");
    }else{
        LOG_INFO("---- PASS ----\n");
    }
    return error;
}

#define EXPECT_GREETING { \
    DSTR_STATIC(expect, "+OK DITM ready.\r\n"); \
    EXPECT_RESPONSE("greeting", &expect); \
}

#define DO_LOGIN { \
    /* USER command */ \
    DSTR_VAR(temp, 64); \
    PROP( FMT(&temp, "USER %x\r\n", FS(g_username)) ); \
    PROP( connection_write(conn, &temp) ); \
    DSTR_STATIC(expect, "+OK\r\n"); \
    EXPECT_RESPONSE("user", &expect); \
    /* PASS command */ \
    temp.len = 0; \
    PROP( FMT(&temp, "PASS %x\r\n", FS(g_password)) ); \
    PROP( connection_write(conn, &temp) ); \
    DSTR_STATIC(expect2, "+OK Logged in.\r\n"); \
    EXPECT_RESPONSE("login", &expect2); \
}

#define DO_QUIT { \
    DSTR_STATIC(temp, "QUIT\r\n"); \
    PROP( connection_write(conn, &temp) ); \
    DSTR_STATIC(expect, "+OK Goodbye, my love.\r\n"); \
    EXPECT_RESPONSE("quit", &expect); \
}

#define CHECK_INJECTED_MESSAGE { \
    PROP( connection_write(conn, &DSTR_LIT("RETR 1\r\n")) ); \
    dstr_t subj; \
    DSTR_WRAP(subj, expect_inj_msg_subj, strlen(expect_inj_msg_subj), true); \
    EXPECT_MULTI_RESPONSE("RETR 1", NULL, &subj, false); \
}

#define CONNECTION_SHOULD_BE_BROKEN { \
    /* well, first a write so in case of a failed test we don't hang */ \
    DSTR_STATIC(arbitrary_value, "LIST\r\n"); \
    derr_t error = connection_write(conn, &arbitrary_value); \
    if(!error){ \
        error = connection_read(conn, recv, NULL); \
    } \
    CATCH(E_CONN){ \
        LOG_ERROR("Good, we expected that %x\n", FD(error_to_dstr(error))); \
    }else{ \
        LOG_ERROR("Expected E_CONN but got %x\n", FD(error_to_dstr(error))); \
        ORIG(E_VALUE, "Wrong error"); \
    } \
}

static derr_t test_case_a(dstr_t* recv, dstr_t* buffer, connection_t* conn){
    (void)buffer;
    // greeting from server
    {
        DSTR_STATIC(expect, "+OK error connecting to remote server\r\n");
        EXPECT_RESPONSE("greeting", &expect);
    }
    /* TODO: there is a SIGSEV during the BIO_free_all() in connection_close()
       if this section of code is enabled... but I haven't seen this outside
       of tests, so perhaps it is an openssl-specific threading issue?
       I think I would have better stability if I implemented a
       native-sockets-wrapped-in-OpenSSL-BIO-objects solution.  This should
       be investigated further, it might be a bug in OpenSSL. */
    // try to read, to prove connection is closed
    // {
    //     // well, first a write so in case of a failed test we don't hang
    //     DSTR_STATIC(arbitrary_value, "LIST\r\n");
    //     derr_t error = connection_write(conn, &arbitrary_value);
    //     if(!error){
    //         PFMT("write didn't fail, trying again\n");
    //         error = connection_write(conn, &arbitrary_value);
    //     }
    //     if(!error){
    //         PFMT("write didn't fail, trying read\n");
    //         error = connection_read(conn, recv, NULL);
    //     }
    //     CATCH(E_CONN){
    //         LOG_ERROR("Good, we expected that %x\n", FD(error_to_dstr(error)));
    //     }else{
    //         LOG_ERROR("Expected E_CONN but got %x\n", FD(error_to_dstr(error)));
    //         ORIG(E_VALUE, "Wrong error");
    //     }
    // }
    return E_OK;
}

static derr_t test_case_b_c(dstr_t* recv, dstr_t* buffer, connection_t* conn){
    (void)buffer;
    EXPECT_GREETING;
    // bad user
    {
        // USER command
        DSTR_VAR(temp, 64);
        PROP( FMT(&temp, "USER bad_user@splintermail.com\r\n") );
        PROP( connection_write(conn, &temp) );
        DSTR_STATIC(expect, "+OK\r\n");
        EXPECT_RESPONSE("user", &expect);
        // PASS command
        temp.len = 0;
        PROP( FMT(&temp, "PASS %x\r\n", FS(g_password)) );
        PROP( connection_write(conn, &temp) );
        DSTR_STATIC(expect2, "-ERR Bad login credentials.\r\n");
        EXPECT_RESPONSE("login", &expect2);
    }
    // bad password
    {
        // USER command
        DSTR_VAR(temp, 64);
        PROP( FMT(&temp, "USER %x\r\n", FS(g_username)) );
        PROP( connection_write(conn, &temp) );
        DSTR_STATIC(expect, "+OK\r\n");
        EXPECT_RESPONSE("user", &expect);
        // PASS command
        temp.len = 0;
        PROP( FMT(&temp, "PASS bad password\r\n") );
        PROP( connection_write(conn, &temp) );
        DSTR_STATIC(expect2, "-ERR Bad login credentials.\r\n");
        EXPECT_RESPONSE("login", &expect2);
    }
    DO_QUIT;
    return E_OK;
}

static derr_t test_broken_conn_after_loginhook(dstr_t* recv, dstr_t* buffer,
                                               connection_t* conn){
    (void)buffer;
    EXPECT_GREETING;
    // pop server will respond +OK even though loginhook throws an error
    DO_LOGIN;
    CONNECTION_SHOULD_BE_BROKEN;
    return E_OK;
}

static derr_t test_msg_inj(dstr_t* recv, dstr_t* buffer, connection_t* conn){
    (void)buffer;
    EXPECT_GREETING;
    DO_LOGIN;
    CHECK_INJECTED_MESSAGE;
    DO_QUIT;
    return E_OK;
}

static derr_t test_ditm_errors(void){
    derr_t error = E_OK;
    int ret;

    // start the ditm thread
    PROP( ditm_thread_start(fps_pop_port) );

    // also start the fake pop_server
    fake_pop_server_t fps;
    PROP_GO( fake_pop_server_new(&fps, NULL, 0, NULL), cu_ditm);
    // start fake pop_server and wait until its ready
    fake_pop_server_start(&fps);

    // prepare ssl context
    ssl_context_t ctx;
    PROP_GO( ssl_context_new_client(&ctx), cu_fps);

    /*  Simulate the following errors in ditm's loginhook:
         a. can't connect (not loginhook)   respond with +OK but terminate connection
         b. Bad username                    respond with -ERR
         c. Bad password                    respond with -ERR
         d. no access to userdir            throw E_FS
         e. maildir_new fails               repeat error
            # careful error handling starts
         f. minversion not met              inject update message
         g. key_tool_new fails              goto inj_msg
         h. ignore_list_load fails          goto inj_msg
         i. pop_client_uidl fails           goto inj_msg
         j. ditm_download_message fails     goto inj_msg
         k. ignore_list_write fails         goto inj_msg
         l. update_key_tool fails           goto inj_msg
        inj_msg:
            E_CONN | E_NOMEM | E_OS | E_SSL: temporary errors, prop error
            E_PARAM (from key_tool_update): prop error (only from hostname-too-long, not possible)
            E_FS: inject message
            E_RESPONSE: inject message
            E_INTERNAL | E_ANY: inject message
    */

    // case A: can't connect to remote server, respond +OK but break connection
    ditm_thread_pop_port = 61234; // something random
    LOG_INFO("---- TEST CASE A: can't connect to remote server ----\n");
    PROP_GO( do_test(test_case_a, &ctx, false, E_CONN, E_OK), cu_ctx);
    ditm_thread_pop_port = fps_pop_port;

    // case B/C: bad username/password
    LOG_INFO("---- TEST CASES B/C: bad username/password ----\n");
    PROP_GO( do_test(test_case_b_c, &ctx, true, E_OK, E_OK), cu_ctx);

    // case D: no access to user dir
    const char* oldpath = ditm_path;
    ditm_path = "not/a/real/dir";
    LOG_INFO("---- TEST CASE D: no access to userdir ----\n");
    PROP_GO( do_test(test_broken_conn_after_loginhook, &ctx, true, E_FS, E_CONN), cu_ctx);
    ditm_path = oldpath;

    // case E: error in maildir_new
    // first we need to set this up with a maildir that is invalid:
    // start clean
    rm_rf(ditm_path); CATCH(E_ANY){ LOG_ERROR("ignoring\n"); }
    // make ditm_dir
    if(!dir_rw_access(ditm_path, true)){
        ORIG_GO(E_FS, "FS setup failed", cu_ctx);
    }
    // make ditm_dir/userdir
    DSTR_VAR(userdir, 4096);
    PROP_GO( FMT(&userdir, "%x/%x", FS(ditm_path), FS(g_username)), cu_ctx);
    if(!dir_rw_access(userdir.data, true)){
        ORIG_GO(E_FS, "FS setup failed", cu_ctx);
    }
    // make ditm_dir/userdir/cur as a file, not a dir
    DSTR_VAR(temp, 4096);
    PROP_GO( FMT(&temp, "%x/%x/cur", FS(ditm_path), FS(g_username)), cu_ctx);
    ret = open(temp.data, O_WRONLY | O_CREAT, 0644);
    if(ret < 0){
        LOG_ERROR("unable to create %x: %x\n", FD(&temp), FE(&errno));
        ORIG_GO(E_FS, "FS setup failed", cu_ctx);
    }
    close(ret);
    // run the test
    LOG_INFO("---- TEST CASE E: error in maildir_new() ----\n");
    PROP_GO( do_test(test_broken_conn_after_loginhook, &ctx, true, E_FS, E_CONN), cu_ctx);
    // cleanup for next test
    PROP_GO( rm_rf(userdir.data), cu_ctx);

    // case F: minimum version not met
    unsigned int old_ver_maj = fps_ver_maj;
    fps_ver_maj = 777; // a large number
    expect_inj_msg_subj = "Subject: Splintermail Software Update";
    LOG_INFO("---- TEST CASE F: minimum version not met ----\n");
    PROP_GO( do_test(test_msg_inj, &ctx, true, E_OK, E_OK), cu_ctx);
    // cleanup for next test
    fps_ver_maj = old_ver_maj;
    PROP_GO( rm_rf(userdir.data), cu_ctx);

    // case G: key_tool_new fails (E_FS, E_INTERNAL, E_NOMEM)
    LOG_INFO("---- TEST CASE G (x3): key_tool_new fails ----\n");
    key_tool_new_should_return = E_FS;
    expect_inj_msg_subj = "Subject: File System Issue";
    PROP_GO( do_test(test_msg_inj, &ctx, true, E_OK, E_OK), cu_ctx);
    PROP_GO( rm_rf(userdir.data), cu_ctx);
    //
    key_tool_new_should_return = E_INTERNAL;
    expect_inj_msg_subj = "Subject: DITM Internal Error";
    PROP_GO( do_test(test_msg_inj, &ctx, true, E_OK, E_OK), cu_ctx);
    PROP_GO( rm_rf(userdir.data), cu_ctx);
    //
    key_tool_new_should_return = E_NOMEM;
    PROP_GO( do_test(test_broken_conn_after_loginhook, &ctx, true, E_NOMEM, E_CONN), cu_ctx);
    // cleanup
    PROP_GO( rm_rf(userdir.data), cu_ctx);
    key_tool_new_should_return = E_OK;

    // case H: ignore_list_load fails (E_FS, E_INTERNAL, E_NOMEM)
    /* this really only throws an error if the path is too long, so we are
       actually going to just ignore it.   It doesn't throw any errors we
       didn't just test in Case G anyway. */

    // case I: pop_client_uidl fails
    // case J: ditm_download_message_fails
    /* Ok, these failures are hard to simulate.  I am going to skip them. */

    // case K: ignore_list_write fails
    LOG_INFO("---- TEST CASE K: ignore_list_write fails ----\n");
    if(!dir_rw_access(userdir.data, true)){
        ORIG_GO(E_FS, "FS setup failed", cu_ctx);
    }
    // create ignore.json as a dir, not a file
    temp.len = 0;
    PROP_GO( FMT(&temp, "%x/%x/ignore.json", FS(ditm_path), FS(g_username)), cu_ctx);
    ret = mkdir(temp.data, 0755);
    if(ret < 0){
        LOG_ERROR("unable to mkdir %x: %x\n", FD(&temp), FE(&errno));
        ORIG_GO(E_FS, "FS setup failed", cu_ctx);
    }
    expect_inj_msg_subj = "Subject: File System Issue";
    PROP_GO( do_test(test_msg_inj, &ctx, true, E_OK, E_OK), cu_ctx);
    // cleanup
    PROP_GO( rm_rf(userdir.data), cu_ctx);

    // case L: key_tool_update fails
    LOG_INFO("---- TEST CASE L (x8): ignore_list_write fails ----\n");
    //
    key_tool_new_should_return = E_INTERNAL;
    expect_inj_msg_subj = "Subject: DITM Internal Error";
    PROP_GO( do_test(test_msg_inj, &ctx, true, E_OK, E_OK), cu_ctx);
    PROP_GO( rm_rf(userdir.data), cu_ctx);
    //
    key_tool_new_should_return = E_FS;
    expect_inj_msg_subj = "Subject: File System Issue";
    PROP_GO( do_test(test_msg_inj, &ctx, true, E_OK, E_OK), cu_ctx);
    PROP_GO( rm_rf(userdir.data), cu_ctx);
    //
    key_tool_new_should_return = E_RESPONSE;
    expect_inj_msg_subj = "Subject: Invalid Server Response";
    PROP_GO( do_test(test_msg_inj, &ctx, true, E_OK, E_OK), cu_ctx);
    PROP_GO( rm_rf(userdir.data), cu_ctx);
    //
    key_tool_new_should_return = E_CONN;
    PROP_GO( do_test(test_broken_conn_after_loginhook, &ctx, true, E_CONN, E_CONN), cu_ctx);
    PROP_GO( rm_rf(userdir.data), cu_ctx);
    //
    key_tool_new_should_return = E_NOMEM;
    PROP_GO( do_test(test_broken_conn_after_loginhook, &ctx, true, E_NOMEM, E_CONN), cu_ctx);
    PROP_GO( rm_rf(userdir.data), cu_ctx);
    //
    key_tool_new_should_return = E_OS;
    PROP_GO( do_test(test_broken_conn_after_loginhook, &ctx, true, E_OS, E_CONN), cu_ctx);
    PROP_GO( rm_rf(userdir.data), cu_ctx);
    //
    key_tool_new_should_return = E_SSL;
    PROP_GO( do_test(test_broken_conn_after_loginhook, &ctx, true, E_SSL, E_CONN), cu_ctx);
    PROP_GO( rm_rf(userdir.data), cu_ctx);

    printf("---------- exit ------------\n");

cu_ctx:
    ssl_context_free(&ctx);
    derr_t fps_error;
cu_fps:
    fps_done();
    fps_error = fake_pop_server_join();
    if(fps_error & E_ANY){
        LOG_CATCH(fps_error);
        LOG_ERROR("fake pop server exited with error\n");
        if(!error) error = fps_error;
    }
    fake_pop_server_free(&fps);
    derr_t ditm_error;
cu_ditm:
    ditm_thread_done();
    ditm_error = ditm_thread_join();
    if(ditm_error & E_ANY){
        LOG_CATCH(ditm_error);
        LOG_ERROR("ditm thread exited with error %x\n",
                  FD(error_to_dstr(ditm_error)));
        if(!error) error = ditm_error;
    }
    return error;
}

static void sig_handler(int signum){
    if(signum == SIGINT){
        rm_rf(g_username);
        exit(1);
    }
}

int main(int argc, char** argv){
    derr_t error;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, &g_test_files, LOG_LVL_INFO);

    signal(SIGINT, sig_handler);
    // if not on windows, OpenSSL requires us to ignore SIGPIPE
    // its especially important here because we break a lot of connections
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    PROP_GO( ssl_library_init(), test_fail);

    PROP_GO( test_ditm_errors(), test_fail);
    error = rm_rf(ditm_path);
    CATCH(E_ANY){
        error = E_OK;
    }

    LOG_ERROR("PASS\n");
    ssl_library_close();
    return 0;

test_fail:
    LOG_ERROR("FAIL\n");
//    error = rm_rf(ditm_path);
//    CATCH(E_ANY){
//        error = E_OK;
//    }
    ssl_library_close();
    return 1;
}
