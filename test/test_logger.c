#include <string.h>

#include <libdstr/libdstr.h>

#include "test_utils.h"

// a function that always fails
static derr_t orig_something(void){
    derr_t e = E_OK;
    ORIG(&e, E_VALUE, "generated error: arg=%x", FS("val"));
}

// a function that propagates something that always fails
static derr_t prop_something(void){
    derr_t e = E_OK;
    PROP(&e, orig_something());
    return e;
}

static derr_t test_trace(void){
    derr_t e = E_OK;
    derr_t e_test = E_OK;

    // test that TRACE appends nicely to empty errors
    TRACE(&e_test, "Hello %x\n", FS("world"));
    if(dstr_cmp(&e_test.msg, &DSTR_LIT("Hello world\n")) != 0){
        ORIG_GO(&e, E_VALUE, "TRACE onto empty msg failed", fail);
    }

    // fill up a dstring big enough to force a realloc
    dstr_t temp;
    PROP_GO(&e, dstr_new(&temp, e_test.msg.size  + 1), fail);
    memset(temp.data, '-', temp.size);
    temp.len = temp.size;

    // test that TRACE works nicely with a realloc
    TRACE(&e_test, "%x", FD(temp));
    if(e_test.msg.len != temp.len + strlen("Hello world\n")){
        ORIG_GO(&e, E_VALUE, "TRACE onto filled msg failed", free_temp);
    }

free_temp:
    dstr_free(&temp);

fail:
    DROP_VAR(&e_test);
    return e;
}

static derr_t test_orig(void){
    derr_t e = E_OK;
    derr_t e_test = E_OK;

    // test that ORIG sticks things together in the expected ways
    e_test = orig_something();

    DSTR_STATIC(exp,
        "ERROR: generated error: arg=val\n"
        "originating VALUEERROR from file test/test_logger.c: "
        "orig_something(), line 10\n"
    );
    if(!dstr_eq(e_test.msg, exp)){
        ORIG_GO(&e,
            E_VALUE, "\nexpected: \"%x\"\nbut got:  \"%x\"", fail,
            FD_DBG(exp), FD_DBG(e_test.msg)
        );
    }

fail:
    DROP_VAR(&e_test);
    return e;
}

static derr_t test_prop(void){
    derr_t e = E_OK;
    PROP_GO(&e, prop_something(), drop);

    // should never get here
    ORIG(&e, E_VALUE, "PROP_GO failed");

drop:
    DROP_VAR(&e);
    return e;
}

static derr_t test_sequential_prop(void){
    derr_t e = E_OK;
    size_t trace_len;

    IF_PROP(&e, orig_something()){
        // just allow this error
        trace_len = e.msg.len;
    }else{
        ORIG(&e, E_VALUE, "failed to generate error");
    }

    IF_PROP(&e, orig_something()){
        // make sure the trace grew
        if(trace_len >= e.msg.len){
            ORIG(&e, E_VALUE, "trace did not grow");
        }
        trace_len = e.msg.len;
    }else{
        ORIG(&e, E_VALUE, "failed to generate error");
    }

    // ensure that prop doesn't drop our error
    IF_PROP(&e, E_OK){
        ORIG(&e, E_VALUE, "still in error case");
    }else{
        if(!is_error(e)){
            ORIG(&e, E_VALUE, "we erased the error");
        }
        if(e.msg.len != trace_len){
            ORIG(&e, E_VALUE, "the message was changed");
        }
    }

    // the real error checking happens in valgrind
    DROP_VAR(&e);

    return e;
}

static derr_t test_rethrow(void){
    derr_t e = E_OK;
    derr_t e1 = E_OK;
    derr_t e2 = E_OK;

    // populate e
    TRACE(&e1, "e1-msg\n");
    IF_PROP(&e1, orig_something()){}

    // populate e2
    TRACE(&e2, "e2-msg\n");
    IF_PROP(&e2, orig_something()){}
    e2.type = E_CONN;

    RETHROW_GO(&e1, &e2, E_VALUE, msg_check);

    // make sure that both messages are still present
msg_check:
    if(dstr_count(&e1.msg, &DSTR_LIT("e1-msg")) == 0){
        ORIG_GO(&e, E_VALUE, "e1-msg not present", done);
    }
    if(dstr_count(&e1.msg, &DSTR_LIT("e2-msg")) == 0){
        ORIG_GO(&e, E_VALUE, "e2-msg not present", done);
    }

    // make sure e2 has been cleaned up
    if(e2.type != E_NONE || e2.msg.data != NULL){
        ORIG_GO(&e, E_VALUE, "e2 not cleaned up", done);
    }

    // make sure e1 took the type E_VALUE specified by RETHROW_GO
    if(e1.type != E_VALUE){
        ORIG_GO(&e, E_VALUE, "e1 type is wrong", done);
    }

    size_t oldmsglen = e1.msg.len;

    // ensure we can RETHROW a single variable
    CATCH_ANY(&e1){
        TRACE_RETHROW(&e1, &e1, E_CONN);
    }else{
        ORIG_GO(&e, E_INTERNAL, "expected an error", done);
    }

    EXPECT_P_GO(&e, "e1.type", e1.type, E_CONN, done);
    EXPECT_U_GT_GO(&e, "e.msg.len", e1.msg.len, oldmsglen, done);


done:
    DROP_VAR(&e1);
    DROP_VAR(&e2);
    return e;
}

