#include "libdstr/libdstr.h"
#include "libduv/libduv.h"
#include "libhttp/libhttp.h"

#include "test/test_utils.h"

dstr_rstream_t dstr_r;
stream_reader_t reader;
derr_t E = E_OK;
dstr_t exp_;
bool success = false;

typedef struct {
    dstr_t *body;
    rstream_i *r;
    const http_pair_t *exp_hdrs;
    const size_t exp_nhdrs;
    size_t nhdrs;
    derr_t e;
    bool base_awaited;
    bool r_awaited;
    bool r_eof;
} test_reader_t;

static void base_await_cb(rstream_i *base, derr_t e){
    test_reader_t *tr = base->data;
    tr->base_awaited = true;
    if(is_error(e)){
        TRACE_PROP_VAR(&tr->e, &e);
    }
}

static void read_cb(rstream_i *r, rstream_read_t *read, dstr_t buf, bool ok){
    test_reader_t *tr = r->data;

    if(!buf.len){
        if(ok){
            // eof
            tr->r_eof = true;
            // we should have all headers by now
            EXPECT_U_GO(&tr->e, "nhdrs", tr->nhdrs, tr->exp_nhdrs, done);
        }
        return;
    }

    tr->body->len += buf.len;

    dstr_t space = dstr_empty_space(*tr->body);
    if(space.size == 0){
        r->cancel(r);
        ORIG_GO(&tr->e, E_VALUE, "read way too much", done);
    }
    stream_must_read(r, read, space, read_cb);

done:
    return;
}

static void hdr_cb(void *data, const http_pair_t hdr){
    test_reader_t *tr = data;
    if(tr->r_awaited){
        ORIG_GO(&tr->e, E_VALUE, "hdr after await_cb", done);
    }
    if(tr->r_eof){
        ORIG_GO(&tr->e, E_VALUE, "hdr after eof", done);
    }
    if(tr->nhdrs >= tr->exp_nhdrs){
        ORIG_GO(&tr->e, E_VALUE, "hdr after eof", done);
    }
    size_t i = tr->nhdrs++;
    EXPECT_D_GO(&tr->e, "hdr.key", &hdr.key, &tr->exp_hdrs[i].key, done);
    EXPECT_D_GO(&tr->e, "hdr.value", &hdr.value, &tr->exp_hdrs[i].value, done);

done:
    return;
}

static void await_cb(rstream_i *r, derr_t e){
    test_reader_t *tr = r->data;
    tr->r_awaited = true;
    if(is_error(e)){
        PROP_VAR_GO(&tr->e, &e, done);
    }else if(!tr->r_eof){
        ORIG_GO(&tr->e, E_VALUE, "await_cb(E_OK) without eof", done);
    }

done:
    return;
}

#define PAIR(k, v) (http_pair_t){.key=DSTR_LIT(k), .value=DSTR_LIT(v)}

#define do_test(i, b, ...) \
    _do_test( \
        (i), \
        (b), \
        &(http_pair_t[]){PAIR("",""), __VA_ARGS__}[1], \
        sizeof(http_pair_t[]){PAIR("",""), __VA_ARGS__} \
            / sizeof(http_pair_t) - 1 \
    )
static derr_t _do_test(
    const dstr_t input,
    const dstr_t exp_body,
    const http_pair_t *hdrs,
    size_t nhdrs
){
    derr_t e = E_OK;

    manual_scheduler_t scheduler;
    scheduler_i *sched = manual_scheduler(&scheduler);

    dstr_rstream_t dstr_r;
    chunked_rstream_t chunked;

    rstream_i *base = dstr_rstream(&dstr_r, sched, input);
    stream_must_await_first(base, base_await_cb);

    rstream_i *r = chunked_rstream(&chunked, sched, base, hdr_cb);
    stream_must_await_first(r, await_cb);

    DSTR_VAR(body, 4096);

    test_reader_t tr = {
        .body = &body,
        .r = r,
        .exp_hdrs = hdrs,
        .exp_nhdrs = nhdrs,
    };
    base->data = &tr;
    r->data = &tr;

    // kick off the first read
    rstream_read_t read;
    stream_must_read(r, &read, body, read_cb);

    // run to completion
    manual_scheduler_run(&scheduler);

    // checks

    TRACE_PROP_VAR(&e, &tr.e);

    /* even though we read everything, we shouldn't be submitting the extra
       read to the base reader to ever see an eof to trigger an await_cb */
    if(tr.base_awaited){
        TRACE_ORIG(&e, E_VALUE, "base_await_cb was called");
    }

    if(!tr.r_awaited){
        TRACE_ORIG(&e, E_VALUE, "await_cb was not called");
    }

    EXPECT_D(&e, "body", &body, &exp_body);

    return e;
}

static derr_t do_test_chunked(size_t readmax){
    derr_t e = E_OK;

    _dstr_rstream_read_max_size = readmax;

    // without trailer headers
    PROP(&e,
        do_test(
            DSTR_LIT(
                "6\r\n"
                "hello \r\n"
                "6\r\n"
                "world!\r\n"
                "0\r\n"
                "\r\n"
            ),
            DSTR_LIT("hello world!")
        )
    );

    // with trailer headers and chunk extensions
    PROP(&e,
        do_test(
            DSTR_LIT(
                "6;key=value;otherkey=othervalue\r\n"
                "hello \r\n"
                "6\r\n"
                "world!\r\n"
                "0\r\n"
                "key: value\r\n"
                "other-key: other-value\r\n"
                "\r\n"
            ),
            DSTR_LIT("hello world!"),
            PAIR("key", "value"),
            PAIR("other-key", "other-value"),
        )
    );

    return e;
}

static derr_t test_chunked(void){
    derr_t e = E_OK;

    size_t old_read_max_size = _dstr_rstream_read_max_size;

    PROP_GO(&e, do_test_chunked(SIZE_MAX), done);
    PROP_GO(&e, do_test_chunked(1), done);

done:
    _dstr_rstream_read_max_size = old_read_max_size;

    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_chunked(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
