#include "libcli/libcli.h"

#include "test/test_utils.h"

typedef enum {
    QUIT_BEFORE_CONNECT = 0,
    QUIT_BEFORE_INITIAL,
    QUIT_AFTER_INITIAL,
    QUIT_WITH_INCOMING_WRITE,
    GET_REJECTED,
    GET_READ_STOP,
    FINISH_EVERYTHING,
    NUM_PLANS,
} plan_e;

static char *plan_name[] = {
    "QUIT_BEFORE_CONNECT",
    "QUIT_BEFORE_INITIAL",
    "QUIT_AFTER_INITIAL",
    "QUIT_WITH_INCOMING_WRITE",
    "GET_REJECTED",
    "GET_READ_STOP",
    "FINISH_EVERYTHING",
    "NUM_PLANS",
};

typedef struct {
    duv_root_t root;
    advancer_t advancer;

    plan_e plan;
    string_builder_t sock;

    status_server_t ss;
    uv_pipe_t pipe;
    uv_connect_t creq;
    uv_write_t wreq;

    dstr_t rbuf;
    char _rbuf[4096];

    int check_cbs;
    int exp_check_cbs;
    bool exp_ss_done;
    // exp_eof state can trigger in the read_cb, before advance_up
    // 1 = exp eof after the next line
    // 2 = exp eof right now
    int exp_eof;
    int write_cbs;

    int quit_at_write_cb;
    int send_2_at_write_cb;

    bool init : 1;
    bool connected : 1;
    bool start_reading : 1;
    bool initial_recv : 1;
    bool reject_recv : 1;
    bool eof_recv : 1;
    bool nonjson_sent : 1;
    bool nonjson_recv : 1;
    bool invalid_sent : 1;
    bool invalid_recv : 1;
    bool unrecog_sent_1 : 1;
    bool unrecog_sent_2 : 1;
    bool unrecog_recv : 1;
    bool check_sent : 1;
    bool read_stop_recv_1 : 1;
    bool read_stop_recv_2 : 1;
    bool update_triggered : 1;
    bool update_recv : 1;
} test_server_t;

DEF_CONTAINER_OF(test_server_t, advancer, advancer_t)

static void ts_ss_check_cb(void *data){
    derr_t e = E_OK;
    test_server_t *ts = data;
    ts->check_cbs++;
    EXPECT_I_GO(&e, "check_cbs", ts->check_cbs, ts->exp_check_cbs, done);
done:
    advancer_schedule(&ts->advancer, e);
}

static void ts_ss_done_cb(void *data, derr_t err){
    derr_t e = E_OK;
    test_server_t *ts = data;
    EXPECT_E_VAR_GO(&e, "done_cb(err=)", &err, E_CANCELED, done);
    EXPECT_B_GO(&e, "exp_ss_done", ts->exp_ss_done, true, done);
done:
    advancer_schedule(&ts->advancer, e);
}

static void ts_handle_close_cb(uv_handle_t *handle){
    test_server_t *ts = handle->data;
    handle->data = NULL;
    advancer_schedule(&ts->advancer, E_OK);
}

static void ts_connect_cb(uv_connect_t *creq, int status){
    derr_t e = E_OK;
    test_server_t *ts = creq->data;
    if(status < 0){
        ORIG_GO(&e, E_VALUE, "failed to connect: %x", done, FUV(status));
    }
    ts->connected = true;
done:
    advancer_schedule(&ts->advancer, e);
}

static void ts_alloc_cb(uv_handle_t *handle, size_t suggested, uv_buf_t *buf){
    (void)suggested;
    test_server_t *ts = handle->data;
    dstr_t space = dstr_empty_space(ts->rbuf);
    if(!space.size) LOG_FATAL("no space left in ts_alloc_cb\n");
    buf->base = space.data;
    buf->len = (unsigned long)space.size;
}

static void ts_read_cb(
    uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf
){
    derr_t e = E_OK;
    test_server_t *ts = stream->data;

    if(nread == 0){
        // EAGAIN or EWOULDBLOCK
        return;
    }

    if(ts->exp_eof == 2){
        if(nread != UV_EOF){
            ORIG_GO(&e,
                E_VALUE,
                "expect read_cb(status=UV_EOF) but got %x\n",
                done,
                FUV((int)nread)
            );
        }
        ts->eof_recv = true;
        goto done;
    }

    if(nread < 0){
        int status = (int)nread;
        ORIG_GO(&e, E_VALUE, "failed to read: %x", done, FUV(status));
    }

    (void)buf;
    ts->rbuf.len += (size_t)nread;

    if(ts->exp_eof == 1 && dstr_contains(ts->rbuf, DSTR_LIT("\n"))){
        ts->exp_eof = 2;
    }

done:
    advancer_schedule(&ts->advancer, e);
}

