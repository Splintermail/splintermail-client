#include "libcitm/libcitm.h"

#include <version.h>

DEF_CONTAINER_OF(status_client_t, schedulable, schedulable_t)

static void advance_state(status_client_t *sc);

static void scheduled(schedulable_t *s){
    status_client_t *sc = CONTAINER_OF(s, status_client_t, schedulable);
    advance_state(sc);
}

static void schedule(status_client_t *sc){
    sc->scheduler->schedule(sc->scheduler, &sc->schedulable);
}

static void on_connect(uv_connect_t *req, int status){
    status_client_t *sc = req->data;

    if(!is_error(sc->e) && status < 0){
        TRACE_ORIG(&sc->e,
            derr_type_from_uv_status(status),
            "unable to connect to %x: %x",
            FSB(sc->status_sock),
            FUV(status)
        );
    }

    sc->connect_done = true;
    schedule(sc);
}

static void on_close(uv_handle_t *handle){
    status_client_t *sc = handle->data;
    sc->pipe_close_done = true;
    schedule(sc);
}

static void alloc_cb(uv_handle_t *handle, size_t suggested, uv_buf_t *buf){
    (void)suggested;
    status_client_t *sc = handle->data;
    dstr_t space = dstr_empty_space(sc->rbuf);
    if(!space.size) LOG_FATAL("no space left in status_client_t's alloc_cb\n");
    buf->base = space.data;
    buf->len = (unsigned long)space.size;
}

static void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf){
    status_client_t *sc = stream->data;
    (void)buf;

    // if we're shutting down, we care about nothing
    if(is_error(sc->e)) return;

    if(nread < 1){
        switch(nread){
            case 0:
                // EAGAIN or EWOULDBLOCK
                return;
            case UV_EOF:
                ORIG_GO(&sc->e,
                    E_RESPONSE,
                    "server closed our connection; maybe it died?",
                    done
                );
            case UV_ENOBUFS:
                // we disallow empty bufs in read calls
                LOG_FATAL("received UV_ENOBUFS in stream server\n");
            case UV_ECANCELED:
                /* not entirely sure how this could happen, but probably while
                   closing */
                return;
            default:
                // we are donezo
                (void)read_cb;
                derr_type_t etype = derr_type_from_uv_status((int)nread);
                ORIG_GO(&sc->e,
                    etype, "status client read_cb(): %x", done, FEMSG(etype)
                );
        }
    }

    // successful read

    // no need to append; bytes were written in-place
    sc->rbuf.len += (size_t)nread;
    if(sc->rbuf.len > sc->rbuf.size){
        LOG_FATAL("status client read too much somehow\n");
    }

    // stop reading if the read buffer is full
    if(sc->rbuf.len == sc->rbuf.size){
        duv_pipe_read_stop(&sc->pipe);
        sc->reading = false;
        // the line-too-long check is inside advance_reads
    }

done:
    schedule(sc);
}

static void write_cb(uv_write_t *req, int status){
    status_client_t *sc = req->data;

    // if we're shutting down, we care about nothing
    if(is_error(sc->e)) return;

    sc->check_done = true;

    if(status < 0){
        derr_type_t etype = derr_type_from_uv_status(status);
        TRACE_ORIG(&sc->e, etype, "status client write_cb(): %x", FUV(status));
    }

    schedule(sc);
}

static derr_t send_check(status_client_t *sc){
    derr_t e = E_OK;

    DSTR_STATIC(msg, "{\"command\":\"check\"}\n");

    uv_buf_t buf = { .base = msg.data, .len = (unsigned long)msg.len };
    PROP(&e, duv_pipe_write(&sc->wreq, &sc->pipe, &buf, 1, write_cb) );

    sc->check_started = true;

    return e;
}

static derr_t advance_checks(status_client_t *sc){
    derr_t e = E_OK;

    // are we sending a check command?
    if(sc->check_started){
        if(!sc->check_done) return e;
        sc->check_started = false;
        sc->check_done = false;
    }

    // do we need to send a check command?
    if(!sc->check_started && sc->want_check){
        PROP(&e, send_check(sc) );
        sc->check_started = true;
        sc->want_check = false;
    }

    return e;
}

