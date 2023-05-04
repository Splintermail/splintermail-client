#include <openssl/ssl.h>

#include "libhttp/libhttp.h"

#define IDLE_MS 3000

static void http_advance_state(duv_http_t *h);
static void req_advance_state(duv_http_req_t *req);
static void req_cancel(duv_http_req_t *req);
static void req_start(duv_http_req_t *req, http_mem_t *mem);

static void duv_http_free_allocations(duv_http_t *h){
    dstr_free(&h->mem.write_buf);
    dstr_free(&h->mem.read_buf);
    if(h->own_ssl_ctx && h->mem.ssl_ctx) SSL_CTX_free(h->mem.ssl_ctx);
}

static void http_schedule_cb(schedulable_t *schedulable){
    duv_http_t *h = CONTAINER_OF(schedulable, duv_http_t, schedulable);
    http_advance_state(h);
}

static void http_schedule(duv_http_t *h){
    h->scheduler->iface.schedule(&h->scheduler->iface, &h->schedulable);
}

static void timer_cb(uv_timer_t *timer){
    duv_http_t *h = timer->data;
    // just cancel the stream, no advance_state is necessary
    if(h->mem.stream) h->mem.stream->cancel(h->mem.stream);
}

static void timer_close_cb(uv_handle_t *handle){
    duv_http_t *h = handle->data;
    h->timer_close_complete = true;
    http_advance_state(h);
}

static void http_advance_state(duv_http_t *h){
    link_t *link;

    if(h->closed) return;

    if(h->close_cb) goto closing;

    // wait for the current request to finish
    if(h->req) return;

    // wait for a pending request
    if(!(link = link_list_pop_first(&h->pending))) return;
    duv_http_req_t *req = CONTAINER_OF(link, duv_http_req_t, link);

    // start the request
    h->req = req;
    req_start(req, &h->mem);
    // cancel any idle timer
    duv_timer_must_stop(&h->timer);

    return;

closing:
    // wait for our requests to be canceled
    if(h->req || !link_list_isempty(&h->pending)) return;

    // the connector must be inactive already
    if(h->mem.connector.active){
        LOG_FATAL("a req finished with connector.active == true\n");
    }

    // close any active connection
    if(h->mem.stream){
        h->mem.stream->cancel(h->mem.stream);
        return;
    }

    if(!h->timer_closed){
        h->timer_closed = true;
        duv_timer_must_stop(&h->timer);
        uv_close(duv_timer_handle(&h->timer), timer_close_cb);
        return;
    }
    if(!h->timer_close_complete) return;

    // done!
    duv_http_free_allocations(h);
    schedulable_cancel(&h->schedulable);
    h->closed = true;
    h->close_cb(h);
}

void duv_http_close(duv_http_t *h, duv_http_close_cb close_cb){
    if(h->close_cb) return;
    h->close_cb = close_cb;

    if(h->req) req_cancel(h->req);
    duv_http_req_t *req;
    LINK_FOR_EACH(req, &h->pending, duv_http_req_t, link){
        req_cancel(req);
    }
    duv_timer_must_stop(&h->timer);

    http_schedule(h);
}

derr_t duv_http_init(
    duv_http_t *h,
    uv_loop_t *loop,
    duv_scheduler_t *scheduler,
    // if NULL, it will be created automatically when it is first needed
    SSL_CTX *ssl_ctx
){
    derr_t e = E_OK;

    *h = (duv_http_t){
        // preserve data
        .data = h->data,
        .loop = loop,
        .scheduler = scheduler,
        .mem = {
            .ssl_ctx = ssl_ctx,
        },
        .own_ssl_ctx = !ssl_ctx,
    };

    link_init(&h->pending);
    schedulable_prep(&h->schedulable, http_schedule_cb);
    // http_reader_t is not initialized until a req_start()

    duv_timer_must_init(loop, &h->timer);
    h->timer.data = h;

    // read_buf size is effectively the longest-allowed line length
    DSTR_WRAP_ARRAY(h->mem.host, h->mem._host);
    PROP_GO(&e, dstr_new(&h->mem.read_buf, 8192), fail);
    // but write_buf size does not correspond to any restrictions
    PROP_GO(&e, dstr_new(&h->mem.write_buf, 4096), fail);

    return e;

fail:
    duv_http_free_allocations(h);

    return e;
}

