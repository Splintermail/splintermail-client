#include <errno.h>

#include <libdstr/libdstr.h>
#include <ui.h>

#include "dummy_ui_harness.h"
#include "test_utils.h"

#define TEST_LOG_LEVEL LOG_LVL_DEBUG

#define SM "splintermail"

// read from our process's dup'd stdout asynchronously:
static dthread_t ar_thread;
static dstr_t ar_buffer;
static derr_t ar_return = {0};

static void* async_reader_thread(void* arg){
    int* fd = (int*)arg;
    derr_t e = E_OK;
    derr_t e2;
    while(true){
        char c;
        // read just one character
        ssize_t amnt_read = compat_read(*fd, &c, 1);
        if(amnt_read == 0){
            break;
        }else if(amnt_read < 0){
            DROP_CMD( FFMT(stderr, NULL, "read failed: %x\n", FE(&errno)) );
            ORIG_GO(&e, E_OS, "read failed during test", done);
        }
        // add that one character to the buffer
        e2 = FMT(&ar_buffer, "%x", FC(c));
        PROP_GO(&e, e2, done);
    }
done:
    ar_return = e;
    return NULL;
}

static derr_t start_async_reader(int* fd){
    derr_t e = E_OK;
    // allocate our buffer
    PROP(&e, dstr_new(&ar_buffer, 8192) );
    // start the thread
    PROP(&e, dthread_create(&ar_thread, async_reader_thread, (void*)fd) );
    return e;
}

static derr_t stop_async_reader(dstr_t** out){
    // join the thread
    dthread_join(&ar_thread);
    // pass buffer
    *out = &ar_buffer;
    return ar_return;
}

// path to where the test files can be found
static const char* g_test_files;

static int real_stdout_fd;

struct test_case_t {
    char* test_name;
    char** argv;
    bool call_citm_loop;
    citm_args_t citm_args;
    char** users;
    bool find_token;
    derr_t read_token_error;
    api_token_t token_to_read;
    bool call_register_token;
    struct register_token_args_t register_token_args;
    bool call_api_password;
    struct api_password_args_t api_password_args;
    bool call_api_token;
    struct api_token_args_t api_token_args;
    char** creatables;
    char** passwords;
    char** strings;
    char* expect_out;
    int expect_return;
};

