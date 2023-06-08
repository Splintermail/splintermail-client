#include "libdstr/libdstr.h"
#include "libduv/libduv.h"
#include "libhttp/libhttp.h"

#include "test/test_utils.h"

static derr_t E = {0};
static bool finished = false;
static rstream_i *base;
static bool force_no_detach = false;

static bool try_detach(limit_rstream_t *l){
    (void)l;
    if(force_no_detach){
        base->cancel(base);
        return false;
    }
    return !base->eof && !base->awaited;
}

static void reader_cb(stream_reader_t *reader, derr_t e){
    (void)reader;
    TRACE_PROP_VAR(&E, &e);
    finished = true;
}

static derr_t do_test(
    const dstr_t input,
    const dstr_t exp,
    size_t limit,
    size_t readinmax,
    size_t readoutmax
){
    derr_t e = E_OK;

    size_t old_read_in_max_size = _dstr_rstream_read_max_size;
    size_t old_read_out_max_size = _stream_reader_read_max_size;
    _dstr_rstream_read_max_size = readinmax;
    _stream_reader_read_max_size = readoutmax;

    manual_scheduler_t scheduler;
    scheduler_i *sched = manual_scheduler(&scheduler);

    dstr_rstream_t dstr_r;
    limit_rstream_t limit_r;
    stream_reader_t reader;

    DSTR_VAR(got, 256);

    base = dstr_rstream(&dstr_r, sched, input);
    rstream_i *rstream = limit_rstream(
        &limit_r, sched, base, limit, try_detach
    );
    stream_read_all(&reader, rstream, &got, reader_cb);

    finished = false;

    // run to completion
    manual_scheduler_run(&scheduler);

    PROP_VAR_GO(&e, &E, done);
    if(!finished) ORIG_GO(&e, E_VALUE, "reader did not finish", done);

    EXPECT_D3_GO(&e, "got", got, exp, done);

    // expect to detach IFF the limit can be fully read
    EXPECT_B_GO(&e, "detached", limit_r.detached, limit <= input.len, done);

done:
    _dstr_rstream_read_max_size = old_read_in_max_size;
    _stream_reader_read_max_size = old_read_out_max_size;

    return e;
}

static derr_t test_limit(void){
    derr_t e = E_OK;

    DSTR_STATIC(input, "abcdefghijklmnopqrstuvwxyz");

    // one test with limit == 0
    PROP(&e, do_test(input, DSTR_LIT(""), 0, SIZE_MAX, SIZE_MAX) );

    for(size_t limit = 1; limit < input.len+1; limit++){
        for(size_t readinmax = 1; readinmax < limit+1; readinmax++){
            for(size_t readoutmax = 1; readoutmax < limit+1; readoutmax++){
                dstr_t exp = dstr_sub2(input, 0, limit);
                IF_PROP(&e,
                    do_test(input, exp, limit, readinmax, readoutmax)
                ){
                    TRACE(&e,
                        "limit=%x, readinmax=%x, readoutmax=%x\n",
                        FU(limit),
                        FU(readinmax),
                        FU(readoutmax)
                    );
                    return e;
                }
            }
        }
    }

    // one test with limit > input.len
    derr_t e2 = do_test(input, input, input.len + 1, SIZE_MAX, SIZE_MAX);
    EXPECT_E_VAR(&e, "e2", &e2, E_RESPONSE);

    // one test with base canceled (try_detach() returns false)
    force_no_detach = true;
    e2 = do_test(input, input, input.len, SIZE_MAX, SIZE_MAX);
    force_no_detach = false;
    EXPECT_E_VAR(&e, "e2", &e2, E_INTERNAL);

    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_limit(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