static void http_await_cb(
    stream_i *stream, derr_t e, link_t *reads, link_t *writes
){
    duv_http_t *h = stream->data;

    h->mem.stream = NULL;

    // don't even log E_CANCELED
    DROP_CANCELED_VAR(&e);

    if(is_error(e)){
        LOG_ERROR("http discarding stream error:\n");
        DUMP(e);
        DROP_VAR(&e);
    }

    // http never reads or writes, and req always cleans up after itself
    if(!link_list_isempty(reads)){
        LOG_FATAL("http_await_cb had unfinished reads\n");
    }
    if(!link_list_isempty(writes)){
        LOG_FATAL("http_await_cb had unfinished writes\n");
    }

    http_schedule(h);
}

static void http_completed_req(duv_http_t *h, duv_http_req_t *req){
    if(h->req == req){
        // this was the active req
        h->req = NULL;
        if(h->mem.stream){
            // we are now responsible for the stream
            h->mem.stream->data = h;
            h->mem.stream->await(h->mem.stream, http_await_cb);
            // start idle timer
            duv_timer_must_start(&h->timer, timer_cb, IDLE_MS);
        }
    }else{
        // this req was pending
        link_remove(&req->link);
    }
    http_schedule(h);
}

////// request //////

static void req_schedule_cb(schedulable_t *schedulable){
    duv_http_req_t *req =
        CONTAINER_OF(schedulable, duv_http_req_t, schedulable);
    req_advance_state(req);
}

static void req_schedule(duv_http_req_t *req){
    req->scheduler->schedule(req->scheduler, &req->schedulable);
}

static bool req_failing(duv_http_req_t *req){
    return is_error(req->e) || req->iface.canceled;
}

static void req_stream_await_cb(
    stream_i *stream, derr_t e, link_t *reads, link_t *writes
){
    duv_http_req_t *req = stream->data;
    http_mem_t *m = req->mem;

    // we only read and write from static memory
    (void)reads;
    (void)writes;
    req->writing = false;

    m->stream = NULL;

    // only we cancel
    DROP_CANCELED_VAR(&e);

    if(req->base){
        /* decide errors based on base, ignoring underlying write errors.
           If the server writes a valid response but rejects our body
           writes, we want to return the valid response.  If the read-side
           of the wire broke, that should show up in base. */
        DROP_VAR(&e);
    }else{
        KEEP_FIRST_IF_NOT_CANCELED_VAR(&req->e, &e);
    }

    req_advance_state(req);
}

static void req_start(duv_http_req_t *req, http_mem_t *m){
    /* there's a remote possibility we are canceled already and scheduled to
       handle it, in which case we don't touch m or m->stream */
    if(req_failing(req)) goto done;

    req->mem = m;
    // re-initialize reader state
    http_reader_free(&m->reader);
    http_reader_init(&m->reader, &m->read_buf);
    m->read_buf.len = 0;

    if(m->stream){
        // we are responsible for this stream now
        m->stream->data = req;
        m->stream->await(m->stream, req_stream_await_cb);
    }

    // detect connections to the wrong host, or which are already failing
    // (don't take any action though, until req_advance_state)
    req->need_conn_cleanup = false
        || (m->connector.active && m->connector.canceling)
        || (m->stream && m->stream->canceled)
        || m->tls != req->tls
        || !dstr_eq(m->host, req->host)
        || m->port != req->port
    ;

done:
    req_advance_state(req);
}


static void req_base_await_cb(rstream_i *rstream, derr_t e, link_t *reads){
    duv_http_req_t *req = rstream->data;

    // only we cancel the base
    DROP_CANCELED_VAR(&e);
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&req->e, &e);

    // unfinished reads go back to pending reads
    link_list_prepend_list(&req->reads, reads);
    req->reading = false;

    req_advance_state(req);
}

