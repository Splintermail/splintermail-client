#include "libcitm/libcitm.h"

static void advance_state(imap_client_t *c);

static void schedule_cb(schedulable_t *schedulable){
    imap_client_t *c = CONTAINER_OF(schedulable, imap_client_t, schedulable);
    advance_state(c);
}

static void schedule(imap_client_t *c){
    if(c->awaited) return;
    c->scheduler->schedule(c->scheduler, &c->schedulable);
}

static void read_cb(stream_i *stream, stream_read_t *req, dstr_t buf){
    imap_client_t *c = stream->data;
    (void)req;
    c->read_done = true;
    c->rbuf.len = buf.len;
    if(buf.len == 0){
        // we always expect the server to tell us to close, then we close
        TRACE_ORIG(&c->e, E_RESPONSE, "unexpected EOF from imap server");
    }
    schedule(c);
}

static void write_cb(stream_i *stream, stream_write_t *req){
    imap_client_t *c = stream->data;
    (void)req;
    c->write_done = true;
    if(c->wbuf_needs_zero){
        // there was something sensitive in our wbuf
        dstr_zeroize(&c->wbuf);
        c->wbuf_needs_zero = false;
    }
    schedule(c);
}

static void await_cb(
    stream_i *stream, derr_t e, link_t *reads, link_t *writes
){
    imap_client_t *c = stream->data;

    if(!c->canceled && !is_error(c->e)){
        // if we didn't cancel the base, nobody else is allowed to
        UPGRADE_CANCELED_VAR(&e, E_INTERNAL);
    }else{
        DROP_CANCELED_VAR(&e);
    }
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&c->e, &e);

    c->read_done = true;
    c->write_done = true;

    if(c->original_base_await_cb){
        // our reads and writes are static
        link_t ignore;
        stream_reads_filter(reads, &ignore, read_cb);
        stream_writes_filter(writes, &ignore, write_cb);
        c->original_base_await_cb(stream, E_OK, reads, writes);
    }
    schedule(c);
}

#define ONCE(x) if(!x && (x = true))

// try to read at least one response from the wire
static derr_t advance_reads(imap_client_t *c, bool *ok){
    derr_t e = E_OK;
    *ok = false;

    while(link_list_isempty(&c->resps)){
        ONCE(c->read_started){
            stream_must_read(c->stream, &c->read_req, c->rbuf, read_cb);
        }
        if(!c->read_done) return e;
        c->read_started = false;
        c->read_done = false;
        LOG_DEBUG("%x recv up %x", FP(c), FD(c->rbuf));
        PROP(&e, imap_resp_read(&c->reader, c->rbuf, &c->resps) );
    }

    *ok = true;
    return e;
}

static void do_write(imap_client_t *c){
    LOG_DEBUG("%x send up %x", FP(c), FD(c->wbuf));
    stream_must_write(c->stream, &c->write_req, &c->wbuf, 1, write_cb);
}

// try to marshal all commands to the wire
static derr_t advance_writes(imap_client_t *c, bool *ok){
    derr_t e = E_OK;
    *ok = false;

    // have we finished the last write we sent?
    if(c->write_sent){
        if(!c->write_done) return e;
        c->write_started = false;
        c->write_sent = false;
        c->write_done = false;
        c->wbuf.len = 0;
        c->nwritten = 0;
    }

    // is there nothing to write?
    if(!c->write_started && link_list_isempty(&c->cmds)){
        *ok = true;
        return e;
    }

    bool want_delay = false;

    // cram as many commands as we can fit into this wbuf
    link_t *link = c->cmds.next;
    while(link != &c->cmds){
        c->write_started = true;
        size_t want = 0;
        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
        PROP(&e,
            imap_cmd_write(cmd, &c->wbuf, &c->write_skip, &want, &c->exts)
        );
        if(want){
            // buffer is full, send the write
            do_write(c);
            c->write_sent = true;
            return e;
        }
        // finished marshaling a cmd
        c->write_skip = 0;
        link = link->next;
        link_remove(&cmd->link);
        imap_cmd_free(cmd);
        if(!c->relay_started) continue;
        // respond to write_cb
        imap_client_write_t *req = CONTAINER_OF(
            link_list_pop_first(&c->writes), imap_client_write_t, link
        );
        req->cb(c, req);
        // did the user cancel us?
        if(c->canceled) return e;
        /* we finished a command with room leftover and we made a write_cb, so
           wait a round to see if our writer has more to send */
        want_delay = true;
    }

    if(want_delay){
        // delay one scheduling round
        link_remove(&c->schedulable.link);
        schedule(c);
    }else{
        // if we didn't make any write_cb's, there's no reason to wait
        do_write(c);
        c->write_sent = true;
    }

    return e;
}


