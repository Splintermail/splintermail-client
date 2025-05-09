#include "libcitm/libcitm.h"

#include <config.h>

DEF_CONTAINER_OF(status_server_t, schedulable, schedulable_t)

static void advance_state(status_server_t *ss);

static void scheduled(schedulable_t *s){
    status_server_t *ss = CONTAINER_OF(s, status_server_t, schedulable);
    advance_state(ss);
}

static void schedule(status_server_t *ss){
    ss->scheduler->schedule(ss->scheduler, &ss->schedulable);
}

struct ss_client_t {
    status_server_t *ss;
    link_t wlink;
    link_t rlink;
    uv_pipe_t pipe;
    char *bad;
    bool close_on_write;
    bool closed;
    dstr_t rbuf;
    char _rbuf[4096];
};

DEF_CONTAINER_OF(ss_client_t, wlink, link_t)
DEF_CONTAINER_OF(ss_client_t, rlink, link_t)

static void client_close_cb(uv_handle_t *handle){
    ss_client_t *c = handle->data;
    link_remove(&c->wlink);
    status_server_t *ss = c->ss;
    free(c);
    schedule(ss);
}

static void ss_client_close(status_server_t *ss, ss_client_t *c){
    if(c->closed) return;
    c->closed = true;
    link_remove(&c->rlink);
    link_remove(&c->wlink);
    link_list_append(&ss->clients_closing, &c->wlink);
    duv_pipe_close(&c->pipe, client_close_cb);
}

static void close_all_clients(status_server_t *ss, link_t *list){
    link_t *link;
    while((link = link_list_pop_first(list))){
        ss_client_t *c = CONTAINER_OF(link, ss_client_t, wlink);
        ss_client_close(ss, c);
    }
}

static void ss_client_bad(status_server_t *ss, ss_client_t *c, char *why){
    link_remove(&c->rlink);
    link_remove(&c->wlink);
    link_list_append(&ss->clients_bad, &c->wlink);
    c->bad = why;
    // no more reads for this one
    duv_pipe_read_stop(&c->pipe);
}

static void alloc_cb(uv_handle_t *handle, size_t suggested, uv_buf_t *buf){
    (void)suggested;
    ss_client_t *c = handle->data;
    dstr_t space = dstr_empty_space(c->rbuf);
    if(!space.size) LOG_FATAL("no space left in ss_client_t's alloc_cb\n");
    buf->base = space.data;
    buf->len = (unsigned long)space.size;
}

static void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf){
    ss_client_t *c = stream->data;
    status_server_t *ss = c->ss;
    derr_type_t etype;

    // if we're shutting down, we care about nothing
    if(is_error(ss->e)) return;

    if(nread < 1){
        switch(nread){
            case 0:
                // EAGAIN or EWOULDBLOCK
                return;
            case UV_EOF:
                // don't care; just stop reading on this one
                duv_pipe_read_stop(&c->pipe);
                return;
            case UV_ENOBUFS:
                // we disallow empty bufs in read calls
                LOG_FATAL("received UV_ENOBUFS in stream server\n");
            case UV_ECANCELED:
                /* not entirely sure how this could happen, but probably while
                   closing */
                return;
            default:
                // this client is donezo
                etype = derr_type_from_uv_status((int)nread);
                LOG_DEBUG(
                    "status client read_cb(): %x: %x\n",
                    FENAME(etype),
                    FEMSG(etype)
                );
                ss_client_close(ss, c);
                // close_cb will wake us up
                return;
        }
    }

    // successful read
    dstr_t dbuf = dstr_from_cstrn(buf->base, (size_t)nread, false);

    // no need to append; bytes were written in-place
    c->rbuf.len += dbuf.len;
    if(c->rbuf.len > c->rbuf.size){
        LOG_FATAL("status client read too much somehow\n");
    }

    // did we see a complete command?
    if(dstr_contains(dbuf, DSTR_LIT("\n"))){
        // queue client for read processing
        link_remove(&c->rlink);
        link_list_append(&ss->reads, &c->rlink);
    }

    // stop reading if the read buffer is full
    if(c->rbuf.len == c->rbuf.size){
        duv_pipe_read_stop(&c->pipe);
        // buffer must contain a command or client would be stalled
        if(!dstr_contains(c->rbuf, DSTR_LIT("\n"))){
            ss_client_bad(ss, c, "command too long");
        }
    }

    schedule(ss);
}

