#include <stdlib.h>
#include <time.h>
#include <limits.h>
#include <signal.h>

#include <libdstr/libdstr.h>
#include <libimap/libimap.h>

#include "test_utils.h"

static derr_t test_dgetenv(void){
    derr_t e = E_OK;

    bool found;
    // variable does not exist
    dstr_t dummy = dgetenv(DSTR_LIT("DUMMY"), &found);
    if(found)
        ORIG(&e, E_VALUE, "found=true when variable did not exist");
    if(dummy.len)
        ORIG(&e, E_VALUE, "nonempty-return value when variable did not exist");

    PROP(&e, dsetenv(DSTR_LIT("DUMMY"), DSTR_LIT("")) );

#ifndef _WIN32 // UNIX (windows putenv() unsets empty env vars)
    // variable is empty
    dummy = dgetenv(DSTR_LIT("DUMMY"), &found);
    if(!found)
        ORIG(&e, E_VALUE, "found=false when variable did exist");
    if(dummy.len)
        ORIG(&e, E_VALUE, "nonempty-return value when variable was empty");
#endif

    PROP(&e, dsetenv(DSTR_LIT("DUMMY"), DSTR_LIT("dummy")) );

    // variable is non-empty
    dummy = dgetenv(DSTR_LIT("DUMMY"), &found);
    if(!found)
        ORIG(&e, E_VALUE, "found=false when variable did exist");
    if(dstr_cmp(&dummy, &DSTR_LIT("dummy")) != 0)
        ORIG(&e, E_VALUE, "wrong return value when variable was non-empty");

    return e;
}

#ifndef _WIN32

static derr_t reader_writer_child(void){
    derr_t e = E_OK;

    DSTR_VAR(in, 128);

    // read stdin
    PROP(&e, dstr_read_all(0, &in) );

    // echo to stdout
    PROP(&e, dstr_write(1, &in) );

    // echo to stderr
    PROP(&e, dstr_write(2, &in) );

    return e;
}

static derr_t test_fork_ex_pipes(void){
    derr_t e = E_OK;

    pid_t pid;
    bool child;

    int std_in;
    int std_out;
    int std_err;

    PROP(&e, dfork_ex(&pid, &child, &std_in, &std_out, &std_err) );
    if(child) RUN_CHILD_PROC(reader_writer_child(), 3);

    DSTR_STATIC(msg, "asdf");
    DSTR_VAR(out, 128);
    DSTR_VAR(err, 128);

    // write to stdin
    PROP_GO(&e, dstr_write(std_in, &msg), cu_child);
    PROP_GO(&e, dclose(std_in), cu_child);

    // read echos
    PROP_GO(&e, dstr_read_all(std_out, &out), cu_child);
    PROP_GO(&e, dstr_read_all(std_err, &err), cu_child);

    EXPECT_DM(&e, "stderr", &err, &msg);
    EXPECT_DM(&e, "stdout", &out, &msg);

cu_child:
    if(is_error(e)){
        DROP_CMD( dkill(pid, SIGKILL) );
        DROP_CMD( dwaitpid(pid, NULL, 0) );
    }else{
        // make sure the child process exited correctly
        int ret;
        PROP(&e, dwait(pid, &ret, NULL) );
        if(ret){
            ORIG(&e, E_VALUE, "child process exited with non-zero exit code");
        }
    }

    return e;
}

static derr_t devnull_child(void){
    derr_t e = E_OK;

    DSTR_VAR(in, 128);

    // read stdin (should be EOF)
    PROP(&e, dstr_read_all(0, &in) );
    if(in.len != 0){
        exit(2);
    }

    // write to stdout (should succeed)
    PROP(&e, dstr_write(1, &DSTR_LIT("junk")) );

    // write to stdout (should succeed)
    PROP(&e, dstr_write(2, &DSTR_LIT("junk")) );

    return e;
}

static derr_t test_fork_ex_devnull(void){
    derr_t e = E_OK;

    pid_t pid;
    bool child;

    PROP(&e, dfork_ex(&pid, &child, &devnull, &devnull, &devnull) );
    if(child) RUN_CHILD_PROC(devnull_child(), 3);

    // make sure the child process exits correctly
    int ret;
    PROP(&e, dwait(pid, &ret, NULL) );
    if(ret){
        ORIG(&e, E_VALUE, "child process exited with non-zero exit code");
    }

    return e;
}

static derr_t call_env(bool extend){
    derr_t e = E_OK;

    char **base = extend ? compat_environ : NULL;
    char **env;

    PROP(&e, make_env(&env, base, DSTR_LIT("EXTRA=extra")) );

    PROP(&e, dexec(DSTR_LIT("env"), env, DSTR_LIT("CLI=cli")) );

    return e;
}

static derr_t test_extend_subproc_env(bool extend){
    derr_t e = E_OK;

    // set an environment variable
    PROP(&e, dsetenv(DSTR_LIT("DUMMY"), DSTR_LIT("dummy")) );

    pid_t pid;
    bool child;

    int std_out;

    PROP(&e, dfork_ex(&pid, &child, NULL, &std_out, NULL) );
    if(child) RUN_CHILD_PROC(call_env(extend), 3);

    dstr_t outbuf;
    PROP_GO(&e, dstr_new(&outbuf, 4096), cu_child);

    PROP_GO(&e, dstr_read_all(std_out, &outbuf), cu_buf);

    // validate the variables we expect to find
    bool found_dummy = dstr_count(&outbuf, &DSTR_LIT("DUMMY=dummy\n"));
    if(found_dummy != extend)
        ORIG_GO(&e, E_VALUE, "found_dummy != extend", cu_buf);

    if(!dstr_count(&outbuf, &DSTR_LIT("EXTRA=extra\n")))
        ORIG_GO(&e, E_VALUE, "did not find EXTRA", cu_buf);

    if(!dstr_count(&outbuf, &DSTR_LIT("CLI=cli\n")))
        ORIG_GO(&e, E_VALUE, "did not find CLI", cu_buf);

cu_buf:
    dstr_free(&outbuf);

cu_child:
    if(is_error(e)){
        DROP_CMD( dkill(pid, SIGKILL) );
        DROP_CMD( dwaitpid(pid, NULL, 0) );
    }else{
        // make sure the child process exited correctly
        int ret;
        PROP(&e, dwait(pid, &ret, NULL) );
        if(ret){
            ORIG(&e, E_VALUE, "child process exited with non-zero exit code");
        }
    }

    return e;
}

#endif // _WIN32

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_dgetenv(), test_fail);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
    PROP_GO(&e, test_fork_ex_pipes(), test_fail);
    PROP_GO(&e, test_fork_ex_devnull(), test_fail);
    PROP_GO(&e, test_extend_subproc_env(true), test_fail);
    PROP_GO(&e, test_extend_subproc_env(false), test_fail);
#endif // _WIN32

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