static void connect_cb(duv_connect_t *connector, derr_t e){
    duv_http_req_t *req = connector->data;
    http_mem_t *m = req->mem;

    if(is_error(e)){
        // tcp is not connected

        // only we cancel it, so E_CANCELED is no issue
        DROP_CANCELED_VAR(&e);

        KEEP_FIRST_IF_NOT_CANCELED_VAR(&req->e, &e);
        goto done;
    }

    // wrap the tcp in a stream
    m->stream = duv_passthru_init_tcp(
        &m->passthru, req->http->scheduler, &m->tcp
    );
    // decect when we don't want tls or when there's no point
    if(!req->tls || req_failing(req)) goto done;
    // do we need to create a default ssl_ctx?
    if(!m->ssl_ctx){
        ssl_context_t ssl_ctx;
        PROP_GO(&req->e, ssl_context_new_client(&ssl_ctx), done);
        m->ssl_ctx = ssl_ctx.ctx;
    }
    stream_i *tls_stream;
    PROP_GO(&req->e,
        duv_tls_wrap_client(
            &m->duv_tls,
            m->ssl_ctx,
            m->host,
            req->scheduler,
            m->stream,
            &tls_stream
        ),
    done);
    m->stream = tls_stream;

done:
    if(m->stream){
        // if a stream was configured, we start with ownership
        m->stream->data = req;
        m->stream->await(m->stream, req_stream_await_cb);
    }
    req_advance_state(req);
}

// a read off the wire, which should contain header content
static void req_read_cb_hdrs(
    stream_i *stream, stream_read_t *read, dstr_t buf
){
    (void)read;
    duv_http_req_t *req = stream->data;
    http_mem_t *m = req->mem;

    req->reading = false;
    m->read_buf.len += buf.len;

    if(buf.len == 0){
        // eof-before-eoh is definitely not allowed
        ORIG_GO(&req->e, E_RESPONSE, "eof before end of headers", done);
    }

done:
    req_advance_state(req);
}

// a read off our configured rstream, which should contain body content
static void req_read_cb_body(
    rstream_i *rstream, rstream_read_t *read, dstr_t buf
){
    duv_http_req_t *req = rstream->data;

    req->reading = false;

    if(buf.len == 0){
        req->iface.eof = true;
    }

    req->original_read_cb(&req->iface, read, buf);
    // no point in checking if we were canceled since we go to advance_state

    req_advance_state(req);
}

