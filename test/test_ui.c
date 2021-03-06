#define HAVE_STRUCT_TIMESPEC
#include <pthread.h>

#include "dummy_ui_harness.h"
#include "test_utils.h"

#include <logger.h>
#include <ui.h>
#include <win_compat.h>

#define TEST_LOG_LEVEL LOG_LVL_DEBUG

#define SM "splintermail"

// read from our process's dup'd stdout asynchronously:
static pthread_t ar_pthread;
static pthread_mutex_t ar_mutex;
static dstr_t ar_buffer;
static derr_t ar_return = E_OK;

static void* async_reader_thread(void* arg){
    int* fd = (int*)arg;
    derr_t error = E_OK;
    while(true){
        char c;
        // read just one character
        ssize_t amnt_read = read(*fd, &c, 1);
        if(amnt_read == 0){
            break;
        }else if(amnt_read < 0){
            FFMT(stderr, NULL, "read failed: %x\n", FE(&errno) );
            ar_return = E_OS;
            break;
        }
        // add that one character to the buffer
        pthread_mutex_lock(&ar_mutex);
        error = FMT(&ar_buffer, "%x", FC(c));
        pthread_mutex_unlock(&ar_mutex);
        PROP_GO(error, done);
    }
done:
    ar_return = E_OK;
    return NULL;
}

static derr_t start_async_reader(int* fd){
    // allocate our buffer
    PROP( dstr_new(&ar_buffer, 8192) );
    // init the mutex
    pthread_mutex_init(&ar_mutex, NULL);
    // start the thread
    pthread_create(&ar_pthread, NULL, async_reader_thread, (void*)fd);
    return E_OK;
}

static derr_t stop_async_reader(dstr_t** out){
    // join the thread
    pthread_join(ar_pthread, NULL);
    // destroy mutex
    pthread_mutex_destroy(&ar_mutex);
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
    bool call_ditm_loop;
    struct ditm_loop_args_t ditm_loop_args;
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
    FFMT(stderr,NULL,"---- running test case %x ----\n", FS(test.test_name));
    // first reset several values
    looked_good = true;
    reason_log.len = 0;
    ditm_called = false;
    register_token_called = false;
    api_token_called = false;
    api_password_called = false;
    // copy some values
    ditm_loop_args = &test.ditm_loop_args;
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
    int ret = pipe(fds);
    if(ret != 0){
        perror("pipe");
        UH_OH("run_test_case failed to open pipe\n");
    }
    fflush(stdout);
    close(1);
    ret = dup(fds[1]);
    if(ret < 0){
        perror("pipe");
        UH_OH("run_test_case failed to dupe pipe\n");
    }
    close(fds[1]);
    // prepare local copy of test.argv
    // because otherwise argv might get modified, resulting in segfault
    int argc = 0;
    DSTR_VAR(argv_buffer, 4096);
    char **temp = test.argv;
    char* local_argv[256];
    while(*temp && (size_t)argc < sizeof(local_argv)/sizeof(*local_argv)){
        // next element of argv will start where argv_buffer.data ends now
        local_argv[argc] = argv_buffer.data + argv_buffer.len;
        PROP( FMT(&argv_buffer, "%x", FS(*temp)) );
        // include the null termination
        PROP( dstr_grow(&argv_buffer, argv_buffer.len + 1) );
        argv_buffer.len += 1;
        temp++;
        argc++;
    }
    PROP( FFMT(stderr,NULL,"running do_main\n") );
    // start the async_reader
    PROP( start_async_reader(&fds[0]) );
    // run the test
    int main_ret = do_main(argc, local_argv, false);
    // capture stdout (and stop_async_reader)
    fflush(stdout);
    close(1);
    dstr_t* out_buffer;
    derr_t error = stop_async_reader(&out_buffer);
    if(error){
        UH_OH("error in stop_async_reader\n");
        out_buffer->len = 0;
    }
    // restore the real stdout
    close(fds[0]);
    ret = dup(real_stdout_fd);
    if(ret < 0){
        perror("pipe");
        UH_OH("run_test_case failed to restore stdin\n");
    }
    PROP_GO( dstr_null_terminate(out_buffer), cleanup);
    // fix the logging
    logger_clear_outputs();
    logger_add_fileptr(TEST_LOG_LEVEL, stderr);
    // compare the results
    if(test.call_ditm_loop != ditm_called)
        UH_OH("run_test_case ditm_called exp %x but got %x\n",
              FI(test.call_ditm_loop), FI(ditm_called));
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
        LOG_ERROR("test %x failed:\n%x", FS(test.test_name), FD(&reason_log));
        LOG_ERROR("---- FAIL ----\n");
        ORIG_GO(E_VALUE, "test failed", cleanup);
    }else{
        LOG_ERROR("---- PASS ----\n");
    }
