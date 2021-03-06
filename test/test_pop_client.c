#include <string.h>
#include <time.h>

#include <common.h>
#include <logger.h>
#include <pop_client.h>
#include <pop_server.h>

#include "fake_pop_server.h"
#include "test_utils.h"

#include <win_compat.h>

// path to where the test files can be found
const char* g_test_files;

// cleaner error handing by creating this helper function
static derr_t do_tests(fake_pop_server_t* fps, pop_client_t* pc, dstr_t* buffer){
    // now connect to the server
    bool ok;
    DSTR_VAR(msg, 1024);
    int result;

    // USER
    dstr_t username;
    username.data = g_username;
    username.len = strlen(g_username);
    username.size = username.len;
    username.fixed_size = true;
    PROP( pop_client_username(pc, &username, &ok, &msg) );
    if(!ok){
        ORIG(E_VALUE, "pop_client_username returned -ERR");
    }

    // PASS
    dstr_t password;
    password.data = g_password;
    password.len = strlen(g_password);
    password.size = password.len;
    password.fixed_size = true;
    PROP( pop_client_password(pc, &password, &ok, &msg) );
    if(!ok){
        ORIG(E_VALUE, "pop_client_password returned -ERR");
    }

    // UIDL
    PROP( pop_client_uidl(pc, &ok, &msg) );
    if(!ok){
        ORIG(E_VALUE, "pop_client_uidl returned -ERR");
    }
    for(size_t i = 0; i < pc->uids.len; i++){
        result = dstr_cmp(&pc->uids.data[i], &fps->uids.data[i]);
        if(result != 0){
            ORIG(E_VALUE, "pop_client_uidl mismatch");
        }
    }

    // RETR
    for(unsigned int i = 0; i < pc->uids.len; i++){
        PROP( pop_client_retrieve(pc, i + 1, &ok, &msg) );
        if(!ok){
            ORIG(E_VALUE, "pop_client_retrieve returned -ERR");
        }
        buffer->len = 0;
        bool found_end;
        PROP( pop_client_get_body(pc, buffer, true,  &found_end) );
        while(found_end == false){
            PROP( pop_client_get_body(pc, buffer, false, &found_end) );
        }
        result = dstr_cmp(buffer, &fps->messages.data[i]);
        if(result != 0){
            ORIG(E_VALUE, "pop_client_retr mismatch");
        }
    }

    // DELE
    PROP( pop_client_delete(pc, 1, &ok, &msg) );
    if(!ok){
        ORIG(E_VALUE, "pop_client_delete returned -ERR");
    }

    // UIDL (retrying after DELE)
    PROP( pop_client_uidl(pc, &ok, &msg) );
    if(!ok){
        ORIG(E_VALUE, "pop_client_uidl after DELE returned -ERR");
    }
    for(size_t i = 0; i < pc->uids.len; i++){
        // note that we shouldn't compare against fps->uids.data[0]
        result = dstr_cmp(&pc->uids.data[i], &fps->uids.data[i+1]);
        if(result != 0){
            ORIG(E_VALUE, "pop_client_uidl after DELE mismatch");
        }
    }

    // RSET
    PROP( pop_client_reset(pc, &ok, &msg) );
    if(!ok){
        ORIG(E_VALUE, "pop_client_reset returned -ERR");
    }

    // UIDL (retrying again after RSET)
    PROP( pop_client_uidl(pc, &ok, &msg) );
    if(!ok){
        ORIG(E_VALUE, "pop_client_uidl after RSET returned -ERR");
    }
    for(size_t i = 0; i < pc->uids.len; i++){
        result = dstr_cmp(&pc->uids.data[i], &fps->uids.data[i]);
        if(result != 0){
            ORIG(E_VALUE, "pop_client_uidl after RSET mismatch");
        }
    }

    // DELE (deleting everything to check parsing of empty pop response body)
    PROP( pop_client_delete(pc, 1, &ok, &msg) );
    if(!ok){
        ORIG(E_VALUE, "pop_client_delete (2.1) returned -ERR");
    }
    PROP( pop_client_delete(pc, 2, &ok, &msg) );
    if(!ok){
        ORIG(E_VALUE, "pop_client_delete (2.2) returned -ERR");
    }
    PROP( pop_client_delete(pc, 3, &ok, &msg) );
    if(!ok){
        ORIG(E_VALUE, "pop_client_delete (2.3) returned -ERR");
    }
    PROP( pop_client_delete(pc, 4, &ok, &msg) );
    if(!ok){
        ORIG(E_VALUE, "pop_client_delete (2.4) returned -ERR");
    }

    // UIDL (with empty list)
    PROP( pop_client_uidl(pc, &ok, &msg) );
    if(!ok){
        ORIG(E_VALUE, "pop_client_uidl after DELE all returned -ERR");
    }
    if(pc->uids.len != 0){
        ORIG(E_VALUE, "pop_client_uidl after DELE all found uids");
    }

    PROP( pop_client_quit(pc, &ok, &msg) );
    if(!ok){
        ORIG(E_VALUE, "pop_client_quit returned -ERR");
    }

    return E_OK;
}

