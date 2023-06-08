#include "libcitm/libcitm.h"

static void advance_state(imap_server_t *s);

static void schedule_cb(schedulable_t *schedulable){
    imap_server_t *s = CONTAINER_OF(schedulable, imap_server_t, schedulable);
    advance_state(s);
}

static void schedule(imap_server_t *s){
    if(s->awaited) return;
    s->scheduler->schedule(s->scheduler, &s->schedulable);
}

static void read_cb(stream_i *stream, stream_read_t *req, dstr_t buf){
    imap_server_t *s = stream->data;
    (void)req;
    s->read_done = true;
    s->rbuf.len = buf.len;
    if(buf.len == 0 && !s->logged_out && !is_error(s->e)){
        TRACE_ORIG(&s->e, E_RESPONSE, "unexpected EOF from imap client");
    }
    schedule(s);
}

static void write_cb(stream_i *stream, stream_write_t *req){
    imap_server_t *s = stream->data;
    (void)req;
    s->write_done = true;
    schedule(s);
}

static void shutdown_cb(stream_i *stream){
    imap_server_t *s = stream->data;
    s->shutdown = true;
    schedule(s);
}

static void await_cb(
    stream_i *stream, derr_t e, link_t *reads, link_t *writes
){
    imap_server_t *s = stream->data;

    if(s->logged_out || s->broken_conn){
        // if we said BYE, ignore expected connection errors
        DROP_VAR(&e);
        // mark shutdown as true so we continue with the logout or broken_conn
        s->shutdown = true;
    }else if(!s->broken_conn && !s->canceled && !is_error(s->e)){
        // if we didn't cancel the base, nobody else is allowed to
        UPGRADE_CANCELED_VAR(&e, E_INTERNAL);
    }else{
        DROP_CANCELED_VAR(&e);
    }
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&s->e, &e);

    s->read_done = true;
    s->write_done = true;

    if(s->original_base_await_cb){
        // our reads and writes are static
        link_t ignore;
        stream_reads_filter(reads, &ignore, read_cb);
        stream_writes_filter(writes, &ignore, write_cb);
        s->original_base_await_cb(stream, E_OK, reads, writes);
    }
    schedule(s);
}

#define ONCE(x) if(!x && (x = true))

// try to read at least one command from the wire
// in the starttls case, leave remaining text in read_buf
static derr_t advance_reads(imap_server_t *s, bool starttls, bool *ok){
    derr_t e = E_OK;
    *ok = false;

    while(link_list_isempty(&s->cmds)){
        // read bytes off the wire
        ONCE(s->read_started){
            stream_must_read(s->stream, &s->read_req, s->rbuf, read_cb);
        }
        if(!s->read_done) return e;
        s->read_started = false;
        s->read_done = false;
        if(starttls){
            size_t skip = 0;
            PROP(&e,
                imap_cmd_read_starttls(&s->reader, s->rbuf, &s->cmds, &skip)
            );
            dstr_t sub = dstr_sub2(s->rbuf, 0, skip);
            /* because we only want to print what was valid starttls, we can't
               print until after we've read, unfortunately */
            LOG_DEBUG("%x recv dn: %x", FP(s), FD(sub));
            // leave unread data in read_buf for preinput to the duv_tls_t
            dstr_leftshift(&s->rbuf, skip);
        }else{
            LOG_DEBUG("%x recv dn: %x", FP(s), FD(s->rbuf));
            PROP(&e, imap_cmd_read(&s->reader, s->rbuf, &s->cmds) );
        }
    }

    *ok = true;
    return e;
}

static void do_write(imap_server_t *s){
    LOG_DEBUG("%x send dn: %x", FP(s), FD(s->wbuf));
    stream_must_write(s->stream, &s->write_req, &s->wbuf, 1, write_cb);
}