static void ts_write_cb(uv_write_t *req, int status){
    derr_t e = E_OK;
    test_server_t *ts = req->data;
    ts->write_cbs++;
    if(status < 0){
        ORIG_GO(&e, E_VALUE, "failed to write: %x", done, FUV(status));
    }
done:
    advancer_schedule(&ts->advancer, e);
}

static derr_t test_server_advance_up(advancer_t *advancer){
    derr_t e = E_OK;

    test_server_t *ts = CONTAINER_OF(advancer, test_server_t, advancer);

    TONCE(ts->init){
        PROP(&e,
            status_server_init(
                &ts->ss,
                &ts->root.loop,
                &ts->root.scheduler.iface,
                ts->sock,
                STATUS_MAJ_NEED_CONF,
                STATUS_MIN_NONE,
                DSTR_LIT(""), // fulldomain
                ts_ss_check_cb,
                ts_ss_done_cb,
                ts // cb_data
            )
        );

        PROP(&e, duv_pipe_init(&ts->root.loop, &ts->pipe, 0) );
        ts->pipe.data = ts;

        if(ts->plan == QUIT_BEFORE_CONNECT){
            advancer_up_done(&ts->advancer);
            return e;
        }

        PROP(&e,
            duv_pipe_connect_path(
                &ts->creq, &ts->pipe, ts->sock, ts_connect_cb
            )
        );
        ts->creq.data = ts;
    }

    if(!ts->connected) return e;
    TONCE(ts->start_reading){
        if(ts->plan == QUIT_BEFORE_INITIAL){
            advancer_up_done(&ts->advancer);
            return e;
        }
        PROP(&e, duv_pipe_read_start(&ts->pipe, ts_alloc_cb, ts_read_cb) );
        if(ts->plan == GET_REJECTED){
            /* start our invalid write now, so we can exercise the server's
               incomplete-line-read codepath.  Also make it long so we can
               exercise the server's reject-long-lines codepath */
            static char xbuf[4096];
            for(size_t i = 0; i < sizeof(xbuf); i++){
                xbuf[i] = 'x';
            }
            uv_buf_t buf = { .base = xbuf, .len = 4096 };
            PROP(&e,
                duv_pipe_write(&ts->wreq, &ts->pipe, &buf, 1, ts_write_cb)
            );
        }
    }

    TFINISH(ts->initial_recv){
        // wait for a complete line
        if(!dstr_contains(ts->rbuf, DSTR_LIT("\n"))) return e;
        DSTR_VAR(exp, 4096);
        PROP(&e,
            FMT(&exp,
                "{"
                    "\"version_maj\":%x,"
                    "\"version_min\":%x,"
                    "\"version_patch\":%x,"
                    "\"major\":\"%x\","
                    "\"minor\":\"%x\","
                    "\"fulldomain\":\"\","
                    "\"configured\":\"no\","
                    "\"tls_ready\":\"no\""
                "}\n",
                FI(SPLINTERMAIL_VERSION_MAJOR),
                FI(SPLINTERMAIL_VERSION_MINOR),
                FI(SPLINTERMAIL_VERSION_PATCH),
                FD(status_maj_dstr(STATUS_MAJ_NEED_CONF)),
                FD(status_min_dstr(STATUS_MIN_NONE))
            )
        );
        EXPECT_D3(&e, "initial read", ts->rbuf, exp);
        ts->rbuf.len = 0;
        if(ts->plan == QUIT_AFTER_INITIAL){
            advancer_up_done(&ts->advancer);
            return e;
        }
    }


    // send an nonjson command
    TONCE(ts->nonjson_sent){
        if(ts->plan == GET_REJECTED) if(ts->write_cbs < 1) return e;
        DSTR_STATIC(nonjson, "abvosiejfeoi\n");
        uv_buf_t buf = {
            .base = nonjson.data, .len = (unsigned long)nonjson.len
        };
        PROP(&e, duv_pipe_write(&ts->wreq, &ts->pipe, &buf, 1, ts_write_cb) );
        if(ts->plan == QUIT_WITH_INCOMING_WRITE){
            // exercise server write_cb errors; quit after the next write_cb
            ts->quit_at_write_cb = ts->write_cbs + 1;
        }
        if(ts->plan == GET_REJECTED){
            // we expect one message, then an EOF
            ts->exp_eof = 1;
        }
    }
    if(ts->plan == QUIT_WITH_INCOMING_WRITE){
        if(ts->write_cbs < ts->quit_at_write_cb) return e;
        advancer_up_done(&ts->advancer);
        return e;
    }
    if(ts->plan == GET_REJECTED){
        TFINISH(ts->reject_recv){
            // wait for a complete line
            if(!dstr_contains(ts->rbuf, DSTR_LIT("\n"))) return e;
            DSTR_STATIC(exp, "{\"fail\":\"command too long\"}\n");
            EXPECT_D3(&e, "rejection response", ts->rbuf, exp);
            ts->rbuf.len = 0;
        }
        if(!ts->eof_recv) return e;
        advancer_up_done(&ts->advancer);
        return e;
    }
    TFINISH(ts->nonjson_recv){
        // wait for a complete line
        if(!dstr_contains(ts->rbuf, DSTR_LIT("\n"))) return e;
        DSTR_STATIC(exp,
            "{"
                "\"status\":\"error\","
                "\"reason\":\"invalid json\""
            "}\n"
        );
        EXPECT_D3(&e, "nonjson response", ts->rbuf, exp);
        ts->rbuf.len = 0;
    }

    // send an invalid command
    TONCE(ts->invalid_sent){
        DSTR_STATIC(invalid, "true\n");
        uv_buf_t buf = {
            .base = invalid.data, .len = (unsigned long)invalid.len
        };
        PROP(&e, duv_pipe_write(&ts->wreq, &ts->pipe, &buf, 1, ts_write_cb) );
    }
    TFINISH(ts->invalid_recv){
        // wait for a complete line
        if(!dstr_contains(ts->rbuf, DSTR_LIT("\n"))) return e;
        DSTR_STATIC(exp,
            "{"
                "\"status\":\"error\","
                "\"reason\":\"invalid command\""
            "}\n"
        );
        EXPECT_D3(&e, "invalid response", ts->rbuf, exp);
        ts->rbuf.len = 0;
    }

    /* send an unrecognized command, in two parts to exercise the "read
       incomplete lines" codepath */
    TONCE(ts->unrecog_sent_1){
        DSTR_STATIC(invalid, "{\"command\":");
        uv_buf_t buf = {
            .base = invalid.data, .len = (unsigned long)invalid.len
        };
        PROP(&e, duv_pipe_write(&ts->wreq, &ts->pipe, &buf, 1, ts_write_cb) );
        // wait for this to be written before writing another
        ts->send_2_at_write_cb = ts->write_cbs + 1;
    }
    if(ts->write_cbs < ts->send_2_at_write_cb) return e;
    TONCE(ts->unrecog_sent_2){
        DSTR_STATIC(invalid, "\"halt-and-catch-fire\"}\n");
        uv_buf_t buf = {
            .base = invalid.data, .len = (unsigned long)invalid.len
        };
        PROP(&e, duv_pipe_write(&ts->wreq, &ts->pipe, &buf, 1, ts_write_cb) );
    }
    TFINISH(ts->unrecog_recv){
        // wait for a complete line
        if(!dstr_contains(ts->rbuf, DSTR_LIT("\n"))) return e;
        DSTR_STATIC(exp,
            "{"
                "\"status\":\"error\","
                "\"reason\":\"unrecognized command\""
            "}\n"
        );
        EXPECT_D3(&e, "unrecognized response", ts->rbuf, exp);
        ts->rbuf.len = 0;
    }

    // client send a check
    TONCE(ts->check_sent){
        /* in the GET_READ_STOP case, we need a full buffer composed of
           multiple not-too-long lines */
        /* also use three commands instead of just 2 to exercise the "multiple
           complete commands in one buffer" case */
        #define z31 "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"
        #define z32 z31 "z"
        #define z255  z32 z32 z32 z32 z32 z32 z32 z31
        #define z256  z255 "z"
        #define z1023  z256 z256 z256 z255
        #define z1024  z1023 "z"
        #define z4095  z1024 z1024 z1024 z1023
        DSTR_STATIC(check,
            "{\"command\":\"check\"}\n"
            "{\"command\":\"unrecog\"}\n"
            z4095 "\n"
        );
        size_t len = ts->plan == GET_READ_STOP ? check.len : 20;
        uv_buf_t buf = { .base = check.data, .len = (unsigned long)len };
        PROP(&e, duv_pipe_write(&ts->wreq, &ts->pipe, &buf, 1, ts_write_cb) );
        ts->exp_check_cbs++;
    }

    if(ts->plan == GET_READ_STOP){
        TFINISH(ts->read_stop_recv_1){
            // wait for a complete line
            if(!dstr_contains(ts->rbuf, DSTR_LIT("\n"))) return e;
            DSTR_STATIC(exp,
                "{"
                    "\"status\":\"error\","
                    "\"reason\":\"unrecognized command\""
                "}\n"
            );
            EXPECT_D3(&e, "get-read-stop response 1", ts->rbuf, exp);
            ts->rbuf.len = 0;
        }
        TFINISH(ts->read_stop_recv_2){
            // wait for a complete line
            if(!dstr_contains(ts->rbuf, DSTR_LIT("\n"))) return e;
            DSTR_STATIC(exp,
                "{"
                    "\"status\":\"error\","
                    "\"reason\":\"invalid json\""
                "}\n"
            );
            EXPECT_D3(&e, "get-read-stop response 2", ts->rbuf, exp);
            ts->rbuf.len = 0;
        }
    }

    // send an update
    TONCE(ts->update_triggered){
        status_server_update(
            &ts->ss,
            STATUS_MAJ_TLS_FIRST,
            STATUS_MIN_CREATE_ACCOUNT,
            DSTR_LIT("yo.com")
        );
    }
    TFINISH(ts->update_recv){
        // wait for a complete line
        if(!dstr_contains(ts->rbuf, DSTR_LIT("\n"))) return e;
        DSTR_VAR(exp, 4096);
        PROP(&e,
            FMT(&exp,
                "{"
                    "\"major\":\"%x\","
                    "\"minor\":\"%x\","
                    "\"fulldomain\":\"yo.com\","
                    "\"configured\":\"yes\","
                    "\"tls_ready\":\"no\""
                "}\n",
                FD(status_maj_dstr(STATUS_MAJ_TLS_FIRST)),
                FD(status_min_dstr(STATUS_MIN_CREATE_ACCOUNT))
            )
        );
        EXPECT_D3(&e, "initial read", ts->rbuf, exp);
        ts->rbuf.len = 0;
        if(ts->plan == QUIT_AFTER_INITIAL){
            advancer_up_done(&ts->advancer);
            return e;
        }
    }

    advancer_up_done(&ts->advancer);

    return e;
}

