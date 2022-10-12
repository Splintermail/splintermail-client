#include <string.h>

#include "libhttp/libhttp.h"

static void advance_state(chunked_rstream_t *c);

static void read_cb(
    rstream_i *base, rstream_read_t *read, dstr_t buf, bool ok
){
    (void)read;
    chunked_rstream_t *c = base->wrapper_data;
    c->reading = false;
    c->buf.len += buf.len;

    if(!ok){
        c->base_failing = true;
    }else if(buf.len == 0){
        /* eof is always an error, since we should never read after we've read
           a complete response */
        ORIG_GO(&c->e, E_RESPONSE, "incomplete chunked response", done);
    }

done:
    advance_state(c);
}

static void await_cb(rstream_i *base, derr_t e){
    chunked_rstream_t *c = base->wrapper_data;
    if(c->base_canceled) DROP_CANCELED_VAR(&e);
    UPGRADE_CANCELED_VAR(&e, E_INTERNAL);
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&c->e, &e);
    c->base_failing = true;
    if(c->original_await_cb) c->original_await_cb(c->base, E_OK);
    advance_state(c);
}

static void schedule_cb(schedulable_t *s){
    chunked_rstream_t *c = CONTAINER_OF(s, chunked_rstream_t, schedulable);
    advance_state(c);
}

static void schedule(chunked_rstream_t *c){
    c->scheduler->schedule(c->scheduler, &c->schedulable);
}

static bool failing(chunked_rstream_t *c){
    return c->iface.canceled || is_error(c->e) || c->base_failing;
}

static bool closing(chunked_rstream_t *c){
    return failing(c) || c->iface.eof;
}

static derr_t parse_chunk_header(chunked_rstream_t *c, bool *ok){
    derr_t e = E_OK;

    *ok = false;

    // track how much we've read locally until we read a complete header
    size_t nread = c->nbufread;
    char *data = c->buf.data;
    if(c->first_chunk_parsed){
        // expect a CRLF from the previous trailer first
        if(c->buf.len < 2 + nread) return e;
        if(data[nread++] != '\r' || data[nread++] != '\n'){
            ORIG(&e, E_RESPONSE, "malformed chunk missing crlf");
        }
    }

    // parse a whole chunk header in one go or give up
    dstr_t buf = dstr_sub2(c->buf, nread, SIZE_MAX);
    http_scanner_t s = http_scanner(&buf);
    HTTP_ONSTACK_PARSER(p, HTTP_CHUNK_MAX_CALLSTACK, HTTP_CHUNK_MAX_SEMSTACK);
    while(true){
        http_scanned_t scanned = http_scanner_next(&s);
        if(scanned.wantmore){
            // don't need a parser to tell us we didn't get a whole header
            goto done;
        }
        size_t chunk_size;
        http_status_e status = http_parse_chunk(
            &p, &buf, &e, scanned.token, scanned.loc, &chunk_size, NULL
        );
        switch(status){
            case HTTP_STATUS_OK: break;

            case HTTP_STATUS_DONE:
                c->first_chunk_parsed = true;
                nread += s.used;
                c->nbufread = nread;
                c->nchunkbytes = chunk_size;
                *ok = true;
                return e;

            case HTTP_STATUS_SYNTAX_ERROR:
                // allow a pre-formatted error
                if(is_error(e)) goto done;
                ORIG_GO(&e, E_RESPONSE, "invalid http response", done);

            case HTTP_STATUS_CALLSTACK_OVERFLOW:
                LOG_FATAL("http parser CALLSTACK_OVERFLOW\n");
                break;

            case HTTP_STATUS_SEMSTACK_OVERFLOW:
                LOG_FATAL("http parser SEMSTACK_OVERFLOW\n");
                break;
        }
    }

done:
    http_parser_reset(&p);
    return e;
}

static derr_t fill_buffer(chunked_rstream_t *c, const char *what){
    derr_t e = E_OK;

    // preserve leftover bytes
    dstr_leftshift(&c->buf, c->nbufread);
    c->nbufread = 0;

    dstr_t space = dstr_empty_space(c->buf);
    if(space.size == 0){
        // all of buf was filled and we don't have a complete header
        ORIG(&e, E_RESPONSE, "%x header line way too long", FS(what));
    }
    c->reading = true;
    stream_must_read(c->base, &c->read, space, read_cb);

    return e;
}