// try to marshal all responses to the wire
static derr_t advance_writes(imap_server_t *s, bool *ok){
    derr_t e = E_OK;
    *ok = false;

    // have we finished the last write we sent?
    if(s->write_sent){
        if(!s->write_done) return e;
        s->write_started = false;
        s->write_sent = false;
        s->write_done = false;
        s->wbuf.len = 0;
        s->nwritten = 0;
    }

    // is there nothing to write?
    if(!s->write_started && link_list_isempty(&s->resps)){
        *ok = true;
        return e;
    }

    bool want_delay = false;

    // cram as many responses as we can fit into this wbuf
    link_t *link = s->resps.next;
    while(link != &s->resps){
        s->write_started = true;
        size_t want = 0;
        imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);
        PROP(&e,
            imap_resp_write(resp, &s->wbuf, &s->write_skip, &want, &s->exts)
        );
        if(want){
            // buffer is full, send the write
            do_write(s);
            s->write_sent = true;
            return e;
        }
        // finished marshaling a response
        s->write_skip = 0;
        link = link->next;
        link_remove(&resp->link);
        imap_resp_free(resp);
        if(!s->relay_started) continue;
        // respond to write_cb
        imap_server_write_t *req = CONTAINER_OF(
            link_list_pop_first(&s->writes), imap_server_write_t, link
        );
        // the one thing we queue ourselves after relay_started is broken_conn
        if(!req) continue;
        req->cb(s, req);
        // did the user cancel us?
        if(s->canceled || s->broken_conn) return e;
        /* we finished a response with room leftover and we made a write_cb, so
           wait a round to see if our writer has more to send */
        want_delay = true;
    }

    if(want_delay){
        // delay one scheduling round
        link_remove(&s->schedulable.link);
        schedule(s);
    }else{
        // if we didn't make any write_cb's, there's no reason to wait
        do_write(s);
        s->write_sent = true;
    }

    return e;
}

static void queue_resp(derr_t *e, imap_server_t *s, imap_resp_t *resp){
    if(is_error(*e)) goto fail;

    resp = imap_resp_assert_writable(e, resp, &s->exts);
    CHECK_GO(e, fail);

    link_list_append(&s->resps, &resp->link);

    return;

fail:
    imap_resp_free(resp);
}

static derr_t send_greeting(imap_server_t *s){
    derr_t e = E_OK;

    // build code
    ie_dstr_t *capas;
    if(s->starttls){
        capas = build_capas_prestarttls(&e);
    }else{
        capas = build_capas_prelogin(&e);
    }
    ie_st_code_arg_t code_arg = {.capa = capas};
    ie_st_code_t *st_code = ie_st_code_new(&e, IE_ST_CODE_CAPA, code_arg);

    // build text
    ie_dstr_t *text = ie_dstr_new2(&e, DSTR_LIT("greetings, friend!"));

    // build response
    ie_st_resp_t *st_resp = ie_st_resp_new(&e, NULL, IE_ST_OK, st_code, text);
    imap_resp_arg_t arg = {.status_type=st_resp};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);

    queue_resp(&e, s, resp);

    CHECK(&e);

    return e;
}

static derr_t pre_starttls_respond(imap_server_t *s, imap_cmd_t *cmd){
    derr_t e = E_OK;

    // allowed pre-starttls commands:
    //   - ERROR,
    //   - PLUS_REQ,
    //   - NOOP
    //   - CAPABILITY (must include LOGINDISABLED and STARTTLS)
    //   - LOGOUT
    //   - STARTTLS

    link_t *link;
    ie_dstr_t **tag = &cmd->tag;
    link_t *out = &s->resps;

    switch(cmd->type){
        case IMAP_CMD_ERROR:
            PROP_GO(&e, respond_error(tag, &cmd->arg.error, out), cu);
            break;

        case IMAP_CMD_PLUS_REQ:
            PROP_GO(&e, RESP_OK(tag, "spit it out", out), cu);
            break;

        case IMAP_CMD_CAPA:
            PROP_GO(&e, respond_capas(tag, build_capas_prestarttls, out), cu);
            break;

        case IMAP_CMD_NOOP:
            PROP_GO(&e, RESP_OK(tag, "zzz...", out), cu);
            break;

        case IMAP_CMD_LOGOUT:
            // free any remaining commands
            while((link = link_list_pop_first(&s->cmds))){
                imap_cmd_free(CONTAINER_OF(link, imap_cmd_t, link));
            }
            PROP_GO(&e, respond_logout(tag, out), cu);
            s->logged_out = true;
            break;

        case IMAP_CMD_STARTTLS:
            PROP_GO(&e, RESP_OK(tag, "it's about time", out), cu);
            // advance_state handles the rest
            break;

        case IMAP_CMD_LOGIN:
            PROP_GO(&e,
                RESP_NO(
                    tag,
                    "did you just leak your password on "
                    "an unencrypted connection?",
                    out
                ),
            cu);
            break;

        case IMAP_CMD_SELECT:
        case IMAP_CMD_EXAMINE:
        case IMAP_CMD_CREATE:
        case IMAP_CMD_DELETE:
        case IMAP_CMD_RENAME:
        case IMAP_CMD_SUB:
        case IMAP_CMD_UNSUB:
        case IMAP_CMD_LIST:
        case IMAP_CMD_LSUB:
        case IMAP_CMD_STATUS:
        case IMAP_CMD_APPEND:
        case IMAP_CMD_CHECK:
        case IMAP_CMD_CLOSE:
        case IMAP_CMD_EXPUNGE:
        case IMAP_CMD_SEARCH:
        case IMAP_CMD_FETCH:
        case IMAP_CMD_STORE:
        case IMAP_CMD_COPY:
        case IMAP_CMD_ENABLE:
        case IMAP_CMD_UNSELECT:
        case IMAP_CMD_IDLE:
        case IMAP_CMD_IDLE_DONE:
            PROP_GO(&e, respond_too_early(tag, out), cu);
            break;

        case IMAP_CMD_AUTH:
        case IMAP_CMD_XKEYSYNC:
        case IMAP_CMD_XKEYSYNC_DONE:
        case IMAP_CMD_XKEYADD:
            PROP_GO(&e, respond_not_supported(tag, out), cu);
            break;
    }

    (void)s; (void)cmd;

cu:
    imap_cmd_free(cmd);

    return e;
}

