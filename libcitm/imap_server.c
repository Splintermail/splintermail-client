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

static void await_cb(stream_i *stream, derr_t e){
    imap_server_t *s = stream->data;

    if(s->logged_out){
        // if user logged out first, ignore expected connection errors
        DROP_VAR(&e);
        // mark shutdown as true so we continue with the logout codepath
        s->shutdown = true;
    }else if(!s->canceled && !s->failed){
        // if we didn't cancel the base, nobody else is allowed to
        UPGRADE_CANCELED_VAR(&e, E_INTERNAL);
    }else{
        DROP_CANCELED_VAR(&e);
    }
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&s->e, &e);

    if(s->original_base_await_cb){
        s->original_base_await_cb(stream, E_OK);
    }
    schedule(s);
}

static void shutdown_cb(stream_i *stream){
    imap_server_t *s = stream->data;
    s->shutdown = true;
    schedule(s);
}

#define ONCE(x) if(!x && (x = true))

static void read_cb(stream_i *stream, stream_read_t *req, dstr_t buf, bool ok){
    imap_server_t *s = stream->data;
    (void)req;
    if(!ok) return;
    s->read_done = true;
    s->rbuf.len = buf.len;
    if(buf.len == 0 && !s->logged_out && !is_error(s->e)){
        TRACE_ORIG(&s->e, E_RESPONSE, "unexpected EOF from imap client");
    }
    schedule(s);
}

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
            // leave unread data in read_buf for preinput to the duv_tls_t
            dstr_leftshift(&s->rbuf, skip);
        }else{
            PROP(&e, imap_cmd_read(&s->reader, s->rbuf, &s->cmds) );
        }
    }

    *ok = true;
    return e;
}

static void write_cb(stream_i *stream, stream_write_t *req, bool ok){
    imap_server_t *s = stream->data;
    (void)req;
    if(!ok) return;
    s->write_done = true;
    schedule(s);
}

// try to marshal all responses to the wire
static derr_t advance_writes(imap_server_t *s, bool *ok){
    derr_t e = E_OK;
    *ok = false;

    while(!link_list_isempty(&s->resps)){
        // write out one wbuf
        ONCE(s->write_started){
            s->wbuf.len = 0;
            s->nwritten = 0;
            size_t want = 0;
            link_t *link = s->resps.next;
            // cram as many responses as we can fit into this wbuf
            while(link != &s->resps && want == 0){
                imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);
                PROP(&e,
                    imap_resp_write(
                        resp, &s->wbuf, &s->write_skip, &want, &s->exts
                    )
                );
                if(want == 0){
                    // finished marshaling a whole response
                    s->nwritten++;
                    s->write_skip = 0;
                    link = link->next;
                }
            }
            stream_must_write(s->stream, &s->write_req, &s->wbuf, 1, write_cb);
        }
        if(!s->write_done) return e;
        // finished a write to the wire
        s->write_started = false;
        s->write_done = false;
        // pop any completed responses from our list
        for(size_t i = 0; i < s->nwritten; i++){
            link_t *link = link_list_pop_first(&s->resps);
            imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);
            imap_resp_free(resp);
        }
        s->nwritten = 0;
    }

    *ok = true;
    return e;
}

static ie_dstr_t *build_capas(derr_t *e, imap_server_t *s){
    if(is_error(*e)) return NULL;

    ie_dstr_t *out = ie_dstr_new2(e, DSTR_LIT("IMAP4rev1"));
    out = ie_dstr_add(e, out, ie_dstr_new2(e, DSTR_LIT("IDLE")));
    if(s->starttls){
        // build suitable pre-STARTTLS capabilities
        out = ie_dstr_add(e, out, ie_dstr_new2(e, DSTR_LIT("STARTTLS")));
        out = ie_dstr_add(e, out, ie_dstr_new2(e, DSTR_LIT("LOGINDISABLED")));
    }else{
        // build suitable pre-LOGIN capabilities
        out = ie_dstr_add(e, out, ie_dstr_new2(e, DSTR_LIT("AUTH=PLAIN")));
        out = ie_dstr_add(e, out, ie_dstr_new2(e, DSTR_LIT("LOGIN")));
    }

    return out;
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
    ie_dstr_t *capas = build_capas(&e, s);
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

static derr_t respond_st(
    imap_server_t *s,
    ie_status_t st,
    ie_dstr_t **tagp,
    dstr_t msg
){
    derr_t e = E_OK;

    ie_dstr_t *text = ie_dstr_new2(&e, msg);
    ie_dstr_t *tag = STEAL(ie_dstr_t, tagp);
    ie_st_resp_t *st_resp = ie_st_resp_new(&e, tag, st, NULL, text);
    imap_resp_arg_t arg = { .status_type = st_resp };
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);
    queue_resp(&e, s, resp);
    CHECK(&e);

    return e;
}

