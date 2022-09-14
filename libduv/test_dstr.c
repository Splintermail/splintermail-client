#include "libdstr/libdstr.h"
#include "libduv/libduv.h"

#include "test/test_utils.h"

manual_scheduler_t scheduler;
rstream_i *stream;
dstr_rstream_t rstream;
derr_t E = E_OK;
DSTR_VAR(total_read, 32);

static derr_t do_read_cb(dstr_t buf, bool ok, dstr_t exp){
    derr_t e = E_OK;
    if(!ok){
        printf("read_cb not ok!\n");
        ORIG(&e, E_VALUE, "read_cb not ok!\n");
    }
    if(!dstr_eq(buf, exp)){
        ORIG(&e,
            E_VALUE, "expected \"%x\" but got \"%x\"", FD(&exp), FD(&buf)
        );
    }
    return e;
}

static void read_cb_eof(rstream_i *s, rstream_read_t *read, dstr_t buf, bool ok){
    (void)s;
    (void)read;
    MERGE_CMD(&E, do_read_cb(buf, ok, DSTR_LIT("")), "read_cb");
    s->close(s);
}

static void read_cb1(rstream_i *s, rstream_read_t *read, dstr_t buf, bool ok){
    (void)s;
    (void)read;
    MERGE_CMD(&E, do_read_cb(buf, ok, DSTR_LIT("h")), "read_cb");
}

static void read_cb2(rstream_i *s, rstream_read_t *read, dstr_t buf, bool ok){
    (void)s;
    (void)read;
    MERGE_CMD(&E, do_read_cb(buf, ok, DSTR_LIT("ello world!")), "read_cb");
}

static void read_cb3(rstream_i *s, rstream_read_t *read, dstr_t buf, bool ok){
    (void)s;
    (void)read;
    MERGE_CMD(&E, do_read_cb(buf, ok, DSTR_LIT("")), "read_cb");
    // first of two eof cbs, don't close the stream
}

static void read_cb5(rstream_i *s, rstream_read_t *read, dstr_t buf, bool ok){
    (void)s;
    (void)read;
    MERGE_CMD(&E, do_read_cb(buf, ok, DSTR_LIT("hello world!")), "read_cb");
}

static void read_cb7(rstream_i *s, rstream_read_t *read, dstr_t buf, bool ok){
    (void)read;
    MERGE_CMD(&E, do_read_cb(buf, ok, DSTR_LIT("hello ")), "read_cb");
    s->close(s);
}

static void await_cb(rstream_i *s, derr_t e){
    (void)s;
    MERGE_VAR(&E, &e, "await_cb");
}

static derr_t test_rstream(void){
    derr_t e = E_OK;

    manual_scheduler_prep(&scheduler);

    DSTR_STATIC(base, "hello world!");

    stream = dstr_rstream(
        &rstream,
        &scheduler.iface,
        base,
        await_cb
    );

    // submit a write that will be filled
    rstream_read_t read1;
    DSTR_VAR(buf1, 1);
    stream_must_read(stream, &read1, buf1, read_cb1);

    // submit another read that won't be filled
    rstream_read_t read2;
    DSTR_VAR(buf2, 32);
    stream_must_read(stream, &read2, buf2, read_cb2);

    // submit another read that will be eof'd
    rstream_read_t read3;
    DSTR_VAR(buf3, 1);
    stream_must_read(stream, &read3, buf3, read_cb3);

    // submit one last read that will also be eof'd
    rstream_read_t read4;
    DSTR_VAR(buf4, 1);
    stream_must_read(stream, &read4, buf4, read_cb_eof);

    // run to completion
    manual_scheduler_run(&scheduler);
    MERGE_VAR(&e, &E, "scheduler run");
    if(!stream->awaited) ORIG(&e, E_VALUE, "stream not awaited");

    // start over
    stream = dstr_rstream(
        &rstream,
        &scheduler.iface,
        base,
        await_cb
    );

    // submit a write that will be filled exactly
    rstream_read_t read5;
    DSTR_VAR(buf5, 12);
    stream_must_read(stream, &read5, buf5, read_cb5);

    // submit another read that will be eof'd
    rstream_read_t read6;
    DSTR_VAR(buf6, 1);
    stream_must_read(stream, &read6, buf6, read_cb_eof);

    // run to completion
    manual_scheduler_run(&scheduler);
    MERGE_VAR(&e, &E, "scheduler run");
    if(!stream->awaited) ORIG(&e, E_VALUE, "stream not awaited");

    // start over
    stream = dstr_rstream(
        &rstream,
        &scheduler.iface,
        base,
        await_cb
    );

    // read half of the base, then close it early
    rstream_read_t read7;
    DSTR_VAR(buf7, 6);
    stream_must_read(stream, &read7, buf7, read_cb7);

    // run to completion
    manual_scheduler_run(&scheduler);
    MERGE_VAR(&e, &E, "scheduler run");
    if(!stream->awaited) ORIG(&e, E_VALUE, "stream not awaited");

    return e;
}


int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_rstream(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
