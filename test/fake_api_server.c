#include <string.h>
#include <stdlib.h>

#include <libdstr/libdstr.h>
#include <libcrypto/libcrypto.h>

#include "test/test_utils.h"
#include "test/fake_api_server.h"
#include "test/bioconn.h"

LIST_HEADERS(int)
LIST_FUNCTIONS(int)

typedef unsigned int uint;
LIST_HEADERS(uint)
LIST_FUNCTIONS(uint)

LIST_HEADERS(exp_hook_t)
LIST_FUNCTIONS(exp_hook_t)

// externally visible global variable
unsigned int fas_api_port = 2002;

// track whether or not our server is in the loop
static bool in_loop = false;
DSTR_STATIC(exit_cmd, "EXIT");
DSTR_STATIC(exit_msg, "\nContent-Length: 4\r\n\r\nEXIT");

// for communicating when the fake_pop_server is ready
static dmutex_t fas_mutex;
static dcond_t  fas_cond;

// easy way to return value from a thread
static derr_t thread_return;

#define Q_DEPTH 8

// mutex-protected expected values
static dmutex_t exp_mutex;
LIST_STATIC_VAR(dstr_t, exp_paths, Q_DEPTH);
LIST_STATIC_VAR(dstr_t, exp_args, Q_DEPTH);
LIST_STATIC_VAR(exp_hook_t, exp_hooks, Q_DEPTH);
LIST_STATIC_VAR(uint, exp_ctrs, Q_DEPTH);

// mutex-protected pre-written response
static dmutex_t resp_mutex;
LIST_STATIC_VAR(int, resp_codes, Q_DEPTH);
LIST_STATIC_VAR(dstr_t, responses, Q_DEPTH);

// the fake api server thread
static dthread_t g_thread;

static derr_t fas_recieve_request(connection_t* conn, dstr_t* recv,
                                  dstr_t* payload){
    derr_t e = E_OK;
    recv->len = 0;
    while(true){
        PROP(&e, connection_read(conn, recv, NULL) );
        // assume you have the entire request already, or just continue:

        // first find the content length
        LIST_PRESET(dstr_t, content_length, DSTR_LIT("\nContent-Length: ") );
        char* pos = dstr_find(recv, &content_length, NULL, NULL);
        if(!pos){
            continue;
        }
        size_t cl_start = (uintptr_t)pos + content_length.data[0].len
                          - (uintptr_t)recv->data;

        // now find the end of the line
        LIST_PRESET(dstr_t, line_end, DSTR_LIT("\r\n"), DSTR_LIT("\n"));
        dstr_t sub = dstr_sub(recv, cl_start, 0);
        pos = dstr_find(&sub, &line_end, NULL, NULL);
        if(!pos){
            continue;
        }
        size_t cl_end = (uintptr_t)pos - (uintptr_t)recv->data;

        // interpret the content length
        sub = dstr_sub(recv, cl_start, cl_end);
        unsigned int cl;
        PROP(&e, dstr_tou(&sub, &cl, 10) );

        // now find the end of the headers
        LIST_PRESET(dstr_t, header_end, DSTR_LIT("\r\n\r\n"), DSTR_LIT("\n\n"));
        size_t which;
        pos = dstr_find(recv, &header_end, &which, NULL);
        if(!pos){
            continue;
        }
        size_t body_start = (uintptr_t)pos + header_end.data[which].len
                            - (uintptr_t)recv->data;

        // set the payload
        *payload = dstr_sub(recv, body_start, 0);

        // if we have the whole payload, we are done
        if(payload->len == cl) break;
    }
    return e;
}

static derr_t fas_respond(connection_t* conn, int code,
                                     const dstr_t* response){
    derr_t e = E_OK;
    DSTR_VAR(headers, 256);
    PROP(&e, FMT(&headers, "HTTP/1.0 %x fake api server gives no reason\r\n"
                        "Content-Length: %x\r\n"
                        "\r\n", FI(code), FU(response->len)) );
    //PFMT("RESPONDING:\n%x%x\n--------\n", FD(headers), FD(*response));
    PROP(&e, connection_write(conn, &headers) );
    PROP(&e, connection_write(conn, response) );
    return e;
}