// greetings must be untagged status-type responses
static ie_st_resp_t *match_greeting(const imap_resp_t *resp){
    if(resp->type != IMAP_RESP_STATUS_TYPE) return NULL;
    ie_st_resp_t *st = resp->arg.status_type;
    if(st->status != IE_ST_OK) return false;
    return st;
}

static derr_t process_greeting(imap_client_t *c, bool *ok){
    derr_t e = E_OK;
    *ok = false;

    link_t *link = link_list_pop_first(&c->resps);
    imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);

    ie_st_resp_t *st = match_greeting(resp);
    if(!st){
        ORIG_GO(&e, E_RESPONSE, "unexpected greeting: %x", cu, FIRESP(resp));
    }

    *ok = true;

cu:
    imap_resp_free(resp);
    return e;
}

static derr_t send_starttls(imap_client_t *c){
    derr_t e = E_OK;

    ie_dstr_t *tag = ie_dstr_new2(&e, DSTR_LIT("STLS1"));
    imap_cmd_arg_t arg = {0};
    imap_cmd_t *cmd = imap_cmd_new(&e, tag, IMAP_CMD_STARTTLS, arg);
    cmd = imap_cmd_assert_writable(&e, cmd, &c->exts);
    CHECK(&e);

    link_list_append(&c->cmds, &cmd->link);

    return e;
}

static derr_t process_starttls_resp(imap_client_t *c, bool *ok){
    derr_t e = E_OK;
    *ok = false;
    imap_resp_t *resp = NULL;

    link_t *link;
    while((link = link_list_pop_first(&c->resps))){
        resp = CONTAINER_OF(link, imap_resp_t, link);
        ie_st_resp_t *st;
        if((st = match_tagged(resp, DSTR_LIT("STLS"), 1))){
            // STARTTLS tagged response
            if(st->status == IE_ST_OK){
                // success!
                *ok = true;
                goto cu;
            }
            ORIG_GO(&e, E_RESPONSE, "STARTTLS failed: %x", cu, FIRESP(resp));
        }else if((st = match_info(resp))){
            // informational response
            LOG_INFO("informational response: %x\n", FIRESP(resp));
        }else{
            ORIG_GO(&e,
                E_RESPONSE,
                "unexpected STARTTLS response: %x",
                cu,
                FIRESP(resp)
            );
        }
        imap_resp_free(STEAL(imap_resp_t, &resp));
    }

cu:
    imap_resp_free(resp);
    return e;
}

static derr_t starttls(imap_client_t *c){
    derr_t e = E_OK;

    // there must be no returned reads, since the client starts the handshake
    if(!link_list_isempty(&c->resps)){
        link_t *link = c->resps.next;
        imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);
        ORIG(&e,
            E_RESPONSE,
            "have unencrypted response after STARTTLS: %x",
            FIRESP(resp)
        );
    }

    // we shouldn't be mid-read or the streams will get corrupted
    if(c->read_started){
        ORIG(&e, E_INTERNAL, "imap_client is mid-read during STARTTLS");
    }

    // un-await our current stream
    c->stream->await(c->stream, NULL);

    stream_i *tls;
    derr_t e2 = duv_tls_wrap_client(
        &c->tls,
        c->conn->ctx,
        c->conn->verify_name,
        c->scheduler,
        c->stream,
        &tls
    );
    // if we succeeded: upgrade our stream
    if(tls) c->stream = tls;
    // success or failure: reawait our stream
    c->stream->await(c->stream, await_cb);
    PROP_VAR(&e, &e2);

    return e;
}

