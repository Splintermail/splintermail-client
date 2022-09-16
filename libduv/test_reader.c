#include "libdstr/libdstr.h"
#include "libduv/libduv.h"

#include "test/test_utils.h"

dstr_rstream_t dstr_r;
stream_reader_t reader;
derr_t E = E_OK;
dstr_t exp_;
bool success = false;

static void reader_cb_ok(stream_reader_t *reader, derr_t e){
    dstr_t *buf = reader->data;
    if(is_error(e)){
        IF_PROP_VAR(&E, &e) return;
    }
    if(!dstr_eq(*buf, exp_)){
        TRACE_ORIG(&E, E_VALUE, "expected %x but got %x", FD(&exp_), FD(buf));
    }
    success = true;
}

static void reader_cb_canceled(stream_reader_t *reader, derr_t e){
    (void)reader;
    if(e.type != E_CANCELED){
        if(is_error(e)){
            IF_PROP_VAR(&E, &e) return;
        }
        TRACE_ORIG(&E, E_VALUE, "expected E_CANCELED but got E_OK");
        return;
    }
    success = true;
}

static void reader_cb_failed(stream_reader_t *reader, derr_t e){
    (void)reader;
    if(!is_error(e)){
        TRACE_ORIG(&E, E_VALUE, "expected failure but got E_OK");
        return;
    }
    DROP_VAR(&e);
    success = true;
}

static derr_t test_reader(void){
    derr_t e = E_OK;

    manual_scheduler_t scheduler;
    manual_scheduler_prep(&scheduler);

    DSTR_STATIC(base, "hello world!");

    rstream_i *r = dstr_rstream(&dstr_r, &scheduler.iface, base);

    DSTR_VAR(bigbuf, 64);
    exp_ = base;
    reader.data = &bigbuf;
    stream_read_all(&reader, r, &bigbuf, reader_cb_ok);

    // run to completion
    manual_scheduler_run(&scheduler);
    PROP_VAR(&e, &E);
    if(!success){
        ORIG(&e, E_VALUE, "no error but success was never set");
    }

    // cancel is safe to call after completion
    stream_reader_cancel(&reader);

    // again, but with an allocated buf
    dstr_t heapbuf;
    PROP(&e, dstr_new(&heapbuf, 4) );
    success = false;
    exp_ = base;
    r = dstr_rstream(&dstr_r, &scheduler.iface, base);
    reader.data = &heapbuf;
    stream_read_all(&reader, r, &heapbuf, reader_cb_ok);
    // run to completion
    manual_scheduler_run(&scheduler);
    dstr_free(&heapbuf);
    PROP_VAR(&e, &E);
    if(!success){
        ORIG(&e, E_VALUE, "no error but success was never set");
    }

    // again, but with cancelation
    success = false;
    r = dstr_rstream(&dstr_r, &scheduler.iface, base);
    stream_read_all(&reader, r, &bigbuf, reader_cb_canceled);
    stream_reader_cancel(&reader);
    // run to completion
    manual_scheduler_run(&scheduler);
    PROP_VAR(&e, &E);
    if(!success){
        ORIG(&e, E_VALUE, "no error but success was never set");
    }

    // again, but break the underlying stream
    success = false;
    r = dstr_rstream(&dstr_r, &scheduler.iface, base);
    stream_read_all(&reader, r, &bigbuf, reader_cb_failed);
    r->cancel(r);
    // run to completion
    manual_scheduler_run(&scheduler);
    PROP_VAR(&e, &E);
    if(!success){
        ORIG(&e, E_VALUE, "no error but success was never set");
    }

    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_reader(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