static bool version_matches(status_client_t *sc){
    return sc->version_maj == SPLINTERMAIL_VERSION_MAJOR
        && sc->version_min == SPLINTERMAIL_VERSION_MINOR
        && sc->version_patch == SPLINTERMAIL_VERSION_PATCH;
}

static derr_t handle_response(status_client_t *sc, dstr_t line, json_t json){
    derr_t e = E_OK;

    // first check the version; don't bother talking to the wrong version
    bool have_vmaj;
    bool have_vmin;
    bool have_vpatch;
    jspec_t *vspec = JOBJ(true,
        JKEYOPT("version_maj", &have_vmaj, JI(&sc->version_maj)),
        JKEYOPT("version_min", &have_vmin, JI(&sc->version_min)),
        JKEYOPT("version_patch", &have_vpatch, JI(&sc->version_patch)),
    );
    derr_t e2 = jspec_read(vspec, json.root);
    CATCH(&e2, E_PARAM){
        TRACE(&e2, "unexpected server response: \"%x\"\n", FD_DBG(line));
        RETHROW(&e, &e2, E_RESPONSE);
    }

    if(have_vmaj != have_vmin || have_vmin != have_vpatch){
        ORIG(&e,
            E_RESPONSE,
            "server response had some version info but not all: \"%x\"",
            FD_DBG(line)
        );
    }

    if(have_vmaj && !version_matches(sc)){
        ORIG(&e,
            E_RESPONSE,
            "server/client version mismatch; server is %x.%x.%x but client is "
            "%x.%x.%x... have you restarted the server since you last "
            "upgraded your splintermail installation?",
            FI(sc->version_maj),
            FI(sc->version_min),
            FI(sc->version_patch),
            FI(SPLINTERMAIL_VERSION_MAJOR),
            FI(SPLINTERMAIL_VERSION_MINOR),
            FI(SPLINTERMAIL_VERSION_PATCH),
        );
    }

    // now read the rest of the response
    bool have_conf;
    bool have_fulldomain;
    bool have_smaj;
    bool have_smin;
    bool have_tls;
    jspec_t *spec = JOBJ(true,
        JKEYOPT("configured", &have_conf, JTRI(&sc->configured)),
        JKEYOPT("fulldomain", &have_fulldomain, JDCPY(&sc->fulldomain)),
        JKEYOPT("major", &have_smaj, JDCPY(&sc->status_maj)),
        JKEYOPT("minor", &have_smin, JDCPY(&sc->status_min)),
        JKEYOPT("tls_ready", &have_tls, JTRI(&sc->tls_ready)),
    );
    e2 = jspec_read(spec, json.root);
    CATCH(&e2, E_PARAM){
        TRACE(&e2, "unexpected server response: \"%x\"\n", FD_DBG(line));
        RETHROW(&e, &e2, E_RESPONSE);
    }

    // initial or update status message
    // we treat both the same by caching static values on the status_client_t
    citm_status_t citm_status;
    PROP(&e,
        citm_status_init(
            &citm_status,
            sc->version_maj,
            sc->version_min,
            sc->version_patch,
            sc->fulldomain,
            sc->status_maj,
            sc->status_min,
            sc->configured,
            sc->tls_ready
        )
    );
    // make the user callback
    sc->update_cb(sc->cb_data, citm_status);
    // did the user cancel us
    if(is_error(sc->e)) return (derr_t){ .type = E_CANCELED };

    return e;
}

