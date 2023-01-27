#include "libdstr/libdstr.h"
#include "libduv/libduv.h"

#include "test/test_utils.h"

static rstream_i *R;
static dstr_rstream_t d1;
static dstr_rstream_t d2;
static derr_t E = {0};
static bool r2_awaited = false;
static bool r_awaited = false;
static size_t cb_order = 0;

#define DEF_READ_CB(name, order, text, exp_ok) \
    static void name( \
        rstream_i *r, rstream_read_t *read, dstr_t buf, bool ok \
    ){ \
        (void)r; \
        (void)read; \
        DSTR_STATIC(exp, text); \
        EXPECT_U_GO(&E, #name ":order", cb_order++, order, fail); \
        EXPECT_B_GO(&E, #name ":ok", ok, exp_ok, fail); \
        EXPECT_D_GO(&E, #name ":buf", &buf, &exp, fail); \
    fail: \
        return; \
    }

DEF_READ_CB(read_cb1a, 0, "hell", true)
DEF_READ_CB(read_cb2a, 1, "o ",   true)
DEF_READ_CB(read_cb3a, 2, "worl", true)
DEF_READ_CB(read_cb4a, 3, "d!",   true)
DEF_READ_CB(read_cb5a, 4, "",     true)
DEF_READ_CB(read_cb6a, 5, "",     true)

DEF_READ_CB(read_cb1b, 6,  "",   false)
DEF_READ_CB(read_cb2b, 7,  "",   false)
DEF_READ_CB(read_cb3b, 8,  "",   false)
DEF_READ_CB(read_cb4b, 9,  "",   false)
DEF_READ_CB(read_cb5b, 10, "",   false)
DEF_READ_CB(read_cb6b, 11, "",   false)

static void await_cb_r2_a(rstream_i *r, derr_t e){
    (void)r;
    r2_awaited = true;
    MERGE_VAR(&E, &e, "await_cb_r2");
}

static void await_cb_a(rstream_i *r, derr_t e){
    (void)r;
    // r2 must already have been awaited
    if(!r2_awaited){
        TRACE_ORIG(&E, E_VALUE, "r2 not yet awaited");
    }
    r_awaited = true;
    MERGE_VAR(&E, &e, "await_cb");
}

static void await_cb_r2_b(rstream_i *r, derr_t e){
    // even when we cancel r2, concat should intercept the error
    (void)r;
    r2_awaited = true;
    MERGE_VAR(&E, &e, "await_cb_r2");
}

static void await_cb_b(rstream_i *r, derr_t e){
    (void)r;
    // r2 must already have been awaited
    if(!r2_awaited){
        TRACE_ORIG(&E, E_VALUE, "r2 not yet awaited");
    }
    // we expect E_CANCELED to have propagated through the concat as E_INTERNAL
    if(!is_error(e)){
        TRACE_ORIG(&E, E_VALUE, "r was not canceled");
    }
    if(e.type == E_INTERNAL) DROP_VAR(&e);
    r_awaited = true;
    MERGE_VAR(&E, &e, "await_cb");
}

static derr_t test_concat(void){
    derr_t e = E_OK;

    manual_scheduler_t scheduler;
    scheduler_i *sched = manual_scheduler(&scheduler);
    rstream_concat_t concat;

    DSTR_STATIC(text1, "hello ");
    DSTR_STATIC(text2, "world!");

    rstream_i *r1 = dstr_rstream(&d1, sched, text1);
    rstream_i *r2 = dstr_rstream(&d2, sched, text2);
    stream_must_await_first(r2, await_cb_r2_a);

    R = rstream_concat(&concat, sched, r1, r2);
    stream_must_await_first(R, await_cb_a);

    // submit all the reads
    rstream_read_t read1;
    DSTR_VAR(buf1, 4);
    stream_must_read(R, &read1, buf1, read_cb1a);

    rstream_read_t read2;
    DSTR_VAR(buf2, 4);
    stream_must_read(R, &read2, buf2, read_cb2a);

    rstream_read_t read3;
    DSTR_VAR(buf3, 4);
    stream_must_read(R, &read3, buf3, read_cb3a);

    rstream_read_t read4;
    DSTR_VAR(buf4, 4);
    stream_must_read(R, &read4, buf4, read_cb4a);

    rstream_read_t read5;
    DSTR_VAR(buf5, 4);
    stream_must_read(R, &read5, buf5, read_cb5a);

    rstream_read_t read6;
    DSTR_VAR(buf6, 4);
    stream_must_read(R, &read6, buf6, read_cb6a);

    // run to completion
    manual_scheduler_run(&scheduler);
    MERGE_VAR(&e, &E, "scheduler run");
    CHECK(&e);
    if(!R->awaited) ORIG(&e, E_VALUE, "R not marked as awaited");
    if(!r_awaited) ORIG(&e, E_VALUE, "R not actually awaited");
    r2_awaited = false;
    r_awaited = false;

    // again, but handle failing rstream
    r1 = dstr_rstream(&d1, sched, text1);
    r2 = dstr_rstream(&d2, sched, text2);
    stream_must_await_first(r2, await_cb_r2_b);
    r2->cancel(r2);

    R = rstream_concat(&concat, sched, r1, r2);
    stream_must_await_first(R, await_cb_b);

    buf1.len = buf2.len = buf3.len = buf4.len = 0;
    stream_must_read(R, &read1, buf1, read_cb1b);
    stream_must_read(R, &read2, buf2, read_cb2b);
    stream_must_read(R, &read3, buf3, read_cb3b);
    stream_must_read(R, &read4, buf4, read_cb4b);
    stream_must_read(R, &read5, buf5, read_cb5b);
    stream_must_read(R, &read6, buf6, read_cb6b);

    // run to completion
    manual_scheduler_run(&scheduler);
    MERGE_VAR(&e, &E, "scheduler run");
    CHECK(&e);
    if(!R->awaited) ORIG(&e, E_VALUE, "R not marked as awaited");
    if(!r_awaited) ORIG(&e, E_VALUE, "R not actually awaited");

    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_concat(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