static derr_t test_pop_client(void){
    derr_t error;

    // preapre ssl context
    ssl_context_t ctx;
    PROP( ssl_context_new_client(&ctx) );

    // miscelaneous use buffer
    dstr_t buffer;
    PROP_GO( dstr_new(&buffer, 4096), cleanup_1 );

    fake_pop_server_t fps;
    const char* files[] = {"fps/mail_basic",
                           "fps/mail_long",
                           "fps/mail_dots",
                           "fps/mail_basic.enc"};
    const char* uids[] = {"basic",
                          "long",
                          "dots",
                          "basic_enc"};
    size_t nfiles = sizeof(files) / sizeof(*files);
    PROP_GO( fake_pop_server_new(&fps, files, nfiles, uids), cleanup_2);

    // start fake_pop_server
    PROP_GO( fake_pop_server_start(&fps), cleanup_3);
    fps_start_test();

    pop_client_t pc;
    PROP_GO( pop_client_new(&pc), cleanup_3);

    bool ok;
    DSTR_VAR(msg, 1024);
    PROP_GO( pop_client_connect(&pc, &ctx, "127.0.0.1", fps_pop_port,
                                &ok, &msg), cleanup_4);

    // run the first set of tests
    PROP_GO( do_tests(&fps, &pc, &buffer), cleanup_5);

    PROP_GO( fps_end_test(), cleanup_5);
    fps_done();

cleanup_5:
    pop_client_free(&pc);
    derr_t fps_error;
cleanup_4:
    fps_error = fake_pop_server_join();
    if(fps_error & E_ANY){
        LOG_CATCH(fps_error);
        LOG_ERROR("fake pop server exited with error\n");
    }
cleanup_3:
    fake_pop_server_free(&fps);
cleanup_2:
    dstr_free(&buffer);
cleanup_1:
    ssl_context_free(&ctx);
    return error;
}

static derr_t test_encode_decode(void){
    // encode test
    {
        DSTR_VAR(buffer, 4096);
        DSTR_STATIC(raw, "hey\r\n"
                         "\r\n"
                         ".\r\n"
                         ".\r\n"
                         ".\r\n"
                         "\r\n"
                         "this is a test.\r\n");
        DSTR_STATIC(enc, "hey\r\n"
                         "\r\n"
                         "..\r\n"
                         "..\r\n"
                         "..\r\n"
                         "\r\n"
                         "this is a test.\r\n");
        PROP( pop3_encode(&raw, &buffer, true) );
        int result;
        result = dstr_cmp(&enc, &buffer);
        if(result != 0){
            ORIG(E_VALUE, "pop3_encode test failed");
        }
    }
    // decode test
    {
        DSTR_VAR(buffer, 4096);
        DSTR_STATIC(raw, "hey\r\n"
                         "..\r\n"
                         "..\r\n"
                         "..\r\n"
                         "\r\n"
                         "this is a test.\r\n"
                         ".\r\n");
        DSTR_STATIC(dec, "hey\r\n"
                         ".\r\n"
                         ".\r\n"
                         ".\r\n"
                         "\r\n"
                         "this is a test.\r\n");
        bool found_end;
        PROP( pop3_decode(&raw, &buffer, &found_end) );
        if(found_end != true){
            ORIG(E_VALUE, "pop3_decode didn't find end");
        }
        int result = 0;
        result = dstr_cmp(&dec, &buffer);
        if(result != 0){
            ORIG(E_VALUE, "pop3_encode test failed");
        }
    }
    return E_OK;
}

int main(int argc, char** argv){
    derr_t error;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, &g_test_files, LOG_LVL_WARN);

    // setup the library (application-wide step)
    PROP_GO( ssl_library_init(), test_fail);

    PROP_GO( test_pop_client(), test_fail);

    PROP_GO( test_encode_decode(), test_fail);

    LOG_ERROR("PASS\n");
    ssl_library_close();
    return 0;

test_fail:
    LOG_ERROR("FAIL\n");
    ssl_library_close();
    return 1;
}