// bytes which we have sitting in our buf which we know belong to a chunk
static dstr_t get_readable(chunked_rstream_t *c){
    // avoid underflow
    if(c->buf.len < c->nbufread || c->nchunkbytes < c->nchunkread){
        LOG_FATAL("underflow\n");
    }
    size_t limit = c->nchunkbytes - c->nchunkread;
    return dstr_sub2(c->buf, c->nbufread, c->nbufread + limit);
}

static void advance_chunks(chunked_rstream_t *c, bool *complete){
    // wait for any in-flight read to return
    if(c->reading) return;

    // wait for user to read
    while(!link_list_isempty(&c->reads)){
        // pass along any bytes that are ready to be consumed
        dstr_t readable;
        while(readable = get_readable(c), readable.len > 0){
            link_t *link = link_list_pop_first(&c->reads);
            rstream_read_t *read = CONTAINER_OF(link, rstream_read_t, link);
            dstr_t sub = dstr_sub2(readable, 0, read->buf.size);
            if(dstr_append_quiet(&read->buf, &sub) != E_NONE){
                LOG_FATAL("dstr_append_quiet failure in chunked_rstream_t\n");
            }
            c->nbufread += sub.len;
            if(c->nbufread >= c->buf.len){
                // finished our read buf
                c->buf.len = 0;
                c->nbufread = 0;
            }
            c->nchunkread += sub.len;
            if(c->nchunkread >= c->nchunkbytes){
                // finished a chunk
                c->nchunkbytes = 0;
                c->nchunkread = 0;
            }
            read->cb(&c->iface, read, read->buf, true);
            // detect if user canceled us
            if(closing(c)) return;
            // if no more reads from the user, we are done here
            if(link_list_isempty(&c->reads)) return;
        }

        if(c->nchunkread == 0 && c->buf.len > c->nbufread){
            // we have unread bytes that could be the start of our chunk header
            bool ok;
            PROP_GO(&c->e, parse_chunk_header(c, &ok), done);
            if(ok){
                // we read a chunk header
                if(c->nchunkbytes == 0){
                    // no more chunks!
                    *complete = true;
                    return;
                }
                // keep passing out chunks to reads
                continue;
            }
        }

        // no bytes in buf, or not enough for a chunk header; need to read
        PROP_GO(&c->e, fill_buffer(c, "chunk"), done);
        return;
    }

done:
    return;
}

static void advance_trailer(chunked_rstream_t *c, bool *complete){
    // wait for any pending read to complete
    if(c->reading) return;

    size_t initial_nbufread = c->nbufread;
    dstr_t buf = dstr_sub2(c->buf, c->nbufread, SIZE_MAX);
    http_scanner_t s = http_scanner(&buf);
    HTTP_ONSTACK_PARSER(
        p, HTTP_HDR_LINE_MAX_CALLSTACK, HTTP_HDR_LINE_MAX_SEMSTACK
    );
    while(true){
        http_scanned_t scanned = http_scanner_next(&s);
        if(scanned.wantmore){
            // don't need a parser to tell us we didn't get a whole header
            goto read_more;
        }
        http_pair_t hdr;
        http_status_e status = http_parse_hdr_line(
            &p, &buf, &c->e, scanned.token, scanned.loc, &hdr, NULL
        );
        switch(status){
            case HTTP_STATUS_OK: break;

            case HTTP_STATUS_DONE:
                // checkpoint read progress
                c->nbufread = initial_nbufread + s.used;
                if(!hdr.key.len){
                    // end of trailer headers
                    if(c->nbufread != c->buf.len){
                        // extra content found after end of trailer headers
                        ORIG_GO(
                            &c->e,
                            E_RESPONSE,
                            "extraneous content after chunked response",
                        done);
                    }
                    *complete = true;
                    return;
                }
                // finished a header line
                c->hdr_cb(c, hdr);
                break;

            case HTTP_STATUS_SYNTAX_ERROR:
                // allow a pre-formatted error
                if(is_error(c->e)) goto done;
                ORIG_GO(&c->e, E_RESPONSE, "invalid http response", done);

            case HTTP_STATUS_CALLSTACK_OVERFLOW:
                LOG_FATAL("http parser CALLSTACK_OVERFLOW\n");
                break;

            case HTTP_STATUS_SEMSTACK_OVERFLOW:
                LOG_FATAL("http parser SEMSTACK_OVERFLOW\n");
                break;
        }
    }

read_more:
    PROP_GO(&c->e, fill_buffer(c, "trailer"), done);

done:
    http_parser_reset(&p);
    return;
}