// ss_write_cb is called either by an injected write cb, or by a normal one
static void ss_write_cb(status_server_t *ss, ss_client_t *c, int status){
    ss->writing = false;

    // if we're shutting down, we care about nothing
    if(is_error(ss->e)) return;

    if(status < 0){
        // don't crash the server, just close this one client
        derr_type_t etype = derr_type_from_uv_status(status);
        LOG_DEBUG(
            "status client write_cb(): %x: %x\n", FENAME(etype), FEMSG(etype)
        );
        ss_client_close(ss, c);
    }else if(c->close_on_write){
        // this client needs closing anyway
        ss_client_close(ss, c);
    }
    // don't schedule here, in case this is an injected write cb
}

static void write_cb(uv_write_t *req, int status){
    ss_client_t *c = req->data;
    status_server_t *ss = c->ss;
    ss_write_cb(ss, c, status);
    schedule(ss);
}

static derr_t do_write(status_server_t *ss, ss_client_t *c, jdump_i *j){
    derr_t e = E_OK;

    ss->wbuf.len = 0;

    // dump the object
    PROP(&e, jdump(j, WD(&ss->wbuf), 0) );
    // complete the line
    PROP(&e, dstr_append(&ss->wbuf, &DSTR_LIT("\n")) );

    ss->wreq.data = c;
    uv_buf_t buf = {
        .base = ss->wbuf.data, .len = (unsigned long)ss->wbuf.len
    };
    // in windows, this can return broken pipe errors... grr.
    int ret = uv_write(
        &ss->wreq, duv_pipe_stream(&c->pipe), &buf, 1, write_cb
    );
    if(ret == UV_EPIPE){
        /* in windows, we can get broken pipe errors here, which is super
           annoying, since we already have to handle that in the write_cb, so
           we'll pretend we handle it in the write_cb anyway to keep the code
           simple */
        ss->inject_write_cb_client = c;
        ss->inject_write_cb_status = ret;
        schedule(ss);
    }else if(ret < 0){
        derr_type_t etype = derr_type_from_uv_status(ret);
        ORIG(&e, etype, "uv_write failed: %x",  FUV(ret));
    }

    ss->writing = true;

    return e;
}

static tri_e is_configured(status_server_t *ss){
    switch(ss->maj){
        case STATUS_MAJ_NO_TLS: return TRI_NA;
        case STATUS_MAJ_MANUAL_CERT: return TRI_NA;
        case STATUS_MAJ_NEED_CONF: return TRI_NO;
        case STATUS_MAJ_TLS_FIRST: return TRI_YES;
        case STATUS_MAJ_TLS_EXPIRED: return TRI_YES;
        case STATUS_MAJ_TLS_RENEW: return TRI_YES;
        case STATUS_MAJ_TLS_GOOD: return TRI_YES;
    }
    LOG_FATAL("unknown status_maj: %x\n", FI(ss->maj));
}

static tri_e is_tls_ready(status_server_t *ss){
    switch(ss->maj){
        case STATUS_MAJ_NO_TLS: return TRI_NA;
        case STATUS_MAJ_MANUAL_CERT: return TRI_NA;
        case STATUS_MAJ_NEED_CONF: return TRI_NO;
        case STATUS_MAJ_TLS_FIRST: return TRI_NO;
        case STATUS_MAJ_TLS_EXPIRED: return TRI_NO;
        case STATUS_MAJ_TLS_RENEW: return TRI_YES;
        case STATUS_MAJ_TLS_GOOD: return TRI_YES;
    }
    LOG_FATAL("unknown status_maj: %x\n", FI(ss->maj));
}

#define DMAJOR(maj) DD(status_maj_dstr(maj))
#define DMINOR(min) DD(status_min_dstr(min))