static derr_t starttls(imap_server_t *s){
    derr_t e = E_OK;

    /* STARTTLS procedure:
         - verify there are no pending reads
         - verify there are no pending writes
         - verify our current stream is not awaited
         - unawait our current stream, wrap it in TLS with preinput */
    if(!link_list_isempty(&s->resps)){
        LOG_FATAL("have pending write before STARTTLS\n");
    }
    if(s->read_started) LOG_FATAL("have pending read before STARTTLS\n");
    if(s->stream->awaited) LOG_FATAL("stream awaited before STARTTLS\n");

    // un-await our stream
    s->stream->await(s->stream, NULL);

    // configure a tls stream
    stream_i *tls = NULL;
    derr_t e2 = duv_tls_wrap_server(
        &s->tls,
        s->conn->ctx,
        s->scheduler,
        s->stream,
        s->rbuf,  // preinput is already in rbuf
        &tls
    );
    // if we succeeded: upgrade our stream
    if(tls) s->stream = tls;
    // success or failure: reawait our stream
    s->stream->await(s->stream, await_cb);
    PROP_VAR(&e, &e2);

    return e;
}

static derr_t send_broken_conn(imap_server_t *s){
    derr_t e = E_OK;

    // build text
    DSTR_STATIC(msg, "broken connection to upstream server");
    ie_dstr_t *text = ie_dstr_new2(&e, msg);

    // build response
    ie_st_resp_t *st_resp = ie_st_resp_new(&e, NULL, IE_ST_BYE, NULL, text);
    imap_resp_arg_t arg = {.status_type=st_resp};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);

    queue_resp(&e, s, resp);

    CHECK(&e);

    return e;
}