static void advance_state(chunked_rstream_t *c){
    link_t *link;
    if(closing(c)) goto closing;

    // first pass all chunks to user
    if(!c->chunks_done){
        advance_chunks(c, &c->chunks_done);
        if(closing(c)) goto closing;
        if(!c->chunks_done) return;
    }

    // then read all headers in the trailer, before sending an EOF
    if(!c->trailer_read){
        advance_trailer(c, &c->trailer_read);
        if(closing(c)) goto closing;
        if(!c->trailer_read) return;
    }

    // make sure we'll be able to send EOF right away
    // (these should already be true but this is a good check)
    if(c->reading || link_list_isempty(&c->reads)) return;
    c->iface.eof = true;

    // try to detach from base
    c->detached = c->try_detach(c);
    // fallthru to closing

closing:
    if(c->iface.awaited) return;

    if(!c->base->awaited && !c->base->canceled && failing(c)){
        c->base->cancel(c->base);
        c->base_canceled = true;
    }

    // wait for our read to return
    if(c->reading) return;

    // return any pending reads
    while((link = link_list_pop_first(&c->reads))){
        rstream_read_t *rread = CONTAINER_OF(link, rstream_read_t, link);
        rread->cb(&c->iface, rread, rread->buf, !failing(c));
    }

    // await base, unless we detached
    if(!c->base->awaited && !c->detached) return;

    // wait to be awaited
    if(!c->await_cb) return;

    c->iface.awaited = true;
    schedulable_cancel(&c->schedulable);
    // call user's await_cb
    if(!is_error(c->e)){
        if(c->iface.canceled) c->e.type = E_CANCELED;
    }
    c->await_cb(&c->iface, c->e);
}

// interface

static bool chunked_read(
    rstream_i *iface,
    rstream_read_t *read,
    dstr_t buf,
    rstream_read_cb cb
){
    chunked_rstream_t *c = CONTAINER_OF(iface, chunked_rstream_t, iface);
    if(!stream_read_checks(iface, buf)) return false;
    rstream_read_prep(read, buf, cb);
    link_list_append(&c->reads, &read->link);
    schedule(c);
    return true;
}

static void chunked_cancel(rstream_i *iface){
    chunked_rstream_t *c = CONTAINER_OF(iface, chunked_rstream_t, iface);
    c->iface.canceled = true;
    c->base->cancel(c->base);
    c->base_canceled = true;
}

static rstream_await_cb chunked_await(
    rstream_i *iface, rstream_await_cb await_cb
){
    chunked_rstream_t *c = CONTAINER_OF(iface, chunked_rstream_t, iface);
    if(c->iface.awaited) return NULL;
    rstream_await_cb out = c->await_cb;
    c->await_cb = await_cb;
    schedule(c);
    return out;
}

rstream_i *chunked_rstream(
    chunked_rstream_t *c,
    scheduler_i *scheduler,
    rstream_i *base,
    bool (*try_detach)(chunked_rstream_t*),
    void (*hdr_cb)(chunked_rstream_t*, const http_pair_t)
){
    *c = (chunked_rstream_t){
        .iface = {
            // preserve data
            .data = c->iface.data,
            .wrapper_data = c->iface.wrapper_data,
            .readable = rstream_default_readable,
            .read = chunked_read,
            .cancel = chunked_cancel,
            .await = chunked_await,
        },
        .base = base,
        .try_detach = try_detach,
        .scheduler = scheduler,
        .hdr_cb = hdr_cb,
        .original_await_cb = base->await(base, await_cb),
    };
    link_init(&c->reads);
    schedulable_prep(&c->schedulable, schedule_cb);
    base->wrapper_data = c;
    DSTR_WRAP_ARRAY(c->buf, c->_buf);
    return &c->iface;
}