#define DSTATUS_KEYS(ss) \
    DKEY("major", DMAJOR(ss->maj)), \
    DKEY("minor", DMINOR(ss->min)), \
    DKEY("fulldomain", DD(ss->fulldomain)), \
    DKEY("configured", DTRI(is_configured(ss))), \
    DKEY("tls_ready", DTRI(is_tls_ready(ss)))

static derr_t write_new(status_server_t *ss, ss_client_t *c){
    derr_t e = E_OK;

    jdump_i *j = DOBJ(
        DKEY("version_maj", DI(SM_VER_MAJ)),
        DKEY("version_min", DI(SM_VER_MIN)),
        DKEY("version_patch", DI(SM_VER_PAT)),
        DSTATUS_KEYS(ss)
    );

    PROP(&e, do_write(ss, c, j) );

    return e;
}

static derr_t write_update(status_server_t *ss, ss_client_t *c){
    derr_t e = E_OK;

    jdump_i *j = DOBJ(DSTATUS_KEYS(ss));

    PROP(&e, do_write(ss, c, j) );

    return e;
}

static derr_t write_bad(status_server_t *ss, ss_client_t *c){
    derr_t e = E_OK;

    jdump_i *j = DOBJ(DKEY("fail", DS(c->bad)));

    PROP(&e, do_write(ss, c, j) );

    return e;
}

// returns true if we decide to write something
static derr_t process_cmd(status_server_t *ss, ss_client_t *c, dstr_t cmd){
    derr_t e = E_OK;

    jdump_i *j;
    dstr_t badreason;

    // parse the response
    json_free(&ss->json);
    IF_PROP(&e, json_parse(cmd, &ss->json) ){
        // invalid json
        badreason = DSTR_LIT("invalid json");
        DROP_VAR(&e);
        goto bad;
    }
    dstr_t command;
    jspec_t *spec = JOBJ(false, JKEY("command", JDREF(&command)));
    IF_PROP(&e, jspec_read(spec, ss->json.root) ){
        // invalid json
        badreason = DSTR_LIT("invalid command");
        DROP_VAR(&e);
        goto bad;
    }
    // right now only the "check" command is allowed
    if(!dstr_eq(command, DSTR_LIT("check"))){
        badreason = DSTR_LIT("unrecognized command");
        goto bad;
    }
    // make our check_cb
    ss->check_cb(ss->cb_data);
    /* we don't respond to a successful check command, since our response here
       wouldn't be synchronized with the actual check, and a well-behaved
       client will always see an update message at the time that a check
       command would be expected anyway */
    return e;

bad:
    j = DOBJ(
        DKEY("status", DS("error")),
        DKEY("reason", DD(badreason)),
    );
    PROP(&e, do_write(ss, c, j) );
    return e;
}

static void listener_close_cb(uv_handle_t *handle){
    status_server_t *ss = handle->data;
    ss->listener_close_done = true;
    schedule(ss);
}