static void req_hdr_cb(
    duv_http_req_t *req, const http_pair_t hdr, bool trailer
){
    // ignore headers after we are failing
    if(req_failing(req)) return;

    if(trailer){
        /* RFC7231: A sender MUST NOT generate a trailer that contains a field
           necessary for message framing (e.g., Transfer-Encoding and
           Content-Length), routing (e.g., Host), request modifiers (e.g.,
           controls and conditionals in Section 5 of [RFC7231]), authentication
           (e.g., see [RFC7235] and [RFC6265]), response control data (e.g.,
           see Section 7.1 of [RFC7231]), or determining how to process the
           payload (e.g., Content-Encoding, Content-Type, Content-Range, and
           Trailer). */
        #define REJECT(name) do { \
            if(dstr_ieq(hdr.key, DSTR_LIT(name))){ \
                ORIG_GO(&req->e, E_RESPONSE, name " in trailer", done); \
            } \
        } while(0)
        REJECT("Transfer-Encoding");
        REJECT("Content-Length");
        REJECT("Host");

        // controls and conditionals in section 5 of RFC7231
        REJECT("Cache-Control");
        REJECT("Expect");
        REJECT("Max-Forwards");
        REJECT("Pragma");
        REJECT("Range");
        REJECT("TE");
        REJECT("If-Match");
        REJECT("If-None-Match");
        REJECT("If-Modified-Since");
        REJECT("If-Unmodified-Since");
        REJECT("If-Range");
        // ... and some client-specific fields that don't matter to us

        // authentication
        // ... all client-side stuff, doesn't matter to us

        // response control data in section 7.1 of RFC7231
        REJECT("Age");
        // REJECT("Cache-Control"); // repeated from above
        REJECT("Expires");
        REJECT("Date");
        REJECT("Location");
        REJECT("Retry-After");
        REJECT("Vary");
        REJECT("Warning");

        // how to process the payload
        REJECT("Content-Encoding");
        REJECT("Content-Type");
        REJECT("Content-Range");
        REJECT("Trailer");
        #undef REJECT
    }

    // Content-Length
    if(dstr_ieq(hdr.key, DSTR_LIT("content-length"))){
        // parse field
        size_t content_length;
        dstr_t stripped = dstr_strip_chars(hdr.value, ' ', '\t');
        PROP_GO(&req->e, dstr_tosize(&stripped, &content_length, 10), done);
        // validate
        switch(req->length_type){
            case HTTP_LEN_UNKNOWN:
                req->length_type = HTTP_LEN_CONTENT_LENGTH;
                req->content_length = content_length;
                break;
            case HTTP_LEN_CONTENT_LENGTH:
                if(req->content_length != content_length){
                    ORIG_GO(&req->e,
                        E_RESPONSE, "duplicate content-length fields",
                    done);
                }
                break;
            case HTTP_LEN_CHUNKED:
                ORIG_GO(&req->e,
                    E_RESPONSE, "chunked encoding and content-length present",
                done);
                break;
        }
    }
    // Transfer-Encoding
    else if(dstr_ieq(hdr.key, DSTR_LIT("transfer-encoding"))){
        // we only support a pure "chunked" encoding
        dstr_t stripped = dstr_strip_chars(hdr.value, ' ', '\t');
        if(!dstr_eq(stripped, DSTR_LIT("chunked"))){
            ORIG_GO(&req->e,
                E_RESPONSE,
                "unsupported Transfer-Encoding: %x",
                done,
                FD_DBG(&hdr.value)
            );
        }
        // validate
        switch(req->length_type){
            case HTTP_LEN_UNKNOWN:
                req->length_type = HTTP_LEN_CHUNKED;
                break;
            case HTTP_LEN_CONTENT_LENGTH:
                ORIG_GO(&req->e,
                    E_RESPONSE, "content-length and chunked encoding present",
                done);
                break;
            case HTTP_LEN_CHUNKED:
                // duplicate but identical fields
                break;
        }
    }
    // Connection
    else if(dstr_ieq(hdr.key, DSTR_LIT("connection"))){
        // connections in HTTP/1.1 are persistent by default
        dstr_t stripped = dstr_strip_chars(hdr.value, ' ', '\t');
        if(dstr_icount2(stripped, DSTR_LIT("close")) > 0){
            req->expect_close = true;
        }
    }

    // pass the hdr to the user too
    req->hdr_cb(req, hdr);

done:
    return;
}

// hdr_cb from within chunked rstream
static void chunked_hdr_cb(chunked_rstream_t *c, const http_pair_t hdr){
    duv_http_req_t *req = c->iface.data;
    req_hdr_cb(req, hdr, true);
}

static bool try_detach(duv_http_req_t *req){
    http_mem_t *m = req->mem;
    if(!m->stream || m->stream->awaited) return false;

    // take back ownership of m->stream
    m->stream->data = req;
    m->stream->await(m->stream, req_stream_await_cb);

    // if it's eof, that's broken but this request is done with it anyway
    if(m->stream->eof) m->stream->cancel(m->stream);

    return true;
}

static bool limit_try_detach(limit_rstream_t *l){
    duv_http_req_t *req = l->iface.data;
    return try_detach(req);
}

static bool chunked_try_detach(chunked_rstream_t *c){
    duv_http_req_t *req = c->iface.data;
    return try_detach(req);
}

static void write_cb(stream_i *stream, stream_write_t *write){
    (void)write;
    duv_http_req_t *req = stream->data;
    req->writing = false;
    req_advance_state(req);
}

static void req_advance_write(duv_http_req_t *req, bool *completed){
    if(req->writing) return;

    http_mem_t *m = req->mem;

    m->write_buf.len = 0;
    *completed = http_marshal_req(&req->marshaler, &m->write_buf);

    const dstr_t bufs[2] = {m->write_buf, req->body};

    // if we completed marshaling headers, send the body too, if there is one
    unsigned int nbufs = (*completed && req->body.len) ? 2 : 1;
    if(req->http->log_requests){
        for(unsigned int i = 0; i < nbufs; i++){
            LOG_INFO("\x1b[32m%x\x1b[m", FD(&bufs[i]));
            fflush(stdout);
        }
    }

    stream_must_write(m->stream, &m->write, bufs, nbufs, write_cb);
    req->writing = true;

    return;
}