static void advance_state(imap_server_t *s){
    bool ok;

    if(is_error(s->e) || s->canceled) goto cu;

    // LOGOUT codepath
    if(s->logged_out){
        // wait for our response to be written
        PROP_GO(&s->e, advance_writes(s, &ok), cu);
        if(!ok) return;
        // no more writes from us
        s->stream->shutdown(s->stream, shutdown_cb);
        if(!s->shutdown) return;
        // proceed with teardown
        goto cu;
    }

    // send an appropriate greeting
    if(!s->greeting_done){
        ONCE(s->greeting_started){
            PROP_GO(&s->e, send_greeting(s), cu);
        }
        PROP_GO(&s->e, advance_writes(s, &ok), cu);
        if(!ok) return;
        s->greeting_done = true;
    }

    while(s->starttls && !s->starttls_done){
        // requires STARTTLS command before proceeding further
        while(!s->starttls_started){
            // finish any writes
            PROP_GO(&s->e, advance_writes(s, &ok), cu);
            if(!ok) return;
            // read one or more commands
            PROP_GO(&s->e, advance_reads(s, true, &ok), cu);
            if(!ok) return;
            // generate the response to each command
            link_t *link;
            while((link = link_list_pop_first(&s->cmds))){
                imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
                imap_cmd_type_t cmd_type = cmd->type;
                PROP_GO(&s->e, pre_starttls_respond(s, cmd), cu);
                if(cmd_type != IMAP_CMD_STARTTLS) continue;
                /* STARTTLS must be the last cmd we read, since
                   anything extra must be preinput to the tls */
                if(!link_list_isempty(&s->cmds)){
                    LOG_FATAL("STARTTLS was not the final cmd read\n");
                }
                s->starttls_started = true;
            }
        }
        // drain all io to the unencryped stream
        PROP_GO(&s->e, advance_writes(s, &ok), cu);
        if(!ok) return;
        // promote the connection to TLS
        PROP_GO(&s->e, starttls(s), cu);
        s->starttls_done = true;
    }

    /* we always do the greeting and the STARTTLS before handling broken conn
       alerts, since broken_conn is a type of graceful shutdown, and both the
       greeting and the STARTTLS are self-contained between us and the client,
       so it's ok to wait until those are completed to send the alert */
    if(s->broken_conn){
        ONCE(s->broken_conn_sent){
            PROP_GO(&s->e, send_broken_conn(s), cu);
        }
        PROP_GO(&s->e, advance_writes(s, &ok), cu);
        if(!ok) return;
        // no more writes from us
        s->stream->shutdown(s->stream, shutdown_cb);
        if(!s->shutdown) return;
        // proceed with teardown
        goto cu;
    }

    // now act as a blind relay
    ONCE(s->relay_started){
        // resps and writes must be 1:1
        if(!link_list_isempty(&s->resps)){
            LOG_FATAL("resps not empty when imap_server starts relaying\n");
        }
        // queue all requested responses
        imap_server_write_t *req;
        LINK_FOR_EACH(req, &s->writes, imap_server_write_t, link){
            imap_resp_t *resp = STEAL(imap_resp_t, &req->resp);
            link_list_append(&s->resps, &resp->link);
        }
    }

    // process read requests
    while(!link_list_isempty(&s->reads)){
        PROP_GO(&s->e, advance_reads(s, false, &ok), cu);
        if(!ok) break;
        // fulfill one read
        imap_server_read_t *req = CONTAINER_OF(
            link_list_pop_first(&s->reads), imap_server_read_t, link
        );
        imap_cmd_t *cmd = CONTAINER_OF(
            link_list_pop_first(&s->cmds), imap_cmd_t, link
        );
        // finished this read request
        req->cb(s, req, cmd);
        // did the user cancel us?
        if(s->canceled) goto cu;
        // if we were soft-canceled, just wait till we're scheduled again
        if(s->broken_conn) return;
    }

    // process write requests
    PROP_GO(&s->e, advance_writes(s, &ok), cu);
    (void)ok;

    return;

cu:
    // cancel our base stream
    s->stream->cancel(s->stream);
    if(!s->stream->awaited) return;

    // wait to be awaited
    if(!s->await_cb) return;

    // free the underlying connection object
    s->conn->free(s->conn);

    // free cmds or responses
    link_t *link;
    while((link = link_list_pop_first(&s->cmds))){
        imap_cmd_free(CONTAINER_OF(link, imap_cmd_t, link));
    }
    while((link = link_list_pop_first(&s->resps))){
        imap_resp_free(CONTAINER_OF(link, imap_resp_t, link));
    }

    schedulable_cancel(&s->schedulable);

    // await_cb must be last (it might free us)
    s->awaited = true;
    link_t reads = {0};
    link_t writes = {0};
    link_list_append_list(&reads, &s->reads);
    link_list_append_list(&writes, &s->writes);
    // free all response before returning unfinished io
    imap_server_write_t *req;
    LINK_FOR_EACH(req, &writes, imap_server_write_t, link){
        imap_resp_free(STEAL(imap_resp_t, &req->resp));
    }
    derr_t e = s->e;
    if(!is_error(e) && (s->canceled || s->broken_conn)){
        e.type = E_CANCELED;
    }
    s->await_cb(s, e, &reads, &writes);
}

static void free_server_memory(imap_server_t *s){
    if(!s) return;
    link_remove(&s->link);
    imap_cmd_reader_free(&s->reader);
    schedulable_cancel(&s->schedulable);
    free(s);
}