static void advance_state(status_server_t *ss){
    link_t *link;

    // even before checking for errors, inject any fake write_cbs with errors
    if(ss->inject_write_cb_client){
        ss_client_t *c = ss->inject_write_cb_client;
        int status = ss->inject_write_cb_status;
        ss->inject_write_cb_client = NULL;
        ss->inject_write_cb_status = 0;
        ss_write_cb(ss, c, status);
    }

    if(is_error(ss->e)) goto cu;

    // if we are already writing, there's nothing we can do
    if(ss->writing) return;

    // do any clients need an initial message?
    if((link = link_list_pop_first(&ss->clients_new))){
        ss_client_t *c = CONTAINER_OF(link, ss_client_t, wlink);
        link_list_append(&ss->clients_idle, &c->wlink);
        PROP_GO(&ss->e, write_new(ss, c), cu);
        // also start reading from these clients
        PROP_GO(&ss->e, duv_pipe_read_start(&c->pipe, alloc_cb, read_cb), cu);
        return;
    }

    // do any clients need an update message?
    if((link = link_list_pop_first(&ss->clients_stale))){
        ss_client_t *c = CONTAINER_OF(link, ss_client_t, wlink);
        link_list_append(&ss->clients_idle, &c->wlink);
        PROP_GO(&ss->e, write_update(ss, c), cu);
        return;
    }

    // do any bad clients need punishment?
    // (we don't pop from this list; it is removed when the write completes)
    if(!link_list_isempty(&ss->clients_bad)){
        link = ss->clients_bad.next;
        ss_client_t *c = CONTAINER_OF(link, ss_client_t, wlink);
        c->close_on_write = true;
        PROP_GO(&ss->e, write_bad(ss, c), cu);
        return;
    }

    // have any clients written commands to us?
    while((link = link_list_pop_first(&ss->reads))){
        ss_client_t *c = CONTAINER_OF(link, ss_client_t, rlink);
        dstr_t cmd, leftover;
        size_t n;
        dstr_split2_soft(c->rbuf, DSTR_LIT("\n"), &n, &cmd, &leftover);
        if(n == 1){
            // there isn't a complete command here
            continue;
        }
        PROP_GO(&ss->e, process_cmd(ss, c, cmd), cu);
        // is there another command left to process?
        if(dstr_contains(leftover, DSTR_LIT("\n"))){
            link_list_prepend(&ss->reads, &c->rlink);
        }
        bool was_full = c->rbuf.len == c->rbuf.size;
        // leftshift the command and its newline
        dstr_leftshift(&c->rbuf, cmd.len + 1);
        if(was_full){
            // start reading again
            PROP_GO(&ss->e,
                duv_pipe_read_start(&c->pipe, alloc_cb, read_cb),
            cu);
        }
        return;
    }

    return;

cu:
    // close our listener first, so we stop getting new clients
    ONCE(ss->listener_close_started){
        duv_pipe_close(&ss->pipe, listener_close_cb);
    }
    if(!ss->listener_close_done) return;

    // close every client
    close_all_clients(ss, &ss->clients_new);
    close_all_clients(ss, &ss->clients_stale);
    close_all_clients(ss, &ss->clients_idle);
    close_all_clients(ss, &ss->clients_bad);
    if(!link_list_isempty(&ss->clients_closing)) return;

    // free memory and shut down
    json_free(&ss->json);
    dstr_free(&ss->wbuf);
    dstr_free(&ss->json_text);
    schedulable_cancel(&ss->schedulable);

    #ifndef _WIN32
    // UNIX only: let go of our file lock
    duv_unlock_fd(ss->lockfd);
    ss->lockfd = -1;
    #endif

    derr_t e = ss->e;
    ss->e = E_OK;
    ss->closed = true;
    ss->done_cb(ss->cb_data, e);
}

static void fail_pipe_close_cb(uv_handle_t *handle){
    ss_client_t *c = handle->data;
    free(c);
}

static void on_listener(uv_stream_t *stream, int status){
    derr_t e = E_OK;

    status_server_t *ss = stream->data;
    ss_client_t *c = NULL;

    if(status < 0){
        // we have no way to recover from a broken listener
        ORIG_GO(&e,
            uv_err_type(status),
            "on_listener failed, shutting down: %x",
            fail,
            FUV(status)
        );
    }

    // there's no way to recover from an failed malloc right here
    c = DMALLOC_STRUCT_PTR(&e, c);
    CHECK_GO(&e, fail);
    c->ss = ss;
    c->pipe.data = c;
    DSTR_WRAP_ARRAY(c->rbuf, c->_rbuf);

    // there's no way to recover from an failed malloc right here
    PROP_GO(&e, duv_pipe_init(ss->loop, &c->pipe, 0), fail);

    // this is guaranteed not to fail
    PROP_GO(&e, duv_pipe_accept(&ss->pipe, &c->pipe), fail_pipe);

    // finish setup in advance_state
    link_list_append(&ss->clients_new, &c->wlink);
    schedule(ss);

    return;

fail_pipe:
    duv_pipe_close(&c->pipe, fail_pipe_close_cb);
fail:
    TRACE(&e, "unrecoverable failure in status_server's on_listener()!\n");
    TRACE_MULTIPROP_VAR(&ss->e, &e);
    schedule(ss);
}

static void on_fail_close_cb(uv_handle_t *handle){
    (void)handle;
}