static derr_t run_test_case(struct test_case_t test){
    derr_t e = E_OK;
    PROP(&e, FFMT(stderr, NULL, "---- running test case %x ----\n",
                 FS(test.test_name)) );
    // first reset several values
    looked_good = true;
    reason_log.len = 0;
    citm_called = false;
    register_token_called = false;
    api_token_called = false;
    api_password_called = false;
    // copy some values
    citm_args = &test.citm_args;
    register_token_args = &test.register_token_args;
    api_password_args = &test.api_password_args;
    api_token_args = &test.api_token_args;
    find_token = test.find_token;
    read_token_error = test.read_token_error;
    token_to_read = &test.token_to_read;
    users = test.users;
    creatables = test.creatables;
    passwords = test.passwords;
    strings = test.strings;
    // replace stdout with a pipe
    int fds[2];
    int ret = compat_pipe(fds);
    if(ret != 0){
        perror("pipe");
        UH_OH("run_test_case failed to open pipe\n");
    }
    fflush(stdout);
    compat_close(1);
    ret = compat_dup(fds[1]);
    if(ret < 0){
        perror("pipe");
        UH_OH("run_test_case failed to dupe pipe\n");
    }
    compat_close(fds[1]);
    // prepare local copy of test.argv
    // because otherwise argv might get modified, resulting in segfault
    int argc = 0;
    DSTR_VAR(argv_buffer, 4096);
    char **temp = test.argv;
    char* local_argv[256];
    while(*temp && (size_t)argc < sizeof(local_argv)/sizeof(*local_argv)){
        // next element of argv will start where argv_buffer.data ends now
        local_argv[argc] = argv_buffer.data + argv_buffer.len;
        PROP(&e, FMT(&argv_buffer, "%x", FS(*temp)) );
        // include the null termination
        PROP(&e, dstr_grow(&argv_buffer, argv_buffer.len + 1) );
        argv_buffer.len += 1;
        temp++;
        argc++;
    }
    PROP(&e, FFMT(stderr, NULL, "running do_main\n") );
    // start the async_reader
    PROP(&e, start_async_reader(&fds[0]) );
    // run the test
    int main_ret = do_main(argc, local_argv, false);
    // capture stdout (and stop_async_reader)
    fflush(stdout);
    compat_close(1);
    dstr_t* out_buffer;
    IF_PROP(&e, stop_async_reader(&out_buffer) ){
        UH_OH("error in stop_async_reader\n");
        out_buffer->len = 0;
        DROP_VAR(&e);
    }
    // restore the real stdout
    compat_close(fds[0]);
    ret = compat_dup(real_stdout_fd);
    if(ret < 0){
        perror("pipe");
        UH_OH("run_test_case failed to restore stdin\n");
    }
    PROP_GO(&e, dstr_null_terminate(out_buffer), cleanup);
    // fix the logging
    logger_clear_outputs();
    logger_add_fileptr(TEST_LOG_LEVEL, stderr);
    // compare the results
    if(test.call_citm_loop != citm_called)
        UH_OH("run_test_case citm_called exp %x but got %x\n",
              FI(test.call_citm_loop), FI(citm_called));
    if(test.call_register_token != register_token_called)
        UH_OH("run_test_case register_token_called exp %x but got %x\n",
              FI(test.call_register_token), FI(register_token_called));
    if(test.call_api_password != api_password_called)
        UH_OH("run_test_case api_password_called exp %x but got %x\n",
              FI(test.call_api_password), FI(api_password_called));
    if(test.call_api_token != api_token_called)
        UH_OH("run_test_case api_token_called exp %x but got %x\n",
              FI(test.call_api_token), FI(api_token_called));
    if(test.expect_return != main_ret)
        UH_OH("run_test_case do_main return exp %x but got %x\n",
              FI(test.expect_return), FI(main_ret));
    if(test.expect_out && strcmp(test.expect_out, out_buffer->data))
        UH_OH("run_test_case stdout exp:\n%x but got:\n%x\n",
              FS(test.expect_out), FD(out_buffer));
    // now, finally check if we passed the tests or not
    if(looked_good == false){
        LOG_ERROR("---- FAIL ----\n");
        TRACE(&e, "test %x failed:\n%x", FS(test.test_name), FD(&reason_log));
        ORIG_GO(&e, E_VALUE, "test failed", cleanup);
    }else{
        LOG_ERROR("---- PASS ----\n");
    }
cleanup:
    dstr_free(out_buffer);
    return e;
}