static void test_server_advance_down(advancer_t *advancer, derr_t *E){
    test_server_t *ts = CONTAINER_OF(advancer, test_server_t, advancer);
    (void)E;

    if(duv_pipe_close2(&ts->pipe, ts_handle_close_cb)){
        return;
    }

    if(status_server_close(&ts->ss)){
        ts->exp_ss_done = true;
        return;
    }

    advancer_down_done(&ts->advancer);
}

static derr_t test_status_server(void){
    derr_t e = E_OK;

    DSTR_VAR(temp, 256);
    string_builder_t sock;
    #ifdef _WIN32
    // windows
    DSTR_VAR(rando, 9);
    PROP(&e, random_bytes(&rando, 9) );
    PROP(&e, FMT(&temp, "\\\\.\\pipe\\test-status-server-%x", FB64D(rando)) );
    sock = SBD(temp);
    #else
    // unix
    PROP(&e, mkdir_temp("test-status-server", &temp) );
    sock = sb_append(&SBD(temp), SBS("sock"));
    #endif

    for(plan_e plan = 0; plan < NUM_PLANS; plan++){
        if(plan) LOG_INFO("---\n");
        LOG_INFO("plan = %x\n", FS(plan_name[plan]));
        test_server_t ts = { .plan = plan, .sock = sock };
        ts.wreq.data = &ts;
        DSTR_WRAP_ARRAY(ts.rbuf, ts._rbuf);
        advancer_prep(
            &ts.advancer,
            &ts.root.scheduler.iface,
            test_server_advance_up,
            test_server_advance_down
        );
        PROP_GO(&e, duv_root_run(&ts.root, &ts.advancer), cu);
    }

cu:
    #ifndef _WIN32
    // unix
    DROP_CMD( rm_rf(temp.data) );
    #endif

    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;
    int exit_code = 1;

    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    PROP_GO(&e, test_status_server(), cu);

    exit_code = 0;

cu:
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
    }

    LOG_ERROR(exit_code ? "FAIL\n" : "PASS\n");
    return exit_code;
}