static void advance_state(imap_client_t *c){
    bool ok;

    if(c->canceled || is_error(c->e)) goto cu;

    // wait for greeting
    while(!c->greeting_done){
        PROP_GO(&c->e, advance_reads(c, &ok), cu);
        if(!ok) return;
        PROP_GO(&c->e, process_greeting(c, &ok), cu);
        if(!ok) continue;
        c->greeting_done = true;
    }

    // do starttls if necessary
    while(c->starttls && !c->starttls_done){
        ONCE(c->starttls_started) PROP_GO(&c->e, send_starttls(c), cu);
        PROP_GO(&c->e, advance_writes(c, &ok), cu);
        if(!ok) return;
        PROP_GO(&c->e, advance_reads(c, &ok), cu);
        if(!ok) return;
        PROP_GO(&c->e, process_starttls_resp(c, &ok), cu);
        if(!ok) continue;
        PROP_GO(&c->e, starttls(c), cu);
        c->starttls_done = true;
    }

    // now act as a blind relay
    ONCE(c->relay_started){
        // cmds and writes must be 1:1
        if(!link_list_isempty(&c->cmds)){
            LOG_FATAL("cmds not empty when imap_client starts relaying\n");
        }
        // queue all requested responses
        imap_client_write_t *req;
        LINK_FOR_EACH(req, &c->writes, imap_client_write_t, link){
            imap_cmd_t *cmd = STEAL(imap_cmd_t, &req->cmd);
            link_list_append(&c->cmds, &cmd->link);
        }
    }

    // process read requests
    while(!link_list_isempty(&c->reads)){
        PROP_GO(&c->e, advance_reads(c, &ok), cu);
        if(!ok) break;
        // fulfill one read
        imap_client_read_t *req = CONTAINER_OF(
            link_list_pop_first(&c->reads), imap_client_read_t, link
        );
        imap_resp_t *resp = CONTAINER_OF(
            link_list_pop_first(&c->resps), imap_resp_t, link
        );
        req->cb(c, req, resp);
        // did the user cancel us?
        if(c->canceled) goto cu;
    }

    // process write requests
    PROP_GO(&c->e, advance_writes(c, &ok), cu);
    (void)ok;

    return;

cu:
    // cancel our base stream
    c->stream->cancel(c->stream);
    if(!c->stream->awaited) return;

    // wait to be awaited
    if(!c->await_cb) return;

    // free the underlying connection object
    c->conn->free(c->conn);

    // free cmds or responses
    link_t *link;
    while((link = link_list_pop_first(&c->cmds))){
        imap_cmd_free(CONTAINER_OF(link, imap_cmd_t, link));
    }
    while((link = link_list_pop_first(&c->resps))){
        imap_resp_free(CONTAINER_OF(link, imap_resp_t, link));
    }

    schedulable_cancel(&c->schedulable);

    // await_cb must be last (it might free us)
    c->awaited = true;
    link_t reads = {0};
    link_t writes = {0};
    link_list_append_list(&reads, &c->reads);
    link_list_append_list(&writes, &c->writes);
    // free all commands before returning unfinished io
    imap_client_write_t *req;
    LINK_FOR_EACH(req, &writes, imap_client_write_t, link){
        imap_cmd_free(STEAL(imap_cmd_t, &req->cmd));
    }
    derr_t e = c->e;
    if(!is_error(e) && c->canceled){
        e.type = E_CANCELED;
    }
    c->await_cb(c, e, &reads, &writes);
}

static void free_client_memory(imap_client_t *c){
    if(!c) return;
    link_remove(&c->link);
    imap_resp_reader_free(&c->reader);
    schedulable_cancel(&c->schedulable);
    free(c);
}

