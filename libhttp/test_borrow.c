#include "libdstr/libdstr.h"
#include "libduv/libduv.h"
#include "libhttp/libhttp.h"

#include "test/test_utils.h"

derr_t E = E_OK;
bool finished = false;

static void write_cb(stream_i *stream, stream_write_t *write, bool ok){
    (void)stream;
    (void)write;
    (void)ok;
}

static void await_cb(rstream_i *rstream, derr_t e){
    (void)rstream;
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&E, &e);
    finished = true;
}

static void read_cb3(
    rstream_i *rstream, rstream_read_t *read, dstr_t buf, bool ok
){
    (void)read;
    // expect eof
    EXPECT_D_GO(&E, "buf", &buf, &DSTR_LIT(""), fail);
    EXPECT_B_GO(&E, "ok", ok, true, fail);
    return;

fail:
    rstream->cancel(rstream);
}

static void read_cb2(
    rstream_i *rstream, rstream_read_t *read, dstr_t buf, bool ok
){
    (void)read;
    EXPECT_D_GO(&E, "buf", &buf, &DSTR_LIT("world!"), fail);
    EXPECT_B_GO(&E, "ok", ok, true, fail);
    // trigger the final read
    buf.len = 0;
    stream_must_read(rstream, read, buf, read_cb3);
    return;

fail:
    rstream->cancel(rstream);
}

static void read_cb1(
    rstream_i *rstream, rstream_read_t *read, dstr_t buf, bool ok
){
    (void)read;
    EXPECT_D_GO(&E, "buf", &buf, &DSTR_LIT("hello "), fail);
    EXPECT_B_GO(&E, "ok", ok, true, fail);
    return;

fail:
    rstream->cancel(rstream);
}

static derr_t test_borrow(void){
    derr_t e = E_OK;

    manual_scheduler_t scheduler;
    scheduler_i *sched = manual_scheduler(&scheduler);

    dstr_stream_t dstr_s;
    DSTR_STATIC(rbase, "hello world!");
    DSTR_VAR(wbase, 1);

    borrow_rstream_t borrow;

    stream_i *base = dstr_stream(&dstr_s, sched, rbase, &wbase);
    rstream_i *r = borrow_rstream(&borrow, sched, base);
    stream_must_await_first(r, await_cb);

    rstream_read_t read1, read2;
    DSTR_VAR(rbuf1, 6);
    DSTR_VAR(rbuf2, 6);

    // start first two reads
    stream_must_read(r, &read1, rbuf1, read_cb1);
    stream_must_read(r, &read2, rbuf2, read_cb2);

    // run to completion
    manual_scheduler_run(&scheduler);

    PROP_VAR(&e, &E);
    if(!finished) ORIG(&e, E_VALUE, "borrow did not finish");
    // base should be EOF but not canceled or awaited
    EXPECT_B(&e, "base->eof", base->eof, true);
    EXPECT_B(&e, "base->canceled", base->canceled, false);
    EXPECT_B(&e, "base->awaited", base->awaited, false);

    // reset, then cancel the borrow stream
    finished = false;
    base = dstr_stream(&dstr_s, sched, rbase, &wbase);
    r = borrow_rstream(&borrow, sched, base);
    stream_must_await_first(r, await_cb);

    r->cancel(r);

    manual_scheduler_run(&scheduler);

    EXPECT_E_VAR(&e, "E", &E, E_CANCELED);
    if(!finished) ORIG(&e, E_VALUE, "borrow did not finish");
    EXPECT_B(&e, "base->canceled", base->canceled, true);
    EXPECT_B(&e, "base->awaited", base->awaited, true);

    // reset, then cancel the underlying stream
    finished = false;
    base = dstr_stream(&dstr_s, sched, rbase, &wbase);
    r = borrow_rstream(&borrow, sched, base);
    stream_must_await_first(r, await_cb);

    base->cancel(base);

    manual_scheduler_run(&scheduler);

    EXPECT_E_VAR(&e, "E", &E, E_INTERNAL);
    if(!finished) ORIG(&e, E_VALUE, "borrow did not finish");
    EXPECT_B(&e, "base->awaited", base->awaited, true);

    // reset, then cause a failure in the underlying stream
    finished = false;
    base = dstr_stream(&dstr_s, sched, rbase, &wbase);
    r = borrow_rstream(&borrow, sched, base);
    stream_must_await_first(r, await_cb);

    stream_write_t write;
    stream_must_write(base, &write, &DSTR_LIT("abc"), 1, write_cb);

    manual_scheduler_run(&scheduler);

    EXPECT_E_VAR(&e, "E", &E, E_FIXEDSIZE);
    if(!finished) ORIG(&e, E_VALUE, "borrow did not finish");
    EXPECT_B(&e, "base->awaited", base->awaited, true);

    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_borrow(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
