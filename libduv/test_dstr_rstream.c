#include "libdstr/libdstr.h"
#include "libduv/libduv.h"

#include "test/test_utils.h"

static rstream_i *R;
static dstr_rstream_t rstream_obj;
static derr_t E = {0};
static char total_read_buf[32];
static dstr_t total_read;

static derr_t do_read_cb(dstr_t buf, dstr_t exp){
    derr_t e = E_OK;
    if(!dstr_eq(buf, exp)){
        ORIG(&e,
            E_VALUE, "expected \"%x\" but got \"%x\"", FD(&exp), FD(&buf)
        );
    }
    return e;
}

static void read_cb_eof(rstream_i *r, rstream_read_t *read, dstr_t buf){
    (void)r;
    (void)read;
    MERGE_CMD(&E, do_read_cb(buf, DSTR_LIT("")), "read_cb");
}

static void read_cb1(rstream_i *r, rstream_read_t *read, dstr_t buf){
    (void)r;
    (void)read;
    MERGE_CMD(&E, do_read_cb(buf, DSTR_LIT("h")), "read_cb");
}

static void read_cb2(rstream_i *r, rstream_read_t *read, dstr_t buf){
    (void)r;
    (void)read;
    MERGE_CMD(&E, do_read_cb(buf, DSTR_LIT("ello world!")), "read_cb");
}

static void read_cb3(rstream_i *r, rstream_read_t *read, dstr_t buf){
    (void)r;
    (void)read;
    MERGE_CMD(&E, do_read_cb(buf, DSTR_LIT("")), "read_cb");
    // first of two eof cbs, don't close the r
}

static void read_cb5(rstream_i *r, rstream_read_t *read, dstr_t buf){
    (void)r;
    (void)read;
    MERGE_CMD(&E, do_read_cb(buf, DSTR_LIT("hello world!")), "read_cb");
}

static void read_never_cb(rstream_i *r, rstream_read_t *read, dstr_t buf){
    (void)r;
    (void)read;
    (void)buf;
    TRACE_ORIG(&E, E_VALUE, "read_never_cb");
}

static void read_cb7(rstream_i *r, rstream_read_t *read, dstr_t buf){
    (void)read;
    MERGE_CMD(&E, do_read_cb(buf, DSTR_LIT("hello ")), "read_cb");
    stream_must_read(r, read, buf, read_never_cb);
    r->cancel(r);
}

static void await_cb(rstream_i *r, derr_t e, link_t *reads){
    (void)r;
    MERGE_VAR(&E, &e, "await_cb");
    if(!link_list_isempty(reads)){
        TRACE_ORIG(&E, E_VALUE, "non-empty reads in await_cb");
    }
}

static void await_cb7(rstream_i *r, derr_t e, link_t *reads){
    (void)r;
    if(is_error(e)){
        if(e.type != E_CANCELED){
            MERGE_VAR(&E, &e, "await_cb");
        }
    }else{
        TRACE_ORIG(&E, E_VALUE, "stream not E_CANCELED");
    }
    if(link_list_isempty(reads)){
        TRACE_ORIG(&E, E_VALUE, "empty reads in await_cb7");
    }
}

static derr_t test_rstream(void){
    derr_t e = E_OK;

    manual_scheduler_t scheduler;
    scheduler_i *sched = manual_scheduler(&scheduler);

    DSTR_STATIC(base, "hello world!");

    R = dstr_rstream(&rstream_obj, sched, base);
    stream_must_await_first(R, await_cb);

    // submit a read that will be filled
    rstream_read_t read1;
    DSTR_VAR(buf1, 1);
    stream_must_read(R, &read1, buf1, read_cb1);

    // submit another read that won't be filled
    rstream_read_t read2;
    DSTR_VAR(buf2, 32);
    stream_must_read(R, &read2, buf2, read_cb2);

    // submit another read that will be eof'd
    rstream_read_t read3;
    DSTR_VAR(buf3, 1);
    stream_must_read(R, &read3, buf3, read_cb3);

    // submit one last read that will also be eof'd
    rstream_read_t read4;
    DSTR_VAR(buf4, 1);
    stream_must_read(R, &read4, buf4, read_cb_eof);

    // run to completion
    manual_scheduler_run(&scheduler);
    MERGE_VAR(&e, &E, "scheduler run");
    CHECK(&e);
    if(!R->awaited) ORIG(&e, E_VALUE, "R not awaited");

    // start over
    R = dstr_rstream(&rstream_obj, sched, base);
    stream_must_await_first(R, await_cb);

    // submit a read that will be filled exactly
    rstream_read_t read5;
    DSTR_VAR(buf5, 12);
    stream_must_read(R, &read5, buf5, read_cb5);

    // submit another read that will be eof'd
    rstream_read_t read6;
    DSTR_VAR(buf6, 1);
    stream_must_read(R, &read6, buf6, read_cb_eof);

    // run to completion
    manual_scheduler_run(&scheduler);
    MERGE_VAR(&e, &E, "scheduler run");
    CHECK(&e);
    if(!R->awaited) ORIG(&e, E_VALUE, "R not awaited");

    // start over
    R = dstr_rstream(&rstream_obj, sched, base);
    stream_must_await_first(R, await_cb7);

    // read half of the base, then close it early
    rstream_read_t read7;
    DSTR_VAR(buf7, 6);
    stream_must_read(R, &read7, buf7, read_cb7);

    // run to completion
    manual_scheduler_run(&scheduler);
    MERGE_VAR(&e, &E, "scheduler run");
    CHECK(&e);
    if(!R->awaited) ORIG(&e, E_VALUE, "R not awaited");

    return e;
}


int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    // configure global buffer
    DSTR_WRAP_ARRAY(total_read, total_read_buf);

    PROP_GO(&e, test_rstream(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
