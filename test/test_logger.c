#include <string.h>

#include <common.h>
#include <logger.h>

#include "test_utils.h"

// a function that always fails
static derr_t orig_something(void){
    derr_t e = E_OK;
    ORIG(&e, E_VALUE, "generated error");
}

// a function that propagates somethign that always fails
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
    TRACE(&e_test, "%x", FD(&temp));
    if(e_test.msg.len != temp.len + strlen("Hello world\n")){
        ORIG_GO(&e, E_VALUE, "TRACE onto filled msg failed", free_temp);
    }

free_temp:
    dstr_free(&temp);

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
    }else{
        ORIG(&e, E_VALUE, "failed to generate error");
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

done:
    DROP_VAR(&e1);
    DROP_VAR(&e2);
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


int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_WARN);

    PROP_GO(&e, test_trace(), test_fail);
    PROP_GO(&e, test_prop(), test_fail);
    PROP_GO(&e, test_sequential_prop(), test_fail);
    PROP_GO(&e, test_rethrow(), test_fail);
    PROP_GO(&e, test_merge(), test_fail);

    int exitval;
test_fail:
    exitval = is_error(e);
    DUMP(e);
    DROP_VAR(&e);
    printf("%s\n", exitval ? "FAIL" : "PASS");
    return exitval;
}