static derr_t req_grab_status(duv_http_req_t *req){
    derr_t e = E_OK;

    http_mem_t *m = req->mem;

    req->status = m->reader.status;

    PROP(&e, dstr_copy(&m->reader.reason, &req->reason) );

    return e;
}

// automatically read from the stream until we have read all of the headers
static void req_advance_read_hdrs(duv_http_req_t *req, bool *completed){
    // wait for a buffer to read from
    if(req->reading) return;

    http_mem_t *m = req->mem;

    // read all the headers
    while(true){
        http_pair_t hdr;
        int state;
        dstr_t space;
        PROP_GO(&req->e, http_read(&m->reader, &hdr, &state), done);
        switch(state){
            case -2:
                // incomplete read (read_buf may have been leftshifted)
                space = dstr_empty_space(m->read_buf);
                if(space.size == 0){
                    ORIG_GO(&req->e, E_FIXEDSIZE, "header too long", done);
                }
                req->reading = true;
                stream_must_read(m->stream, &m->read, space, req_read_cb_hdrs);
                return;

            case -1:
                // header found
                if(req->status == 0){
                    PROP_GO(&req->e, req_grab_status(req), done);
                }
                req_hdr_cb(req, hdr, false);
                if(req_failing(req)) return;
                break;

            default:
                if(state < 0){
                    ORIG_GO(&req->e,
                        E_INTERNAL,
                        "http_read returned: %x",
                        done,
                        FI(state)
                    );
                }
                // no more headers
                if(req->status == 0){
                    // (just in case there were no headers at all)
                    PROP_GO(&req->e, req_grab_status(req), done);
                }
                m->initial_body_offset = (size_t)state;
                *completed = true;
                return;
        }
    }

done:
    return;
}

static void req_process_length(duv_http_req_t *req){
    int status = req->status;

    // RFC7230: section 3.3.3, "Message Body Length"

    /* 1.  Any response to a HEAD request and any response with a 1xx
           (Informational), 204 (No Content), or 304 (Not Modified) status
           code is always terminated by the first empty line after the
           header fields, regardless of the header fields present in the
           message, and thus cannot contain a message body. */

    if(req->method == HTTP_METHOD_HEAD
        || (status >= 100 && status <= 199)
        || status == 204
        || status == 304
    ){
        // treat it like Content-Length: 0
        req->length_type = HTTP_LEN_CONTENT_LENGTH;
        req->content_length = 0;
        return;
    };

    // remainder of RFC7230 section 3.3.3 does not apply or is already handled.
}

static rstream_i *req_configure_base(duv_http_req_t *req){
    /*
                                     (close-deliniated)
       initial_body ----+           +------------------+
                        |           |                  |
       wire --> borrow -+-> concat -+-----> limit -----+--> req --> user
                                    |                  |
                                    +----> chunked ----+
    */

    http_mem_t *m = req->mem;

    dstr_t sub = dstr_sub2(m->read_buf, m->initial_body_offset, SIZE_MAX);
    rstream_i *initial_body = dstr_rstream(
        &m->initial_body, req->scheduler, sub
    );

    rstream_i *borrow = borrow_rstream(&m->borrow, req->scheduler, m->stream);

    rstream_i *concat = rstream_concat(
        &m->concat, req->scheduler, initial_body, borrow
    );

    // put an appropriate reader in front of it
    switch(req->length_type){
        case HTTP_LEN_UNKNOWN:
            // Read until EOF on wire (close-delineated)
            return concat;
        case HTTP_LEN_CONTENT_LENGTH:
            // read Content-Length bytes
            return limit_rstream(
                &m->limit,
                req->scheduler,
                concat,
                req->content_length,
                limit_try_detach
            );
        case HTTP_LEN_CHUNKED:
            // read Transfer-Encoding: chunked
            return chunked_rstream(
                &m->chunked,
                req->scheduler,
                concat,
                chunked_try_detach,
                chunked_hdr_cb
            );
    }
    LOG_FATAL("unknown length type: %x\n", FU(req->length_type));
}