static derr_t run_all_cases(void){
    derr_t e = E_OK;

    // non-citm, non-api call tests
    {
        struct test_case_t test_case = {0};

        test_case.test_name = "help";
        test_case.argv = (char*[]){SM, "--help", NULL};
        PROP(&e, run_test_case(test_case) );
        test_case.argv = (char*[]){SM, "-h", NULL};
        PROP(&e, run_test_case(test_case) );

        test_case.test_name = "dump-conf";
        test_case.expect_out = "";
        test_case.argv = (char*[]){SM, "--dump-conf", NULL};
        PROP(&e, run_test_case(test_case) );
        test_case.expect_out = NULL;

        test_case.test_name = "version";
        test_case.expect_out = "0.4.0\n";
        test_case.argv = (char*[]){SM, "--version", NULL};
        PROP(&e, run_test_case(test_case) );
        test_case.argv = (char*[]){SM, "-v", NULL};
        PROP(&e, run_test_case(test_case) );
        test_case.expect_out = NULL;

        // prep a config file
        DSTR_VAR(configpath, 4096);
        PROP(&e, FMT(&configpath, "%x/test_ui/testconf", FS(g_test_files)) );
        // prepare the expected stdout
        test_case.test_name = "config";
        test_case.expect_out = "splintermail-dir 12345\n";
        test_case.argv = (char*[]){SM, "--config", configpath.data, "--dump-conf", NULL};
        PROP(&e, run_test_case(test_case) );
        test_case.argv = (char*[]){SM, "-c", configpath.data, "--dump-conf", NULL};
        PROP(&e, run_test_case(test_case) );
        test_case.expect_out = NULL;

        test_case.test_name = "reject bad options";
        test_case.expect_return = 1;
        test_case.argv = (char*[]){SM, "citm", "--avseij", NULL};
        PROP(&e, run_test_case(test_case) );
        test_case.argv = (char*[]){SM, "citm", "-z", NULL};
        PROP(&e, run_test_case(test_case) );
        test_case.argv = (char*[]){SM, "citm", "--listen-port", NULL};
        PROP(&e, run_test_case(test_case) );

        test_case.test_name = "reject non-existent config file";
        test_case.expect_return = 2;
        test_case.argv = (char*[]){SM, "-c", "fake_file", NULL};
        PROP(&e, run_test_case(test_case) );

        test_case.test_name = "missing positional args";
        test_case.expect_return = 3;
        test_case.argv = (char*[]){SM, NULL};
        PROP(&e, run_test_case(test_case) );

        test_case.test_name = "need --user flag";
        test_case.expect_return = 5;
        test_case.users = (char*[]){NULL};
        test_case.argv = (char*[]){SM, "list_aliases", NULL};
        PROP(&e, run_test_case(test_case) );
        test_case.users = (char*[]){"a@fqdn", "b@fqdn", NULL};
        test_case.argv = (char*[]){SM, "list_aliases", NULL};
        PROP(&e, run_test_case(test_case) );
        test_case.users = NULL;

        test_case.test_name = "password confirmation fails";
        test_case.expect_return = 6;
        test_case.passwords = (char*[]){"a", "b", "c", NULL};
        test_case.argv = (char*[]){SM, "-u", "a", "change_password", NULL};
        PROP(&e, run_test_case(test_case) );
        test_case.passwords = NULL;

    }

    // test CITM args
    {
        struct test_case_t test_case = {
            .call_citm_loop = true,
            .citm_args = {
                .local_host = "127.0.0.1",
                .local_svc = "1993",
                .key = "splintermail/citm-127.0.0.1-key.pem",
                .cert = "splintermail/citm-127.0.0.1-cert.pem",
                .remote_host = "splintermail.com",
                .remote_svc = "993",
                .maildir_root = "splintermail/citm",
                .indicate_ready = false,
                .to_return = E_OK,
            },
        };

        test_case.test_name = "citm basic test";
        test_case.argv = (char*[]){SM, "citm", NULL};
        PROP(&e, run_test_case(test_case) );

        test_case.test_name = "citm uncaught error";
        test_case.expect_return = 127;
        test_case.citm_args.to_return = (derr_t){.type=E_INTERNAL};
        test_case.argv = (char*[]){SM, "citm", NULL};
        PROP(&e, run_test_case(test_case) );
        test_case.expect_return = 0;
        test_case.citm_args.to_return = E_OK;

        test_case.test_name = "citm listen-port";
        test_case.citm_args.local_svc = "1234";
        test_case.argv = (char*[]){SM, "citm", "--listen-port", "1234", NULL};
        PROP(&e, run_test_case(test_case) );
        test_case.citm_args.local_svc = "1993";

        test_case.test_name = "citm splintermail-dir";
        test_case.citm_args.key = "some_dir/citm-127.0.0.1-key.pem";
        test_case.citm_args.cert = "some_dir/citm-127.0.0.1-cert.pem";
        test_case.citm_args.maildir_root = "some_dir/citm";
        test_case.argv = (char*[]){
            SM, "citm", "--splintermail-dir", "some_dir", NULL
        };
        PROP(&e, run_test_case(test_case) );
        test_case.citm_args.maildir_root = "";
        test_case.citm_args.key = "splintermail/citm-127.0.0.1-key.pem";
        test_case.citm_args.cert = "splintermail/citm-127.0.0.1-cert.pem";

        test_case.test_name = "citm cert";
        test_case.citm_args.cert = "some_file";
        test_case.argv = (char*[]){SM, "citm", "--cert", "some_file", NULL};
        PROP(&e, run_test_case(test_case) );
        test_case.citm_args.cert = "splintermail/citm-127.0.0.1-cert.pem";

        test_case.test_name = "citm key";
        test_case.citm_args.key = "some_file";
        test_case.argv = (char*[]){SM, "citm", "--key", "some_file", NULL};
        PROP(&e, run_test_case(test_case) );
        test_case.citm_args.key = "splintermail/citm-127.0.0.1-key.pem";
    }

/*
    API Cred Search INPUT:          REG?    HAVE_TOKEN?     CASE
        account_dir no access       no      no              A
        account_dir access          -
            creds found             -
                with access
                    good creds      no      yes             B
                    err creds       no      no              C
                no access           no      no              D (same as C)
            creds not found         yes     no              E

    * there is no good way with the fake filesystem setup I have to test
        if revoked api tokens are deleted

    check/prompt Register INPUT     REG?    PROMPT?         CASE (following E)
        noreg found                 no      no              W
        noreg not found             -       -
            user says y             yes     yes             X
            user says n             no      yes             Y
            user says e             no      yes             Z
            user says anything else -       -       # we should have a test for this

    * there is no good way with the fake filesystem setup I have to test
        if a noregister file was created

*/

    // test API COMMAND args, api_token_call()
    // These all fall under Case "B"
    {
        // define our token
        struct test_case_t test_case = {
            .call_api_token = true,
            .find_token = true,
            .token_to_read = {.key = 12345,
                              .secret = DSTR_LIT("ABCDEF"),
                              .nonce = 0},
            .api_token_args = (struct api_token_args_t){
                .host = "splintermail.com",
                .port = 443,
                .token = {.key = 12345,
                          .secret = DSTR_LIT("ABCDEF"),
                          .nonce = 1},
                .code = 200,
                .reason = "OK",
                .json = "{\"status\":\"success\", \"contents\":\"ok\"}",
                .to_return = E_OK,
            },
            .users = (char*[]){"user@fqdn", NULL},
            .expect_out = "\"ok\"\n",
        };

        test_case.test_name = "basic token test";
        test_case.argv = (char*[]){SM, "list_aliases", NULL};
        PROP(&e, run_test_case(test_case) );

        test_case.test_name = "delete useless token test";
        test_case.argv = (char*[]){SM, "list_aliases", NULL};
        test_case.api_token_args.code = 403;
        test_case.expect_return = 9;
        test_case.expect_out = NULL;
        PROP(&e, run_test_case(test_case) );

        test_case.test_name = "misc failure test";
        test_case.argv = (char*[]){SM, "list_aliases", NULL};
        test_case.api_token_args.code = 500;
        test_case.expect_return = 10;
        test_case.expect_out = NULL;
        PROP(&e, run_test_case(test_case) );
    }

    // test API COMMAND args, api_password_call(), no register
    {
        // define our token
        struct test_case_t test_case = {
            .call_api_password = true,
            .token_to_read = {.key = 12345,
                              .secret = DSTR_LIT("ABCDEF"),
                              .nonce = 0},
            .api_password_args = (struct api_password_args_t){
                .host = "splintermail.com",
                .port = 443,
                .user = "user@fqdn",
                .pass = "pass",
                .code = 200,
                .reason = "OK",
                .json = "{\"status\":\"success\", \"contents\":\"ok\"}",
                .to_return = E_OK,
            },
            .expect_out = "\"ok\"\n",
        };

        // case "A"
        test_case.test_name = "bad permissions access_dir";
        test_case.users = NULL;
        test_case.passwords = (char*[]){"pass", NULL};
        test_case.argv = (char*[]){SM, "-a", "fake_file", "--user", "user@fqdn", "list_aliases", NULL};
        PROP(&e, run_test_case(test_case) );

        // case "A", different variety
        test_case.test_name = "bad permissions user_dir";
        test_case.users = (char*[]){"no.perms.user@fqdn", NULL};
        test_case.api_password_args.user = "no.perms.user@fqdn";
        test_case.passwords = (char*[]){"pass", NULL};
        test_case.argv = (char*[]){SM, "list_aliases", NULL};
        PROP(&e, run_test_case(test_case) );

        // case "C" -> noreg ("W")
        test_case.test_name = "bad creds";
        test_case.users = (char*[]){"user@fqdn", NULL};
        test_case.api_password_args.user = "user@fqdn";
        test_case.find_token = true;
        test_case.read_token_error = (derr_t){E_PARAM};
        test_case.passwords = (char*[]){"pass", NULL};
        test_case.argv = (char*[]){SM, "list_aliases", NULL};
        PROP(&e, run_test_case(test_case) );
        test_case.find_token = false;
        test_case.read_token_error = E_OK;

        // case "D"
        test_case.test_name = "no access to creds";
        test_case.users = (char*[]){"no.creds.access.user@fqdn", NULL};
        test_case.api_password_args.user = "no.creds.access.user@fqdn";
        test_case.passwords = (char*[]){"pass", NULL};
        test_case.argv = (char*[]){SM, "list_aliases", NULL};
        PROP(&e, run_test_case(test_case) );

        // case "E" -> noreg ("W")
        test_case.test_name = "no creds yet, noreg found";
        test_case.creatables = (char*[]){"noreg.user@fqdn/api_token.json", NULL};
        test_case.users = (char*[]){"noreg.user@fqdn", NULL};
        test_case.api_password_args.user = "noreg.user@fqdn";
        test_case.passwords = (char*[]){"pass", NULL};
        test_case.argv = (char*[]){SM, "list_aliases", NULL};
        PROP(&e, run_test_case(test_case) );
        test_case.creatables = NULL;

        // case "X" (via "E", "Z")
        test_case.test_name = "i-dont-wanna register";
        test_case.creatables = (char*[]){"regme.user@fqdn/api_token.json",
                                         "regme.user@fqdn/noregister",
                                         NULL};
        test_case.users = (char*[]){"regme.user@fqdn", NULL};
        test_case.api_password_args.user = "regme.user@fqdn";
        test_case.passwords = (char*[]){"pass", NULL};
        test_case.strings = (char*[]){"q", "r", "s", "n", NULL};
        test_case.argv = (char*[]){SM, "list_aliases", NULL};
        PROP(&e, run_test_case(test_case) );
        test_case.creatables = NULL;

    }

    // test API COMMAND args, api_password_call(), with register
    {
        // case "X" (via "E")
        struct test_case_t test_case = {
            .test_name = "no creds yet",
            .users = (char*[]){"regme.user@fqdn", NULL},
            .passwords = (char*[]){"pass", NULL},
            .creatables = (char*[]){"regme.user@fqdn/api_token.json",
                                    "regme.user@fqdn/noregister",
                                    NULL},
            .strings = (char*[]){"y", NULL},
            .call_api_password = true,
            .api_password_args = (struct api_password_args_t){
                .host = "splintermail.com",
                .port = 443,
                .user = "regme.user@fqdn",
                .pass = "pass",
                .code = 200,
                .reason = "OK",
                .json = "{\"status\":\"success\", \"contents\":\"ok\"}",
                .to_return = E_OK,
            },
            .call_register_token = true,
            .register_token_args = (struct register_token_args_t){
                .host = "splintermail.com",
                .port = 443,
                .user = NULL,
                .pass = NULL,
                .creds_path = NULL,
                .to_return = E_OK,
            },
            .argv = (char*[]){SM, "list_aliases", NULL},
            .expect_out = "\"ok\"\n",
        };
        PROP(&e, run_test_case(test_case) );
    }

    // Situations where we even if we have a token (Case "B") we need_password
    {
        // define our token
        struct test_case_t test_case = {
            .find_token = true,
            .token_to_read = {.key = 12345,
                              .secret = DSTR_LIT("ABCDEF"),
                              .nonce = 0},
            .call_api_token = false,
            .call_api_password = true,
            // don't care about the args, those are tested elsewhere
            .api_password_args = (struct api_password_args_t){
                .host = "splintermail.com",
                .port = 443,
                .user = "user@fqdn",
                .pass = "pass",
                .code = 200,
                .reason = "OK",
                .json = "{\"status\":\"success\", \"contents\":\"ok\"}",
                .to_return = E_OK,
            },
            .users = (char*[]){"user@fqdn", NULL},
            .expect_out = "\"ok\"\n",
        };

        test_case.test_name = "add device test";
        test_case.passwords = (char*[]){"pass", NULL};
        test_case.argv = (char*[]){SM, "add_device", NULL};
        PROP(&e, run_test_case(test_case) );

        test_case.test_name = "add token test";
        test_case.passwords = (char*[]){"pass", NULL};
        test_case.argv = (char*[]){SM, "add_token", NULL};
        PROP(&e, run_test_case(test_case) );

        test_case.test_name = "delete all mail test";
        test_case.passwords = (char*[]){"pass", NULL};
        test_case.strings = (char*[]){"I really want to do this", NULL};
        test_case.argv = (char*[]){SM, "delete_all_mail", NULL};
        PROP(&e, run_test_case(test_case) );

        test_case.test_name = "delete all aliases test";
        test_case.passwords = (char*[]){"pass", NULL};
        test_case.strings = (char*[]){"I really want to do this", NULL};
        test_case.argv = (char*[]){SM, "delete_all_aliases", NULL};
        PROP(&e, run_test_case(test_case) );

        test_case.test_name = "delete account test";
        test_case.passwords = (char*[]){"pass", NULL};
        test_case.strings = (char*[]){"I really want to do this", NULL};
        test_case.argv = (char*[]){SM, "delete_account", NULL};
        PROP(&e, run_test_case(test_case) );

        test_case.test_name = "failed confirmation test";
        test_case.passwords = (char*[]){"pass", NULL};
        test_case.strings = (char*[]){"i really want to do this", NULL};
        test_case.argv = (char*[]){SM, "delete_account", NULL};
        test_case.call_api_password = false;
        test_case.expect_out = NULL;
        test_case.expect_return = 8;
        PROP(&e, run_test_case(test_case) );
    }

    return e;
}