cleanup:
    dstr_free(out_buffer);
    return error;
}


int main(int argc, char** argv){
    derr_t error;
    PARSE_TEST_OPTIONS(argc, argv, &g_test_files, LOG_LVL_INFO);

    logger_add_fileptr(TEST_LOG_LEVEL, stderr);
    dstr_new(&reason_log, 4096);
    // first save the stdout file descriptor
    real_stdout_fd = dup(1);
    if(real_stdout_fd < 0){
        perror("dup");
        goto fail;
    }

    // non-ditm, non-api call tests
    {
        struct test_case_t test_case = {0};

        test_case.test_name = "help";
        test_case.argv = (char*[]){SM, "--help", NULL};
        PROP_GO( run_test_case(test_case), fail);
        test_case.argv = (char*[]){SM, "-h", NULL};
        PROP_GO( run_test_case(test_case), fail);

        test_case.test_name = "dump-conf";
        test_case.expect_out = "dump-conf\n";
        test_case.argv = (char*[]){SM, "--dump-conf", NULL};
        PROP_GO( run_test_case(test_case), fail);
        test_case.expect_out = NULL;

        test_case.test_name = "version";
        test_case.expect_out = "0.2.0\n";
        test_case.argv = (char*[]){SM, "--version", NULL};
        PROP_GO( run_test_case(test_case), fail);
        test_case.argv = (char*[]){SM, "-v", NULL};
        PROP_GO( run_test_case(test_case), fail);
        test_case.expect_out = NULL;

        // prep a config file
        DSTR_VAR(configpath, 4096);
        PROP_GO( FMT(&configpath, "%x/test_ui/testconf", FS(g_test_files)), fail);
        // prepare the expected stdout
        DSTR_VAR(expect_out, 4096);
        PROP_GO( FMT(&expect_out, "config %x\ndump-conf\nditm-dir 12345\n",
                                  FD(&configpath)), fail);
        test_case.test_name = "config";
        test_case.expect_out = expect_out.data;
        test_case.argv = (char*[]){SM, "--config", configpath.data, "--dump-conf", NULL};
        PROP_GO( run_test_case(test_case), fail);
        test_case.argv = (char*[]){SM, "-c", configpath.data, "--dump-conf", NULL};
        PROP_GO( run_test_case(test_case), fail);
        test_case.expect_out = NULL;


        test_case.test_name = "reject bad options";
        test_case.expect_return = 1;
        test_case.argv = (char*[]){SM, "ditm", "--avseij", NULL};
        PROP_GO( run_test_case(test_case), fail);
        test_case.argv = (char*[]){SM, "ditm", "-z", NULL};
        PROP_GO( run_test_case(test_case), fail);
        test_case.argv = (char*[]){SM, "ditm", "--pop-port", NULL};
        PROP_GO( run_test_case(test_case), fail);

        test_case.test_name = "reject non-existent config file";
        test_case.expect_return = 2;
        test_case.argv = (char*[]){SM, "-c", "fake_file", NULL};
        PROP_GO( run_test_case(test_case), fail);

        test_case.test_name = "missing positional args";
        test_case.expect_return = 3;
        test_case.argv = (char*[]){SM, NULL};
        PROP_GO( run_test_case(test_case), fail);

        test_case.test_name = "invalid pop-port numbers";
        test_case.expect_return = 4;
        test_case.argv = (char*[]){SM, "--pop-port", "0", "ditm", NULL};
        PROP_GO( run_test_case(test_case), fail);
        test_case.argv = (char*[]){SM, "--pop-port", "65536", "ditm", NULL};
        PROP_GO( run_test_case(test_case), fail);

        test_case.test_name = "need --user flag";
        test_case.expect_return = 5;
        test_case.users = (char*[]){NULL};
        test_case.argv = (char*[]){SM, "list_aliases", NULL};
        PROP_GO( run_test_case(test_case), fail);
        test_case.users = (char*[]){"a@fqdn", "b@fqdn", NULL};
        test_case.argv = (char*[]){SM, "list_aliases", NULL};
        PROP_GO( run_test_case(test_case), fail);
        test_case.users = NULL;

        test_case.test_name = "password confirmation fails";
        test_case.expect_return = 6;
        test_case.passwords = (char*[]){"a", "b", "c", NULL};
        test_case.argv = (char*[]){SM, "-u", "a", "change_password", NULL};
        PROP_GO( run_test_case(test_case), fail);
        test_case.passwords = NULL;

    }

    // test DITM args
    {
        struct test_case_t test_case = {
            .call_ditm_loop = true,
            .ditm_loop_args = {
                .rhost = "splintermail.com",
                .rport = 995,
                .ditm_dir = NULL,
                .port = 1995,
                .api_host = "splintermail.com",
                .api_port = 443,
                .cert = NULL,
                .key = NULL,
                .to_return = E_OK,
            },
        };

        test_case.test_name = "ditm basic test";
        test_case.argv = (char*[]){SM, "ditm", NULL};
        PROP_GO( run_test_case(test_case), fail);

        test_case.test_name = "ditm uncaught error";
        test_case.expect_return = 255;
        test_case.ditm_loop_args.to_return = E_INTERNAL;
        test_case.argv = (char*[]){SM, "ditm", NULL};
        PROP_GO( run_test_case(test_case), fail);
        test_case.expect_return = 0;
        test_case.ditm_loop_args.to_return = E_OK;

        test_case.test_name = "ditm pop-port";
        test_case.ditm_loop_args.port = 1234;
        test_case.argv = (char*[]){SM, "ditm", "--pop-port", "1234", NULL};
        PROP_GO( run_test_case(test_case), fail);
        test_case.ditm_loop_args.port = 1995;

        test_case.test_name = "ditm ditm-dir";
        test_case.ditm_loop_args.ditm_dir = "some_dir";
        test_case.argv = (char*[]){SM, "ditm", "--ditm-dir", "some_dir", NULL};
        PROP_GO( run_test_case(test_case), fail);
        test_case.ditm_loop_args.ditm_dir = NULL;

        test_case.test_name = "ditm cert";
        test_case.ditm_loop_args.cert = "some_file";
        test_case.argv = (char*[]){SM, "ditm", "--cert", "some_file", NULL};
        PROP_GO( run_test_case(test_case), fail);
        test_case.ditm_loop_args.cert = NULL;

        test_case.test_name = "ditm key";
        test_case.ditm_loop_args.key = "some_file";
        test_case.argv = (char*[]){SM, "ditm", "--key", "some_file", NULL};
        PROP_GO( run_test_case(test_case), fail);
        test_case.ditm_loop_args.key = NULL;
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
        PROP_GO( run_test_case(test_case), fail);

        test_case.test_name = "delete useless token test";
        test_case.argv = (char*[]){SM, "list_aliases", NULL};
        test_case.api_token_args.code = 403;
        test_case.expect_return = 8;
        test_case.expect_out = NULL;
        PROP_GO( run_test_case(test_case), fail);

        test_case.test_name = "misc failure test";
        test_case.argv = (char*[]){SM, "list_aliases", NULL};
        test_case.api_token_args.code = 500;
        test_case.expect_return = 9;
        test_case.expect_out = NULL;
        PROP_GO( run_test_case(test_case), fail);
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
        PROP_GO( run_test_case(test_case), fail);

        // case "A", different variety
        test_case.test_name = "bad permissions user_dir";
        test_case.users = (char*[]){"no.perms.user@fqdn", NULL};
        test_case.api_password_args.user = "no.perms.user@fqdn";
        test_case.passwords = (char*[]){"pass", NULL};
        test_case.argv = (char*[]){SM, "list_aliases", NULL};
        PROP_GO( run_test_case(test_case), fail);

        // case "C" -> noreg ("W")
        test_case.test_name = "bad creds";
        test_case.users = (char*[]){"user@fqdn", NULL};
        test_case.api_password_args.user = "user@fqdn";
        test_case.find_token = true;
        test_case.read_token_error = E_PARAM;
        test_case.passwords = (char*[]){"pass", NULL};
        test_case.argv = (char*[]){SM, "list_aliases", NULL};
        PROP_GO( run_test_case(test_case), fail);
        test_case.find_token = false;
        test_case.read_token_error = E_OK;

        // case "D"
        test_case.test_name = "no access to creds";
        test_case.users = (char*[]){"no.creds.access.user@fqdn", NULL};
        test_case.api_password_args.user = "no.creds.access.user@fqdn";
        test_case.passwords = (char*[]){"pass", NULL};
        test_case.argv = (char*[]){SM, "list_aliases", NULL};
        PROP_GO( run_test_case(test_case), fail);

        // case "E" -> noreg ("W")
        test_case.test_name = "no creds yet";
        test_case.creatables = (char*[]){"noreg.user@fqdn/api_token.json", NULL};
        test_case.users = (char*[]){"noreg.user@fqdn", NULL};
        test_case.api_password_args.user = "noreg.user@fqdn";
        test_case.passwords = (char*[]){"pass", NULL};
        test_case.argv = (char*[]){SM, "list_aliases", NULL};
        PROP_GO( run_test_case(test_case), fail);
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
        PROP_GO( run_test_case(test_case), fail);
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
        PROP_GO( run_test_case(test_case), fail);
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
        PROP_GO( run_test_case(test_case), fail);

        test_case.test_name = "add token test";
        test_case.passwords = (char*[]){"pass", NULL};
        test_case.argv = (char*[]){SM, "add_token", NULL};
        PROP_GO( run_test_case(test_case), fail);

        test_case.test_name = "delete all mail test";
        test_case.passwords = (char*[]){"pass", NULL};
        test_case.strings = (char*[]){"I really want to do this", NULL};
        test_case.argv = (char*[]){SM, "delete_all_mail", NULL};
        PROP_GO( run_test_case(test_case), fail);

        test_case.test_name = "delete all aliases test";
        test_case.passwords = (char*[]){"pass", NULL};
        test_case.strings = (char*[]){"I really want to do this", NULL};
        test_case.argv = (char*[]){SM, "delete_all_aliases", NULL};
        PROP_GO( run_test_case(test_case), fail);

        test_case.test_name = "delete account test";
        test_case.passwords = (char*[]){"pass", NULL};
        test_case.strings = (char*[]){"I really want to do this", NULL};
        test_case.argv = (char*[]){SM, "delete_account", NULL};
        PROP_GO( run_test_case(test_case), fail);

        test_case.test_name = "failed confirmation test";
        test_case.passwords = (char*[]){"pass", NULL};
        test_case.strings = (char*[]){"i really want to do this", NULL};
        test_case.argv = (char*[]){SM, "delete_account", NULL};
        test_case.call_api_password = false;
        test_case.expect_out = NULL;
        test_case.expect_return = 7;
        PROP_GO( run_test_case(test_case), fail);
    }

    LOG_ERROR("PASS\n");
    dstr_free(&reason_log);
    return 0;

fail:

    LOG_ERROR("FAIL\n");
    dstr_free(&reason_log);
    return 1;
}