static void* fas_thread(void* arg){
    (void) arg;
    derr_t e = E_OK;

    // allocate memory for reading into
    dstr_t recv;
    PROP_GO(&e, dstr_new(&recv, 4096), cleanup_1);

    // allocate memory for the response (easier error handling)
    dstr_t resp;
    PROP_GO(&e, dstr_new(&resp, 4096), cleanup_2);

    // prepare ssl context
    DSTR_VAR(certfile, 4096);
    DSTR_VAR(keyfile, 4096);
    PROP_GO(&e,
        FMT(&certfile, "%x/%x", FS(g_test_files), FS("ssl/good-cert.pem")),
    cleanup_3);
    PROP_GO(&e,
        FMT(&keyfile, "%x/%x", FS(g_test_files), FS("ssl/good-key.pem")),
    cleanup_3);
    ssl_context_t ctx;
    PROP_GO(&e,
        ssl_context_new_server(&ctx, certfile.data, keyfile.data),
    cleanup_3);

    listener_t listener;
    PROP_GO(&e, listener_new_ssl(&listener, &ctx, "127.0.0.1", fas_api_port), cleanup_4);
    LOG_INFO("Fake API Server ready for incoming connections\n");

    // signal the main thread
    dmutex_lock(&fas_mutex);
    dcond_signal(&fas_cond);
    dmutex_unlock(&fas_mutex);

    in_loop = true;
    bool should_continue = true;
    while(should_continue){
        connection_t conn;
        // accept a connection
        LOG_INFO("Fake API Server about to accept()\n");
        PROP_GO(&e, listener_accept(&listener, &conn), cleanup_5);

        // read everything off the wire
        dstr_t payload;
        PROP_GO(&e, fas_recieve_request(&conn, &recv, &payload), cleanup_6);

        // check if it is the exit command
        if(dstr_cmp(&exit_cmd, &payload) == 0){
            LOG_INFO("Received exit command\n");
            should_continue = false;
            goto cleanup_6;
        }

        // now decode the payload (destroys the recv buffer)
        DSTR_VAR(body, 4096);
        PROP_GO(&e, b642bin_stream(&payload, &body), cleanup_6);
        if(payload.len > 0){
            ORIG_GO(&e, E_VALUE, "bad base64 decode", cleanup_6);
        }

        // get the expected values for the path and arg
        DSTR_VAR(exp_path, 128);
        DSTR_VAR(exp_arg, 2048);
        exp_hook_t hook;
        unsigned int ctr;
        PROP_GO(&e, fas_expect_get(&exp_path, &exp_arg, &hook, &ctr), cleanup_6);

        //PFMT("expecting %x %x\nreceived:%x\n", FD(exp_path), FD(exp_arg), FD(body));

        // now parse the body
        json_t json;
        JSON_PREP_PREALLOCATED(json, 4096, 32, true);

        PROP_GO(&e, json_parse(body, &json), cleanup_6);

        dstr_t path;
        bool have_arg;
        dstr_t _arg;
        jspec_t *jspec = JOBJ(false,
            JKEY("arg", JOPT(&have_arg, JDREF(&_arg))),
            JKEY("path", JDREF(&path)),
        );

        bool ok;
        DSTR_VAR(errbuf, 1024);
        PROP_GO(&e, jspec_read_ex(jspec, json.root, &ok, &errbuf), cleanup_6);
        if(!ok){
            ORIG_GO(&e, E_VALUE, "%x", cleanup_6, FD(errbuf));
        }

        // check the path
        if(!dstr_eq(path, exp_path)){
            TRACE(&e, "expected path: %x\n"
                      "         but got: %x\n",
                     FD(exp_path), FD(path));
            ORIG_GO(&e, E_VALUE, "wrong path", cleanup_6);
        }

        // check the argument, unless exp_arg is the wildcard
        if(!dstr_eq(exp_arg, DSTR_LIT("*")) != 0){
            if(!have_arg){
                if(exp_arg.data != NULL){
                    TRACE(&e, "expected argument: %x\n"
                              "          but got null\n", FD(exp_arg));
                    ORIG_GO(&e, E_VALUE, "wrong argument", cleanup_6);
                }else if(!dstr_eq(_arg, exp_arg) != 0){
                    TRACE(&e, "expected argument: %x\n"
                              "          but got: %x\n",
                             FD(exp_arg), FD(_arg));
                    ORIG_GO(&e, E_VALUE, "wrong argument", cleanup_6);
                }
            }
        }

        // call hook if necessary
        if(hook){
            PROP_GO(&e, hook(&path, &_arg, ctr), cleanup_6);
        }

        // now get the response
        int code;
        PROP_GO(&e, fas_response_get(&code, &resp), cleanup_6);

        // write the response to the wire
        PROP_GO(&e, fas_respond(&conn, code, &resp), cleanup_6);

cleanup_6:
        connection_close(&conn);
        if(is_error(e)) goto cleanup_5;
    }
    in_loop = false;

cleanup_5:
    listener_close(&listener);
cleanup_4:
    ssl_context_free(&ctx);
cleanup_3:
    dstr_free(&resp);
cleanup_2:
    dstr_free(&recv);
cleanup_1:
    thread_return = e;
    // signal the main thread, in case of early exit
    dmutex_lock(&fas_mutex);
    dcond_signal(&fas_cond);
    dmutex_unlock(&fas_mutex);

    LOG_INFO("fas exiting: %x\n", FD(error_to_dstr(e.type)) );
    return NULL;
}