derr_t imap_server_new(
    imap_server_t **out,
    scheduler_i *scheduler,
    citm_conn_t *conn
){
    derr_t e = E_OK;

    *out = NULL;

    imap_server_t *s = DMALLOC_STRUCT_PTR(&e, s);
    CHECK(&e);

    *s = (imap_server_t){
        .scheduler = scheduler,
        .conn = conn,
        .tls = { .iface.data = s },
        .starttls = conn->security == IMAP_SEC_STARTTLS,
        .exts = { .idle = EXT_STATE_ON },
    };

    PROP_GO(&e, imap_cmd_reader_init(&s->reader, &s->exts), fail);

    DSTR_WRAP_ARRAY(s->rbuf, s->rbufmem);
    DSTR_WRAP_ARRAY(s->wbuf, s->wbufmem);

    schedulable_prep(&s->schedulable, schedule_cb);

    if(conn->security == IMAP_SEC_TLS){
        dstr_t empty = {0};
        PROP_GO(&e,
            duv_tls_wrap_server(
                &s->tls,
                conn->ctx,
                scheduler,
                conn->stream,
                empty,
                &s->stream
            ),
        fail);
        // await only the tls stream
        s->stream->await(s->stream, await_cb);
    }else{
        // use the base for reads and writes
        s->stream = s->conn->stream;
        // await the base stream
        s->original_base_await_cb = s->stream->await(
            s->stream, await_cb
        );
    }

    s->stream->data = s;
    *out = s;

    // server starts conversation with a greeting
    schedule(s);

    return e;

fail:
    free_server_memory(s);
    return e;
}

void imap_server_logged_out(imap_server_t *s){
    s->logged_out = true;
    schedule(s);
}

void imap_server_cancel(imap_server_t *s, bool broken_conn){
    if(!s) return;
    if(broken_conn){
        s->broken_conn = true;
    }else{
        s->canceled = true;
    }
    schedule(s);
}

// must be awaited already
void imap_server_free(imap_server_t **sptr){
    imap_server_t *s = *sptr;
    if(!s) return;
    free_server_memory(s);
    *sptr = NULL;
}

// for handling lists of servers/clients, returns ok when list is empty
bool imap_server_list_cancelfree(link_t *list){
    imap_server_t *s, *temp;
    LINK_FOR_EACH_SAFE(s, temp, list, imap_server_t, link){
        imap_server_cancel(s, false);
        if(s->awaited){
            imap_server_free(&s);
        }
    }
    return link_list_isempty(list);
}

#define DETECT_INVALID(code, what) do { \
    if(code){ \
        LOG_ERROR(what " after " #code "\n"); \
        return false; \
    } \
} while(0)

// retruns ok=false if await_cb has been called
// out may be NULL
bool imap_server_await(
    imap_server_t *s, imap_server_await_cb cb, imap_server_await_cb *out
){
    if(out) *out = NULL;
    DETECT_INVALID(s->awaited, "imap_server_await");
    if(out) *out = s->await_cb;
    s->await_cb = cb;
    schedule(s);
    return true;
}

void imap_server_unawait(imap_server_t *s){
    s->await_cb = NULL;
}

bool imap_server_read(
    imap_server_t *s, imap_server_read_t *req, imap_server_read_cb cb
){
    DETECT_INVALID(s->awaited, "imap_server_read");
    DETECT_INVALID(s->canceled, "imap_server_read");
    DETECT_INVALID(s->broken_conn, "imap_server_read");
    DETECT_INVALID(s->logged_out, "imap_server_read");

    *req = (imap_server_read_t){ .cb = cb };
    link_list_append(&s->reads, &req->link);
    schedule(s);
    return true;
}

bool imap_server_write(
    imap_server_t *s,
    imap_server_write_t *req,
    imap_resp_t *resp,
    imap_server_write_cb cb
){
    DETECT_INVALID(s->awaited, "imap_server_write");
    DETECT_INVALID(s->canceled, "imap_server_write");
    DETECT_INVALID(s->broken_conn, "imap_server_write");
    DETECT_INVALID(s->logged_out, "imap_server_write");

    if(!s->relay_started){
        // at first, we queue up reqs with their responses
        *req = (imap_server_write_t){ .resp = resp, .cb = cb };
        link_list_append(&s->writes, &req->link);
    }else{
        // after we start relaying, we queue the responses immediately
        *req = (imap_server_write_t){ .cb = cb };
        link_list_append(&s->writes, &req->link);
        link_list_append(&s->resps, &resp->link);
    }
    schedule(s);
    return true;
}