static derr_t advance_reads(status_client_t *sc){
    derr_t e = E_OK;

    // is there nothing new to read?
    if(sc->rbuf.len <= sc->read_checked) return e;

    while(true){
        dstr_t new = dstr_sub2(sc->rbuf, sc->read_checked, SIZE_MAX);
        dstr_t line, remainder;
        size_t n;
        dstr_split2_soft(new, DSTR_LIT("\n"), &n, &line, &remainder);
        if(n == 1){
            // no new line yet
            if(sc->rbuf.len == sc->rbuf.size){
                ORIG(&e,
                    E_RESPONSE,
                    "status server sent too long of a response: \"%x\"",
                    FD_DBG(sc->rbuf)
                );
            }
            sc->read_checked = sc->rbuf.len;
            return e;
        }
        // we have a line; reconfigure it to include what came before
        line = dstr_sub2(sc->rbuf, 0, sc->read_checked + line.len);
        // parse the line
        PROP(&e, json_parse(line, &sc->json) );
        // handle the line
        PROP(&e, handle_response(sc, line, sc->json) );
        json_free(&sc->json);
        // done with this line
        dstr_leftshift(&sc->rbuf, line.len + 1);
        sc->read_checked = 0;
        // restart reading?
        if(!sc->reading){
            PROP(&e, duv_pipe_read_start(&sc->pipe, alloc_cb, read_cb) );
            sc->reading = true;
        }
    }
}

static void advance_state(status_client_t *sc){
    if(is_error(sc->e)) goto cu;

    derr_t e = E_OK;

    // first connect our pipe
    if(!sc->connect_done){
        ONCE(sc->connect_started){
            PROP_GO(&e,
                duv_pipe_connect_path(
                    &sc->connect, &sc->pipe, sc->status_sock, on_connect
                ),
            fail);
        }
        return;
    }

    ONCE(sc->initial_read_start){
        PROP_GO(&e, duv_pipe_read_start(&sc->pipe, alloc_cb, read_cb), fail);
        sc->reading = true;
    }

    PROP_GO(&e, advance_checks(sc), fail);
    PROP_GO(&e, advance_reads(sc), fail);

    return;

fail:
    // if we exited because the user closed us in a callback, don't propagate
    if(!is_error(sc->e)){
        sc->e = e;
        e = (derr_t){0};
    }
cu:
    /* libuv documents that uv_cancel() doesn't work on connect requests, so
       we can't cancel it, but we can close the underlying pipe */
    ONCE(sc->pipe_close_started){
        duv_pipe_close(&sc->pipe, on_close);
    }
    if(sc->connect_started && !sc->connect_done) return;
    if(!sc->pipe_close_done) return;

    // finish cleanup and return

    sc->done = true;

    dstr_free(&sc->fulldomain);
    dstr_free(&sc->status_maj);
    dstr_free(&sc->status_min);
    dstr_free(&sc->rbuf);
    // dstr_free(&sc->wbuf);
    json_free(&sc->json);

    e = sc->e;
    sc->e = (derr_t){0};
    sc->done_cb(sc->cb_data, e);
}

derr_t status_client_init(
    status_client_t *sc,
    uv_loop_t *loop,
    scheduler_i *scheduler,
    string_builder_t status_sock,
    status_update_cb update_cb,
    status_done_cb done_cb,
    void *cb_data
){
    derr_t e = E_OK;

    *sc = (status_client_t){
        .scheduler = scheduler,
        .status_sock = status_sock,
        .update_cb = update_cb,
        .done_cb = done_cb,
        .cb_data = cb_data,
        .wreq = { .data = sc },
        .connect = { .data = sc },
    };

    schedulable_prep(&sc->schedulable, scheduled);

    json_prep(&sc->json);

    PROP_GO(&e, dstr_new(&sc->rbuf, 4096), fail);
    // PROP_GO(&e, dstr_new(wbuf, 128), fail);

    PROP_GO(&e, duv_pipe_init(loop, &sc->pipe, 0), fail);
    sc->pipe.data = sc;

    sc->started = true;
    schedule(sc);

    return e;

fail:
    dstr_free(&sc->rbuf);
    // dstr_free(&sc->wbuf);
    return e;
}

// ask the server to check on its configuration
void status_client_check(status_client_t *sc){
    sc->want_check = true;
    schedule(sc);
}

// returns true if done_cb is coming
bool status_client_close(status_client_t *sc){
    if(!sc->started || sc->done) return false;
    if(is_error(sc->e)) return true;
    sc->e.type = E_CANCELED;
    schedule(sc);
    return true;
}