derr_t fas_start(void){
    derr_t e = E_OK;

    // zero all lists
    resp_codes.len = 0;
    responses.len = 0;
    exp_paths.len = 0;
    exp_args.len = 0;
    exp_hooks.len = 0;
    exp_ctrs.len = 0;

    // prepare for protecting read/write of expected calls and responses
    PROP(&e, dmutex_init(&exp_mutex) );
    PROP_GO(&e, dmutex_init(&resp_mutex), fail_exp_mutex);
    // prepare for the cond_wait
    PROP_GO(&e, dcond_init(&fas_cond), fail_resp_mutex);
    PROP_GO(&e, dmutex_init(&fas_mutex), fail_fas_cond);

    // lock so the server doesn't signal before we are waiting
    dmutex_lock(&fas_mutex);
    // start the server
    PROP_GO(&e, dthread_create(&g_thread, fas_thread, NULL), fail_fas_mutex);
    // now wait the server to be ready
    dcond_wait(&fas_cond, &fas_mutex);
    // unlock mutex
    dmutex_unlock(&fas_mutex);

    return e;

fail_fas_mutex:
    dmutex_unlock(&fas_mutex);
    dmutex_free(&fas_mutex);
fail_fas_cond:
    dcond_free(&fas_cond);
fail_resp_mutex:
    dmutex_free(&resp_mutex);
fail_exp_mutex:
    dmutex_free(&exp_mutex);
    return e;
}

static derr_t fas_send_exit_command(void){
    derr_t e = E_OK;
    // prepare to use SSL
    ssl_context_t ctx;
    PROP(&e, ssl_context_new_client(&ctx) );
    // connect
    connection_t conn;
    PROP_GO(&e, connection_new_ssl(&conn, &ctx, "127.0.0.1", fas_api_port), cu1);
    PROP_GO(&e, connection_write(&conn, &exit_msg), cu2);
    // wait for fas thread to exit
    dthread_join(&g_thread);
cu2:
    connection_close(&conn);
cu1:
    ssl_context_free(&ctx);
    return e;
}

derr_t fas_join(void){
    derr_t e = E_OK;
    // if we are in the loop we will send the exit command
    if(in_loop){
        LOG_INFO("sending exit command to fake api server\n");
        e = fas_send_exit_command();
        if(is_error(e)){
            TRACE(&e, "error shutting down fake_api_server nicely\n");
            DUMP(e);
            DROP_VAR(&e);
            exit(1);
        }
    }
    dthread_join(&g_thread);

    dmutex_free(&fas_mutex);
    dcond_free(&fas_cond);
    dmutex_free(&resp_mutex);
    dmutex_free(&exp_mutex);
    // free expect lists
    for(size_t i = 0; i < exp_args.len; i++){
        dstr_free(&exp_args.data[i]);
    }
    exp_args.len = 0;
    for(size_t i = 0; i < exp_paths.len; i++){
        dstr_free(&exp_paths.data[i]);
    }
    exp_paths.len = 0;
    exp_hooks.len = 0;
    exp_ctrs.len = 0;
    // free response list
    for(size_t i = 0; i < responses.len; i++){
        dstr_free(&responses.data[i]);
    }
    responses.len = 0;
    resp_codes.len = 0;
    MERGE_CMD(&e, thread_return, "fake api server");
    return e;
}