static void req_advance_state(duv_http_req_t *req){
    http_mem_t *m = req->mem;

    if(req->iface.awaited) return;

    if(req_failing(req)) goto failing;

    // wait to be started
    if(m == NULL) return;

    // handle old stale connections
    if(req->need_conn_cleanup){
        if(m->connector.active){
            duv_connect_cancel(&m->connector);
            // wait for the connection to finish
            return;
        }
        if(m->stream){
            m->stream->cancel(m->stream);
            // wait for stream to finish closing
            return;
        }
        // we no longer have a stale connection
        req->need_conn_cleanup = false;
    }
    if(!req->connect_started){
        // starting now, any connection failures apply to this request
        req->connect_started = true;
        if(!m->stream){
            // start a new connection
            PROP_GO(&req->e, dstr_copy(&req->host, &m->host), failing);
            m->port = req->port;
            DSTR_VAR(portbuf, 32);
            PROP_GO(&req->e, FMT(&portbuf, "%x", FU(m->port)), failing);
            m->connector.data = req;
            PROP_GO(&req->e,
                duv_connect(
                    req->http->loop,
                    &m->tcp,
                    0,
                    &m->connector,
                    connect_cb,
                    m->host,
                    portbuf,
                    NULL
                ),
            failing);
        }
    }
    if(!req->established){
        if(!m->stream) return;
        req->established = true;
    }

    // write the request
    if(!req->written){
        req_advance_write(req, &req->written);
        if(req_failing(req)) goto failing;
        // fallthru
    }

    // even as we write the request, start reading for headers
    if(!req->eoh){
        req_advance_read_hdrs(req, &req->eoh);
        if(req_failing(req)) goto failing;
        if(!req->eoh) return;
        req_process_length(req);
        req->base = req_configure_base(req);
        req->base->data = req;
        req->base->await(req->base, req_base_await_cb);
    }

    // wait for any in-flight read to come back
    if(req->reading) return;

    // process reads for the rstream
    link_t *link;
    while((link = link_list_pop_first(&req->reads))){
        rstream_read_t *read = CONTAINER_OF(link, rstream_read_t, link);
        if(!req->iface.eof){
            // submit another read to the rstream
            req->reading = true;
            req->original_read_cb = read->cb;
            stream_must_read(req->base, read, read->buf, req_read_cb_body);
            break;
        }
        // respond eof to remaining reads
        read->buf.len = 0;
        read->cb(&req->iface, read, read->buf);
        if(req_failing(req)) goto failing;
    }

    // wait till we've sent our eof
    if(!req->iface.eof) return;
    // also explicitly wait for our write to finish
    if(req->writing) return;

    // always await our base
    if(!req->base->awaited) return;

    if(req->expect_close){
        // the base may have awaited or detached the underlying stream
        if(m->stream){
            m->stream->cancel(m->stream);
            return;
        }
    }

    // the duv_http_t is done with us now
    if(!req->completed){
        req->completed = true;
        http_completed_req(req->http, req);
    }

    // wait to be awaited
    if(!req->await_cb) return;

    schedulable_cancel(&req->schedulable);
    link_t reads = {0};
    link_list_append_list(&reads, &req->reads);
    req->iface.awaited = true;
    req->await_cb(&req->iface, E_OK, &reads);
    return;

failing:
    // were we even started?
    if(m){
        /* cleanup the connection after any failure; we don't know what state
           the connection is in */
        if(m->connector.active) duv_connect_cancel(&m->connector);
        if(m->stream) m->stream->cancel(m->stream);
        // wait for everything to be cleaned up
        if(req->reading) return;
        if(req->writing) return;
        if(m->connector.active) return;
        if(m->stream) return;
    }

    // await our base, if we had one
    if(req->base && !req->base->awaited) return;

    // the duv_http_t is done with us now
    if(!req->completed){
        req->completed = true;
        http_completed_req(req->http, req);
    }

    // wait to be awaited
    if(!req->await_cb) return;

    // give the error to the user's await_cb
    if(!is_error(req->e) && req->iface.canceled) req->e.type = E_CANCELED;
    schedulable_cancel(&req->schedulable);
    link_t reads2 = {0};
    link_list_append_list(&reads2, &req->reads);
    req->iface.awaited = true;
    req->await_cb(&req->iface, req->e, &reads2);
}