static derr_t respond_error(
    imap_server_t *s, ie_dstr_t **tagp, ie_dstr_t **errorp
){
    derr_t e = E_OK;

    ie_dstr_t *tag = STEAL(ie_dstr_t, tagp);  // might be NULL
    ie_dstr_t *text = STEAL(ie_dstr_t, errorp);
    ie_st_resp_t *st_resp = ie_st_resp_new(&e, tag, IE_ST_BAD, NULL, text);
    imap_resp_arg_t arg = { .status_type = st_resp };
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);
    queue_resp(&e, s, resp);
    CHECK(&e);

    return e;
}

static derr_t respond_capas(imap_server_t *s, ie_dstr_t **tag){
    derr_t e = E_OK;

    ie_dstr_t *capas = build_capas(&e, s);
    imap_resp_arg_t arg = { .capa = capas };
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_CAPA, arg);
    queue_resp(&e, s, resp);
    CHECK(&e);

    dstr_t text = DSTR_LIT("now you know, and knowing is half the battle");
    PROP(&e, respond_st(s, IE_ST_OK, tag, text) );

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

    #define respond_ok(msg) respond_st(s, IE_ST_OK, tag, DSTR_LIT(msg))
    #define respond_no(msg) respond_st(s, IE_ST_NO, tag, DSTR_LIT(msg))
    #define respond_bad(msg) respond_st(s, IE_ST_BAD, tag, DSTR_LIT(msg))

    switch(cmd->type){
        case IMAP_CMD_ERROR:
            PROP_GO(&e, respond_error(s, tag, &cmd->arg.error), cu);
            break;

        case IMAP_CMD_PLUS_REQ:
            PROP_GO(&e, respond_ok("spit it out"), cu);
            break;

        case IMAP_CMD_CAPA:
            PROP_GO(&e, respond_capas(s, tag), cu);
            break;

        case IMAP_CMD_NOOP:
            PROP_GO(&e, respond_ok("zzz..."), cu);
            break;

        case IMAP_CMD_LOGOUT:
            // free any remaining commands
            while((link = link_list_pop_first(&s->cmds))){
                imap_cmd_free(CONTAINER_OF(link, imap_cmd_t, link));
            }
            PROP_GO(&e, respond_ok("get offa my lawn!"), cu);
            s->logged_out = true;
            break;

        case IMAP_CMD_STARTTLS:
            PROP_GO(&e, respond_ok("it's about time"), cu);
            // advance_state handles the rest
            break;

        case IMAP_CMD_LOGIN:
            PROP_GO(&e,
                respond_no(
                    "did you just leak your password on "
                    "an unencrypted connection?"
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
            PROP_GO(&e, respond_bad("it's too early for that"), cu);
            break;

        case IMAP_CMD_AUTH:
        case IMAP_CMD_XKEYSYNC:
        case IMAP_CMD_XKEYSYNC_DONE:
        case IMAP_CMD_XKEYADD:
            PROP_GO(&e, respond_bad("command not supported"), cu);
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

static void advance_state(imap_server_t *s){
    bool ok;

    if(s->canceled || s->failed) goto cu;

    // LOGOUT codepath
    if(s->logged_out){
        // wait for our response to be written
        PROP_GO(&s->e, advance_writes(s, &ok), fail);
        if(!ok) return;
        // no more writes from us
        s->stream->shutdown(s->stream, shutdown_cb);
        if(!s->shutdown) return;
        // if we actually shutdown (user didn't close), we still need to cancel
        s->stream->cancel(s->stream);
        if(!s->stream->awaited) return;
        goto io_return;
    }

    // send an appropriate greeting
    if(!s->greeting_done){
        ONCE(s->greeting_started){
            PROP_GO(&s->e, send_greeting(s), fail);
        }
        PROP_GO(&s->e, advance_writes(s, &ok), fail);
        if(!ok) return;
        s->greeting_done = true;;
    }

    while(s->starttls && !s->starttls_done){
        // requires STARTTLS command before proceeding further
        while(!s->starttls_started){
            // finish any writes
            PROP_GO(&s->e, advance_writes(s, &ok), fail);
            if(!ok) return;
            // read one or more commands
            PROP_GO(&s->e, advance_reads(s, true, &ok), fail);
            if(!ok) return;
            // generate the response to each command
            link_t *link;
            while((link = link_list_pop_first(&s->cmds))){
                imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
                imap_cmd_type_t cmd_type = cmd->type;
                PROP_GO(&s->e, pre_starttls_respond(s, cmd), fail);
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
        PROP_GO(&s->e, advance_writes(s, &ok), fail);
        if(!ok) return;
        // promote the connection to TLS
        PROP_GO(&s->e, starttls(s), fail);
        s->starttls_done = true;
    }

    // now act as a blind relay

    // process read requests
    while(!link_list_isempty(&s->reads)){
        PROP_GO(&s->e, advance_reads(s, false, &ok), fail);
        if(!ok) break;
        // fulfill one read
        imap_server_read_t *req = CONTAINER_OF(
            link_list_pop_first(&s->reads), imap_server_read_t, link
        );
        imap_cmd_t *cmd = CONTAINER_OF(
            link_list_pop_first(&s->cmds), imap_cmd_t, link
        );
        // finished this read request
        req->cb(s, req, cmd, true);
    }

    // process write requests
    while(!link_list_isempty(&s->writes)){
        // peek at the next write
        imap_server_write_t *req = CONTAINER_OF(
            s->writes.next, imap_server_write_t, link
        );
        if(req->resp){
            imap_resp_t *resp = STEAL(imap_resp_t, &req->resp);
            link_list_append(&s->resps, &resp->link);
        }
        PROP_GO(&s->e, advance_writes(s, &ok), fail);
        if(!ok) break;
        // finished this write request
        link_remove(&req->link);
        req->cb(s, req, true);
    }

    return;

fail:
    s->failed = true;

cu:
    // cancel our base stream
    s->stream->cancel(s->stream);
    if(!s->stream->awaited) return;

    // close the underlying connection object
    s->conn->close(s->conn);

io_return:
    // return any IO requests
    link_t *link;
    while((link = link_list_pop_first(&s->reads))){
        imap_server_read_t *req = CONTAINER_OF(link, imap_server_read_t, link);
        req->cb(s, req, NULL, false);
    }
    while((link = link_list_pop_first(&s->writes))){
        imap_server_write_t *req =
            CONTAINER_OF(link, imap_server_write_t, link);
        req->cb(s, req, false);
    }

    // free cmds or responses
    while((link = link_list_pop_first(&s->cmds))){
        imap_cmd_free(CONTAINER_OF(link, imap_cmd_t, link));
    }
    while((link = link_list_pop_first(&s->resps))){
        imap_resp_free(CONTAINER_OF(link, imap_resp_t, link));
    }

    // wait to be awaited
    if(!s->await_cb) return;

    // cleanup our internals
    schedulable_cancel(&s->schedulable);

    // await_cb must be last (it might free us)
    s->awaited = true;
    s->await_cb(s, s->e);
}

static void free_server_memory(imap_server_t *s){
    if(!s) return;
    imap_cmd_reader_free(&s->reader);
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
    conn->close(conn);
    return e;
}

// must not have been awaited yet (that is, await_cb must not have been called)
imap_server_await_cb imap_server_await(
    imap_server_t *s, imap_server_await_cb cb
){
    if(s->awaited) LOG_FATAL("imap server was already awaited!\n");
    imap_server_await_cb out = s->await_cb;
    s->await_cb = cb;
    schedule(s);
    return out;
}

void imap_server_cancel(imap_server_t *s){
    s->canceled = true;
    schedule(s);
}

static void await_self(imap_server_t *s, derr_t e){
    // swallow any error since we were closed, not awaited
    DROP_VAR(&e);
    free_server_memory(s);
}

// if not awaited, it will stay alive long enough to await itself
void imap_server_free(imap_server_t **sptr){
    imap_server_t *s = *sptr;
    if(!s) return;
    if(!s->awaited){
        imap_server_cancel(s);
        imap_server_await(s, await_self);
    }else{
        free_server_memory(s);
    }
    *sptr = NULL;
}

void imap_server_read(
    imap_server_t *s, imap_server_read_t *req, imap_server_read_cb cb
){
    // XXX: rewrite stream_i to have await_cb return unfinished reads and writes
    //      AND all read_cbs mean success
    //      AND all write_cbs mean success
    //      AND the transition from "you can read and write to this stream_i"
    //          to "you can't read/write anymore" is atomic (for failures)
}
