#include "libhttp/libhttp.h"

derr_t http_sync_init(http_sync_t *sync, SSL_CTX *ctx){
    derr_t e = E_OK;

    *sync = (http_sync_t){0};

    PROP(&e, duv_loop_init(&sync->loop) );

    PROP_GO(&e, duv_scheduler_init(&sync->scheduler, &sync->loop), fail_loop);

    PROP_GO(&e,
        duv_http_init(&sync->http, &sync->loop, &sync->scheduler, ctx),
    fail_sched);

    sync->initialized = true;

    return e;

fail_sched:
    duv_scheduler_close(&sync->scheduler);
fail_loop:
    uv_loop_close(&sync->loop);
    DROP_CMD( duv_run(&sync->loop) );

    return e;
}

void http_sync_free(http_sync_t *sync){
    if(!sync->initialized) return;
    sync->initialized = false;

    duv_http_close(&sync->http, NULL);
    DROP_CMD( duv_run(&sync->loop) );

    duv_scheduler_close(&sync->scheduler);
    uv_loop_close(&sync->loop);
    DROP_CMD( duv_run(&sync->loop) );
}

// memory for a single synchronous request
typedef struct {
    hdr_selector_t *selectors;
    duv_http_req_t req;
    stream_reader_t reader;
    derr_t e;
    bool done;
} sync_req_t;

DEF_CONTAINER_OF(sync_req_t, reader, stream_reader_t)

static void sync_req_reader_cb(stream_reader_t *reader, derr_t err){
    sync_req_t *sreq = CONTAINER_OF(reader, sync_req_t, reader);
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&sreq->e, &err);
    sreq->done = true;
}

DEF_CONTAINER_OF(sync_req_t, req, duv_http_req_t)

static void sync_req_hdr_cb(duv_http_req_t *req, const http_pair_t hdr){
    sync_req_t *sreq = CONTAINER_OF(req, sync_req_t, req);

    // have we failed previously?
    if(is_error(sreq->e)) return;

    for(hdr_selector_t *sel = sreq->selectors; sel; sel = sel->next){
        if(sel->found) continue;
        if(!dstr_eq(sel->key, hdr.key)) continue;
        PROP_GO(&sreq->e, dstr_copy(&hdr.value, sel->value), fail);
        sel->found = true;
        break;
    }

    return;

fail:
    // cancel the rest of the request
    stream_reader_cancel(&sreq->reader);
}

derr_t http_sync_req(
    http_sync_t *sync,
    http_method_e method,
    url_t url,
    http_pairs_t *params,
    http_pairs_t *hdrs,
    const dstr_t body,
    // headers you want to receive
    hdr_selector_t *selectors,
    int *status,
    dstr_t *reason, // limited to 256 characters
    dstr_t *resp
){
    derr_t e = E_OK;

    sync_req_t sreq = { selectors };

    rstream_i *rstream = duv_http_req(
        &sreq.req,
        &sync->http,
        method,
        url,
        params,
        hdrs,
        body,
        sync_req_hdr_cb
    );

    stream_read_all(&sreq.reader, rstream, resp, sync_req_reader_cb);

    // drain events until we see our reader cb
    while(!sreq.done){
        /* use UV_RUN_ONCE because when we finish a request, there's likely a
           timer open for the socket we haven't closed yet */
        IF_PROP(&e, duv_step(&sync->loop) ){
            // we don't really have recourse if libuv fails to run.
            LOG_FATAL("uv_run() failed: %x\n", FD(e.msg));
        }
    }

    // detect in-loop failures
    PROP_VAR(&e, &sreq.e);

    // copy outputs
    PROP(&e, dstr_copy(&sreq.req.reason, reason) );
    *status = sreq.req.status;

    return e;
}