// rstream interface
static bool req_read(
    rstream_i *iface,
    rstream_read_t *read,
    dstr_t buf,
    rstream_read_cb cb
){
    if(!stream_read_checks(iface, buf)) return false;

    duv_http_req_t *req = CONTAINER_OF(iface, duv_http_req_t, iface);

    rstream_read_prep(read, buf, cb);
    link_list_append(&req->reads, &read->link);

    // only schedule if there's a chance we can actually process this read
    if(req->base) req_schedule(req);

    return true;
}

static void req_cancel(duv_http_req_t *req){
    if(req->iface.awaited || req->iface.canceled) return;
    req->iface.canceled = true;
    req_schedule(req);
}

static void req_rstream_cancel(rstream_i *iface){
    duv_http_req_t *req = CONTAINER_OF(iface, duv_http_req_t, iface);
    req_cancel(req);
}

static rstream_await_cb req_await(rstream_i *iface, rstream_await_cb cb){
    duv_http_req_t *req = CONTAINER_OF(iface, duv_http_req_t, iface);

    if(req->iface.awaited) return NULL;

    rstream_await_cb out = req->await_cb;
    req->await_cb = cb;
    req_schedule(req);
    return out;
}

rstream_i *duv_http_req(
    duv_http_req_t *req,
    duv_http_t *http,
    http_method_e method,
    // memory for url/params/hdrs/body must be valid until rstream is awaited
    url_t url,
    http_pairs_t *params,
    http_pairs_t *hdrs,
    const dstr_t body,
    // called once per header, note that headers may arrive after the body
    duv_http_hdr_cb hdr_cb
){
    *req = (duv_http_req_t){
        .iface = {
            // preserve data
            .data = req->iface.data,
            .wrapper_data = req->iface.wrapper_data,
            .read = req_read,
            .cancel = req_rstream_cancel,
            .await = req_await,
        },
        .http = http,
        .scheduler = &http->scheduler->iface,
        .method = method,
        .body = body,
        .hdr_cb = hdr_cb,
    };

    link_init(&req->link);
    link_init(&req->reads);
    schedulable_prep(&req->schedulable, req_schedule_cb);

    if(method != HTTP_METHOD_HEAD){
        // add a `TE` header to advertise support for chunked trailers
        req->hdr_te = HTTP_PAIR("TE", "trailers");

        /* add a `Connection: TE` header since TE only applies to the current
           hop, a MUST from RFC2730 section 4.3 */
        req->hdr_connection = HTTP_PAIR("Connection", "TE");

        hdrs = HTTP_PAIR_CHAIN(hdrs, &req->hdr_connection, &req->hdr_te);
    }

    req->marshaler = http_marshaler(
        method,
        url,
        params,
        hdrs,
        body.len
    );

    // now we are complete enough to fail

    if(http->canceled){
        ORIG_GO(&req->e, E_PARAM, "http is already canceled", fail);
    }

    dstr_t scheme = dstr_from_off(url.scheme);
    if(dstr_eq(scheme, DSTR_LIT("http"))){
        req->tls = false;
    }else if(dstr_eq(scheme, DSTR_LIT("https"))){
        req->tls = true;
    }else{
        ORIG_GO(&req->e,
            E_PARAM, "invalid schema: '%x'\n", fail, FD_DBG(&scheme)
        );
    }

    dstr_t host = dstr_from_off(url.host);
    DSTR_WRAP_ARRAY(req->host, req->_host);
    DSTR_WRAP_ARRAY(req->reason, req->_reason);
    PROP_GO(&req->e, dstr_append(&req->host, &host), fail);

    dstr_t port = dstr_from_off(url.port);
    if(port.len == 0){
        req->port = req->tls ? 443 : 80;
    }else{
        PROP_GO(&req->e, dstr_tou(&port, &req->port, 10), fail);
    }

    // attach ourselves to the duv_http_t
    link_list_append(&http->pending, &req->link);
    http_schedule(http);

fail:
    // no need to schedule even in failure cases; await() will shedule
    return &req->iface;
}