derr_t status_server_init(
    status_server_t *ss,
    int *sockfd,  // sockfd only available in unix, from systemd/launchd
    uv_loop_t *loop,
    scheduler_i *scheduler,
    string_builder_t sockpath,
    status_maj_e maj,
    status_min_e min,
    dstr_t fulldomain,
    status_check_cb check_cb,
    status_done_cb done_cb,
    void *cb_data
){
    derr_t e = E_OK;

    *ss = (status_server_t){
        .loop = loop,
        .scheduler = scheduler,
        .maj = maj,
        .min = min,
        .check_cb = check_cb,
        .done_cb = done_cb,
        .cb_data = cb_data,
        #ifndef _WIN32
        .lockfd = -1,
        #endif
    };

    schedulable_prep(&ss->schedulable, scheduled);

    PROP_GO(&e, dstr_new(&ss->wbuf, 4096), fail);
    PROP_GO(&e, dstr_new(&ss->json_text, 4096), fail);

    size_t nnodes = sizeof(ss->json_nodes)/sizeof(*ss->json_nodes);
    json_prep_preallocated(
        &ss->json, &ss->json_text, ss->json_nodes, nnodes, false
    );

    DSTR_WRAP_ARRAY(ss->fulldomain, ss->_fulldomain);
    PROP_GO(&e, dstr_copy(&fulldomain, &ss->fulldomain), fail);

    PROP_GO(&e, duv_pipe_init(loop, &ss->pipe, 0), fail);
    ss->pipe.data = ss;

#ifdef _WIN32
    PROP_GO(&e, duv_pipe_bind_path(&ss->pipe, sockpath), fail);
    // any user should have read/write access to our pipe
    PROP_GO(&e, duv_pipe_chmod(&ss->pipe, UV_WRITABLE | UV_READABLE), fail);
#else
    // unix: inherit a socket or create our own
    if(sockfd && *sockfd > -1){
        // socket fd was provided by systemd/launchd
        PROP_GO(&e, duv_pipe_open(&ss->pipe, *sockfd), fail);
        *sockfd = -1;
    }else{
        // create our own
        // note that sockpath is limited to like 108 chars for AF_UNIX sockets
        DSTR_VAR(lock, 256);
        PROP_GO(&e, FMT(&lock, "%x", FSB(sockpath)), fail);
        if(dstr_endswith2(lock, DSTR_LIT(".sock"))) lock.len -= 5;
        PROP_GO(&e, dstr_append(&lock, &DSTR_LIT(".lock")), fail);
        PROP_GO(&e,
            duv_pipe_bind_with_lock(
                &ss->pipe, sockpath, SBD(lock), &ss->lockfd
            ),
        fail);
        // any user should have read/write access to our pipe
        PROP_GO(&e,
            duv_pipe_chmod(&ss->pipe, UV_WRITABLE | UV_READABLE),
        fail);
    }
#endif

    PROP_GO(&e, duv_pipe_listen(&ss->pipe, 5, on_listener), fail);

    ss->started = true;

    return e;

fail:
    if(sockfd && *sockfd > -1){ compat_close(*sockfd); *sockfd = -1; }
    if(ss->pipe.data) duv_pipe_close(&ss->pipe, on_fail_close_cb);
    dstr_free(&ss->wbuf);
    dstr_free(&ss->json_text);

    return e;
}

// returns true if a done_cb is coming
bool status_server_close(status_server_t *ss){
    if(ss->closed || !ss->started) return false;
    if(is_error(ss->e)) return true;
    ss->e = (derr_t){ .type = E_CANCELED };
    schedule(ss);
    return true;
}

void status_server_update(
    status_server_t *ss,
    status_maj_e maj,
    status_min_e min,
    // fulldomain is a reference to the acme_manager_t's memory
    dstr_t fulldomain
){
    if(ss->closed || is_error(ss->e) || !ss->started) return;

    ss->maj = maj;
    ss->min = min;

    derr_t e = dstr_copy(&fulldomain, &ss->fulldomain);
    TRACE_MULTIPROP_VAR(&ss->e, &e);

    // all idle clients transition to stale
    link_list_append_list(&ss->clients_stale, &ss->clients_idle);

    schedule(ss);
}