derr_t imap_client_new(
    imap_client_t **out,
    scheduler_i *scheduler,
    citm_conn_t *conn
){
    derr_t e = E_OK;

    *out = NULL;

    imap_client_t *c = DMALLOC_STRUCT_PTR(&e, c);
    CHECK(&e);

    *c = (imap_client_t){
        .scheduler = scheduler,
        .conn = conn,
        .tls = { .iface.data = c },
        .starttls = conn->security == IMAP_SEC_STARTTLS,
        .exts = {
            .uidplus = EXT_STATE_ON,
            .enable = EXT_STATE_ON,
            .condstore = EXT_STATE_ON,
            .qresync = EXT_STATE_ON,
            .unselect = EXT_STATE_ON,
            .idle = EXT_STATE_ON,
            .xkey = EXT_STATE_ON,
        },
    };

    PROP_GO(&e, imap_resp_reader_init(&c->reader, &c->exts), fail);

    DSTR_WRAP_ARRAY(c->rbuf, c->rbufmem);
    DSTR_WRAP_ARRAY(c->wbuf, c->wbufmem);

    schedulable_prep(&c->schedulable, schedule_cb);

    if(conn->security == IMAP_SEC_TLS){
        PROP_GO(&e,
            duv_tls_wrap_client(
                &c->tls,
                conn->ctx,
                conn->verify_name,
                scheduler,
                conn->stream,
                &c->stream
            ),
        fail);
        // await only the tls stream
        c->stream->await(c->stream, await_cb);
    }else{
        // use the base for reads and writes
        c->stream = c->conn->stream;
        c->stream->data = c;
        // await the base stream
        c->original_base_await_cb = c->stream->await(
            c->stream, await_cb
        );
    }

    *out = c;

    // client starts reading the greeting automatically
    schedule(c);

    return e;

fail:
    free_client_memory(c);
    return e;
}

void imap_client_cancel(imap_client_t *c){
    if(!c) return;
    c->canceled = true;
    if(!c->awaited) schedule(c);
}

// must be awaited already
void imap_client_free(imap_client_t **cptr){
    imap_client_t *c = *cptr;
    if(!c) return;
    free_client_memory(c);
    *cptr = NULL;
}

// for handling lists of servers/clients, returns ok when list is empty
bool imap_client_list_cancelfree(link_t *list){
    imap_client_t *c, *temp;
    LINK_FOR_EACH_SAFE(c, temp, list, imap_client_t, link){
        imap_client_cancel(c);
        if(c->awaited){
            imap_client_free(&c);
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
bool imap_client_await(
    imap_client_t *c, imap_client_await_cb cb, imap_client_await_cb *out
){
    if(out) *out = NULL;
    DETECT_INVALID(c->awaited, "imap_client_await");
    if(out) *out = c->await_cb;
    c->await_cb = cb;
    schedule(c);
    return true;
}

void imap_client_unawait(imap_client_t *c){
    c->await_cb = NULL;
}

bool imap_client_read(
    imap_client_t *c, imap_client_read_t *req, imap_client_read_cb cb
){
    DETECT_INVALID(c->awaited, "imap_client_read");
    DETECT_INVALID(c->canceled, "imap_client_read");

    *req = (imap_client_read_t){ .cb = cb };
    link_list_append(&c->reads, &req->link);
    schedule(c);
    return true;
}

bool imap_client_write(
    imap_client_t *c,
    imap_client_write_t *req,
    imap_cmd_t *cmd,
    imap_client_write_cb cb
){
    DETECT_INVALID(c->awaited, "imap_client_write");
    DETECT_INVALID(c->canceled, "imap_client_write");

    if(!c->relay_started){
        // at first, we queue up reqs with their command
        *req = (imap_client_write_t){ .cmd = cmd, .cb = cb };
        link_list_append(&c->writes, &req->link);
    }else{
        // after we start relaying, we queue the commands immediately
        *req = (imap_client_write_t){ .cb = cb };
        link_list_append(&c->writes, &req->link);
        link_list_append(&c->cmds, &cmd->link);
    }
    schedule(c);
    return true;
}