derr_t fas_expect_put(const dstr_t* path, const dstr_t* arg,
                      exp_hook_t hook, unsigned int counter){
    derr_t e = E_OK;
    // lock the response mutex
    dmutex_lock(&exp_mutex);

    // copy the path
    dstr_t c;
    PROP_GO(&e, dstr_new(&c, path->len), fail_1);
    PROP_GO(&e, dstr_copy(path, &c), fail_2);

    // copy the arg
    dstr_t a;
    PROP_GO(&e, dstr_new(&a, arg->len), fail_2);
    PROP_GO(&e, dstr_copy(arg, &a), fail_3);

    // push the path
    PROP_GO(&e, LIST_APPEND(dstr_t, &exp_paths, c), fail_3);

    // push the arg
    PROP_GO(&e, LIST_APPEND(dstr_t, &exp_args, a), fail_4);

    // push the hook
    PROP_GO(&e, LIST_APPEND(exp_hook_t, &exp_hooks, hook), fail_5);

    // push the hook
    PROP_GO(&e, LIST_APPEND(uint, &exp_ctrs, counter), fail_6);


    dmutex_unlock(&exp_mutex);
    return e;

fail_6:
    exp_hooks.len--;
fail_5:
    exp_args.len--;
fail_4:
    exp_paths.len--;
fail_3:
    dstr_free(&a);
fail_2:
    dstr_free(&c);
fail_1:
    dmutex_unlock(&resp_mutex);
    return e;
}

derr_t fas_expect_get(dstr_t* path, dstr_t* arg,
                      exp_hook_t* hook, unsigned int* counter){
    derr_t e = E_OK;
    // lock the response mutex
    dmutex_lock(&exp_mutex);

    if(exp_paths.len == 0){
        ORIG_GO(&e, E_VALUE, "no expect to pop", cleanup);
    }

    // copy into the path
    dstr_t* c = &exp_paths.data[0];
    PROP_GO(&e, dstr_copy(c, path), cleanup);

    // copy into the arg
    dstr_t* a = &exp_args.data[0];
    PROP_GO(&e, dstr_copy(a, arg), cleanup);

    // set the hook
    *hook = exp_hooks.data[0];

    // set the counter
    *counter = exp_ctrs.data[0];

    // shorten expect lists
    dstr_free(c);
    dstr_free(a);
    LIST_DELETE(dstr_t, &exp_paths, 0);
    LIST_DELETE(dstr_t, &exp_args, 0);
    LIST_DELETE(exp_hook_t, &exp_hooks, 0);
    LIST_DELETE(uint, &exp_ctrs, 0);

cleanup:
    dmutex_unlock(&exp_mutex);
    return e;
}

derr_t fas_response_put(int code, const dstr_t* response){
    derr_t e = E_OK;
    // lock the response mutex
    dmutex_lock(&resp_mutex);

    // push the code
    PROP_GO(&e, LIST_APPEND(int, &resp_codes, code), fail_1);

    // allocate for the response
    dstr_t resp;
    PROP_GO(&e, dstr_new(&resp, response->len), fail_2);

    // copy the response
    PROP_GO(&e, dstr_copy(response, &resp), fail_3);

    // push the response
    PROP_GO(&e, LIST_APPEND(dstr_t, &responses, resp), fail_3);

    dmutex_unlock(&resp_mutex);
    return e;

fail_3:
    dstr_free(&resp);
fail_2:
    // remove what was added to resp_codes
    resp_codes.len--;
fail_1:
    dmutex_unlock(&resp_mutex);
    return e;
}

derr_t fas_response_get(int* code, dstr_t* response){
    derr_t e = E_OK;
    // lock the response mutex
    dmutex_lock(&resp_mutex);

    if(responses.len == 0){
        ORIG_GO(&e, E_VALUE, "no response to pop", cleanup);
    }

    // set the code
    *code = resp_codes.data[0];

    // copy the response into the arg
    dstr_t* r = &responses.data[0];
    PROP_GO(&e, dstr_copy(r, response), cleanup);

    // shorten responses list
    dstr_free(r);
    LIST_DELETE(dstr_t, &responses, 0);
    LIST_DELETE(int, &resp_codes, 0);

cleanup:
    dmutex_unlock(&resp_mutex);
    return e;
}

derr_t fas_assert_done(void){
    derr_t e = E_OK;
    if(responses.len || exp_paths.len > 0){
        ORIG(&e, E_VALUE, "fake api server not called as many times as expected");
    }
    return e;
}
