#include "libdstr/libdstr.h"
#include "libduv/libduv.h"

#include "test/test_utils.h"

static derr_t E = {0};
static uv_loop_t loop;
static uv_tcp_t listener;
static bool connected = false;
static bool accepted = false;
static bool listener_closed = false;
static uv_tcp_t peer;
static duv_connect_t connector;
static uv_tcp_t tcp;
static uv_async_t async;
static bool finishing = false;
static bool success = false;

// PHASES
// 1 = connect successfully
// 2 = fail to connect (nobody listening)
// 3 = fail to connect (gai fails)
// 4 = cancel a connection mid-gai
// 5 = cancel a connection mid-connect

static void noop_close_cb(uv_handle_t *handle){
    (void)handle;
}

static void finish(void){
    if(finishing) return;
    finishing = true;
    if(!listener_closed) duv_tcp_close(&listener, noop_close_cb);
    duv_async_close(&async, noop_close_cb);
    duv_connect_cancel(&connector);
}

static void on_connect_phase_5(duv_connect_t *c, derr_t e){
    (void)c;
    if(is_error(e)){
        if(e.type != E_CANCELED){
            PROP_VAR_GO(&E, &e, done);
        }
    }else{
        duv_tcp_close(&tcp, noop_close_cb);
        ORIG_GO(&E, E_VALUE, "on_connect phase 5 not canceled", done);
    }

    // clean up our test hook
    _connect_started_hook = NULL;

    // no more phases
    success = true;

done:
    finish();
}

static void connect_started_hook(void){
    // cancel the connect in the next loop iteration
    uv_async_send(&async);
}

static void on_connect_phase_4(duv_connect_t *c, derr_t e){
    (void)c;
    if(is_error(e)){
        if(e.type != E_CANCELED){
            PROP_VAR_GO(&E, &e, fail);
        }
    }else{
        // no error: tcp must be connected
        duv_tcp_close(&tcp, noop_close_cb);
        ORIG_GO(&E, E_VALUE, "on_connect phase 4 not canceled", fail);
    }

    // BEGIN PHASE 5: "cancel a connection mid-connect"
    PROP_GO(&E,
        duv_connect(
            &loop,
            &tcp,
            0,
            &connector,
            on_connect_phase_5,
            "localhost",
            "51429",
            NULL
        ),
    fail);

    // phase 5 is triggered by a hook in the duv_connect logic
    _connect_started_hook = connect_started_hook;

    return;

fail:
    finish();
}

static void on_connect_phase_3(duv_connect_t *c, derr_t e){
    (void)c;
    if(!is_error(e)){
        duv_tcp_close(&tcp, noop_close_cb);
        TRACE(&E, "duv_connect did not error\n");
        ORIG_GO(&E, E_VALUE, "on_connect phase 3 fail", fail);
    }
    DROP_VAR(&e);

    // BEGIN PHASE 4: "cancel a connection mid-gai"
    PROP_GO(&E,
        duv_connect(
            &loop,
            &tcp,
            0,
            &connector,
            on_connect_phase_4,
            "localhost",
            "51429",
            NULL
        ),
    fail);
    // schedule a callback to cancel
    uv_async_send(&async);
    return;

fail:
    finish();
}

static void on_connect_phase_2(duv_connect_t *c, derr_t e){
    (void)c;
    if(!is_error(e)){
        duv_tcp_close(&tcp, noop_close_cb);
        TRACE(&E, "duv_connect did not error\n");
        ORIG_GO(&E, E_VALUE, "on_connect phase 2 fail", fail);
    }
    DROP_VAR(&e);

    // BEGIN PHASE 3: "fail to connect (gai fails)"
    PROP_GO(&E,
        duv_connect(
            &loop,
            &tcp,
            0,
            &connector,
            on_connect_phase_3,
            "notarealdomain.notarealtld",
            "51429",
            NULL
        ),
    fail);
    return;

fail:
    // close tcp as well as everything else
    finish();
}

static void phase_1_listener_closed(uv_handle_t *handle){
    (void)handle;
    // BEGIN PHASE 2: "fail to connect (nobody listening)"
    PROP_GO(&E,
        duv_connect(
            &loop,
            &tcp,
            0,
            &connector,
            on_connect_phase_2,
            "localhost",
            "51429",
            NULL
        ),
    fail);
    return;

fail:
    finish();
}

static void phase_1_tcp_closed(uv_handle_t *handle){
    (void)handle;
    duv_tcp_close(&listener, phase_1_listener_closed);
    listener_closed = true;
}

static void end_phase_1(void){
    duv_tcp_close(&peer, noop_close_cb);
    duv_tcp_close(&tcp, phase_1_tcp_closed);
}

static void on_connect_phase_1(duv_connect_t *c, derr_t e){
    (void)c;
    if(is_error(e)){
        TRACE(&E, "on_connect_phase_1 errored\n");
        PROP_VAR_GO(&E, &e, fail);
    }

    /* linux seems to always accept before connection is done, but windows and
       mac do not, so we accept either order */
    connected = true;
    if(accepted){
        end_phase_1();
    }
    return;

fail:
    finish();
}

static void on_listener(uv_stream_t *server, int status){
    (void)server;
    if(status < 0){
        TRACE(&E, "on_listener: %x\n", FUV(&status));
        ORIG_GO(&E, uv_err_type(status), "on_listener error", fail);
    }

    PROP_GO(&E, duv_tcp_init(&loop, &peer), fail);

    // accept peer connection
    PROP_GO(&E, duv_tcp_accept(&listener, &peer), fail_peer);

    accepted = true;
    if(connected){
        end_phase_1();
    }

    return;

fail_peer:
    duv_tcp_close(&peer, noop_close_cb);
fail:
    finish();
}

static void on_async(uv_async_t *handle){
    (void)handle;
    duv_connect_cancel(&connector);
}

static derr_t test_connect(void){
    derr_t e = E_OK;

    struct sockaddr_storage ss;
    PROP(&e, read_addr(&ss, "127.0.0.1", 51429) );

    PROP(&e, duv_loop_init(&loop) );

    PROP_GO(&e, duv_tcp_init(&loop, &listener), fail_loop);

    PROP_GO(&e, duv_tcp_binds(&listener, &ss, 0), fail_listener);

    PROP_GO(&e, duv_tcp_listen(&listener, 1, on_listener), fail_listener);

    PROP_GO(&e, duv_async_init(&loop, &async, on_async), fail_listener);

    // BEGIN PHASE 1: "connect successfully"
    PROP_GO(&e,
        duv_connect(
            &loop,
            &tcp,
            0,
            &connector,
            on_connect_phase_1,
            "localhost",
            "51429",
            NULL
        ),
    fail_async);

    PROP_GO(&e, duv_run(&loop), done);

done:
    MERGE_VAR(&e, &E, "from loop");

    if(!is_error(e) && !success){
        ORIG(&e, E_INTERNAL, "no error, but was not successful");
    }

    return e;

fail_async:
    duv_async_close(&async, noop_close_cb);
fail_listener:
    duv_tcp_close(&listener, noop_close_cb);
fail_loop:
    DROP_CMD( duv_run(&loop) );
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_connect(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