static derr_t test_trim_logfile(void){
    derr_t e = E_OK;

    DSTR_VAR(tmp, 256);
    PROP(&e, mkdir_temp("test-ui", &tmp) );

    DSTR_VAR(log, 256);
    PROP_GO(&e, FMT(&log, "%x/logfile", FD(&tmp)), cu);
    char *path = log.data;
    int result;

    // safe when logfile doesn't exist
    PROP_GO(&e, trim_logfile(path, 20), cu);

    // safe when logfile is empty
    PROP_GO(&e, touch(path), cu);
    PROP_GO(&e, trim_logfile(path, 20), cu);
    PROP_GO(&e, file_cmp_dstr(path, &DSTR_LIT(""), false, &result), cu);
    EXPECT_I_GO(&e, "result", result, 0, cu);

    // safe when logfile is small
    DSTR_STATIC(f1,
        "000\n"
        "111\n"
        "222\n"
        "333\n"
        "444\n"
        "555\n"
        "666\n"
        "777\n"
        "888\n"
        "999\n"
    );
    PROP_GO(&e, dstr_write_file(path, &f1), cu);
    PROP_GO(&e, trim_logfile(path, 200), cu);
    PROP_GO(&e, file_cmp_dstr(path, &f1, false, &result), cu);
    EXPECT_I_GO(&e, "result", result, 0, cu);

    // safe when logfile is exact length
    PROP_GO(&e, trim_logfile(path, (long)f1.len), cu);
    PROP_GO(&e, file_cmp_dstr(path, &f1, false, &result), cu);
    EXPECT_I_GO(&e, "result", result, 0, cu);

#ifndef _WIN32
// UNIX, with \n endings
    DSTR_STATIC(f2,
        "888\n"
        "999\n"
    );
    for(size_t i = 8; i < 12; i++){
        PROP_GO(&e, dstr_write_file(path, &f1), cu);
        PROP_GO(&e, trim_logfile(path, 10), cu);
        PROP_GO(&e, file_cmp_dstr(path, &f2, false, &result), cu);
        EXPECT_I_GO(&e, "result", result, 0, cu);
    }

#else
// windows, with \r\n endings

    DSTR_STATIC(f2,
        "888\r\n"
        "999\r\n"
    );
    for(size_t i = 10; i < 15; i++){
        PROP_GO(&e, dstr_write_file(path, &f1), cu);
        PROP_GO(&e, trim_logfile(path, 10), cu);
        PROP_GO(&e, file_cmp_dstr(path, &f2, false, &result), cu);
        EXPECT_I_GO(&e, "result", result, 0, cu);
    }

#endif

cu:
    DROP_CMD( rm_rf(tmp.data) );
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    PARSE_TEST_OPTIONS(argc, argv, &g_test_files, LOG_LVL_INFO);

    logger_add_fileptr(TEST_LOG_LEVEL, stderr);
    PROP_GO(&e, dstr_new(&reason_log, 4096), fail_0);
    // first save the stdout file descriptor
    real_stdout_fd = compat_dup(1);
    if(real_stdout_fd < 0){
        perror("dup");
        goto fail;
    }

    PROP_GO(&e, run_all_cases(), fail);
    PROP_GO(&e, test_trim_logfile(), fail);

    LOG_ERROR("PASS\n");
    dstr_free(&reason_log);
    return 0;

fail:
    dstr_free(&reason_log);
fail_0:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