static derr_t test_nofail(void){
    derr_t e = E_OK;
    derr_t e1 = E_OK;

    NOFAIL_GO(&e1, E_VALUE, orig_something(), check_match);
check_match:
    if(e1.type != E_INTERNAL){
        ORIG_GO(&e, E_VALUE, "match failed", done);
    }

    // reset error
    DROP_VAR(&e1);

    NOFAIL_GO(&e1, E_PARAM, orig_something(), check_passthru);
check_passthru:
    if(e1.type != E_VALUE){
        ORIG_GO(&e, E_VALUE, "passthru failed", done);
    }

done:
    DROP_VAR(&e1);
    return e;
}

static derr_t test_merge(void){
    // test the length of the message from orig_something
    derr_t e = orig_something();
    size_t len = e.msg.len;
    DROP_VAR(&e);

    // merging an error into an E_OK should look like the plain old message
    MERGE_CMD(&e, orig_something(), "merge-msg");
    if(e.msg.len != len){
        ORIG_GO(&e, E_VALUE, "merge (E_OK<-error) message is wrong", done);
    }

    // now merging nothing into an error should have no effect
    MERGE_CMD(&e, E_OK, "some stuff");
    if(e.msg.len != len){
        ORIG_GO(&e, E_VALUE, "merge (error<-E_OK) message is wrong", done);
    }

    // merging a message into an error should leave a merge message
    MERGE_CMD(&e, orig_something(), "merge-msg");
    if(dstr_count(&e.msg, &DSTR_LIT("merge-msg")) != 1){
        ORIG_GO(&e, E_VALUE, "merge (error<-error) message is wrong", done);
    }

done:
    DROP_VAR(&e);
    return e;
}

// this test is only useful when run under valgrind
static derr_t test_merge_noleak(void){
    // merge two errors, then free
    {
        derr_t e = orig_something();
        MERGE_CMD(&e, orig_something(), "merge_cmd 2");
        DROP_VAR(&e);
    }
    // same thing using MERGE_VAR
    {
        derr_t e1 = orig_something();
        derr_t e2 = orig_something();
        MERGE_VAR(&e1, &e2, "merge_var 2");
        DROP_VAR(&e1);
    }
    // merge into E_OK, then free
    {
        derr_t e = E_OK;
        MERGE_CMD(&e, orig_something(), "merge_cmd 2");
        DROP_VAR(&e);
    }
    // same thing using MERGE_VAR
    {
        derr_t e1 = E_OK;
        derr_t e2 = orig_something();
        MERGE_VAR(&e1, &e2, "merge_var 2");
        DROP_VAR(&e1);
    }
    // merge E_OK into an error, then free
    {
        derr_t e = orig_something();
        MERGE_CMD(&e, E_OK, "merge_cmd 2");
        DROP_VAR(&e);
    }
    // same thing using MERGE_VAR
    {
        derr_t e1 = orig_something();
        derr_t e2 = E_OK;
        MERGE_VAR(&e1, &e2, "merge_var 2");
        DROP_VAR(&e1);
    }
    return E_OK;
}

static derr_t test_log_stack_and_heap(void){
    derr_t e = E_OK;

    DSTR_VAR(temp, 256);
    DSTR_VAR(log, 256);

    PROP(&e, mkdir_temp("test-logger", &temp) );
    PROP_GO(&e, FMT(&log, "%x/log", FD(temp)), cu);
    PROP_GO(&e, logger_add_filename(LOG_LVL_DEBUG, log.data), cu);

    char *s =
        "128chars--------------------------------------------------------"
        "----------------------------------------------------------------"
    ;

    DSTR_VAR(exp, 4096);

    // log something short
    LOG_DEBUG("%x%x%x%x\n", FS(s), FS(s), FS(s), FS(s));
    PROP_GO(&e, FMT(&exp, "%x%x%x%x\n", FS(s), FS(s), FS(s), FS(s)), cu);

    // log something long
    LOG_DEBUG(
        "%x%x%x%x%x%x%x%x\n",
        FS(s), FS(s), FS(s), FS(s), FS(s), FS(s), FS(s), FS(s)
    );
    PROP_GO(&e,
        FMT(&exp,
            "%x%x%x%x%x%x%x%x\n",
            FS(s), FS(s), FS(s), FS(s), FS(s), FS(s), FS(s), FS(s)
        ),
    cu);

    DSTR_VAR(got, 4096);
    DSTR_VAR(norm, 4096);
    PROP_GO(&e, dstr_read_file(log.data, &got), cu);

    // normalize line endings
    LIST_PRESET(dstr_t, find, DSTR_LIT("\r\n"));
    LIST_PRESET(dstr_t, repl, DSTR_LIT("\n"));
    PROP_GO(&e, dstr_recode(&got, &norm, &find, &repl, false), cu);

    EXPECT_DM_GO(&e, "logfile", norm, exp, cu);

cu:
    DROP_CMD( rm_rf_path(&SBD(temp)) );
    logger_clear_outputs();
    logger_add_fileptr(LOG_LVL_WARN, stdout);

    return e;
}


int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_WARN);

    PROP_GO(&e, test_trace(), test_fail);
    PROP_GO(&e, test_orig(), test_fail);
    PROP_GO(&e, test_prop(), test_fail);
    PROP_GO(&e, test_sequential_prop(), test_fail);
    PROP_GO(&e, test_rethrow(), test_fail);
    PROP_GO(&e, test_nofail(), test_fail);
    PROP_GO(&e, test_merge(), test_fail);
    PROP_GO(&e, test_merge_noleak(), test_fail);
    PROP_GO(&e, test_log_stack_and_heap(), test_fail);

    int exitval;
test_fail:
    exitval = is_error(e);
    DUMP(e);
    DROP_VAR(&e);
    printf("%s\n", exitval ? "FAIL" : "PASS");
    return exitval;
}
