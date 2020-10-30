#include <stdio.h>
#include <stdlib.h>
#include "uv_util.h"
#include "libdstr/libdstr.h"

typedef struct {
    uv_loop_t loop;
    uv_async_t wakeup;
    uv_timer_t timer;
    uv_udp_t udp;
} globals_t;

static void close_loop(globals_t *g){
    uv_udp_close(&g->udp, NULL);
    uv_timer_close(&g->timer, NULL);
    uv_async_close(&g->wakeup, NULL);
    uv_loop_close(&g->loop);
}

static void do_work_timer(uv_timer_t *handle);

static void do_work(globals_t *g){
    derr_t e = E_OK;

    printf("hi!\n");

    int ret = uv_timer_stop(&g->timer);
    if(ret < 0){
        TRACE(&e, "uv_timer_stop: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error stopping timer", fail);
    }

    ret = uv_timer_start(&g->timer, do_work_timer, 5000, 0);
    if(ret < 0){
        TRACE(&e, "uv_timer_start: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error starting timer", fail);
    }

fail:
    DUMP(e);
    DROP_VAR(&e);
}

static void do_work_async(uv_async_t *handle){
    globals_t *g = handle->loop->data;
    do_work(g);
}

static void do_work_timer(uv_timer_t *handle){
    globals_t *g = handle->loop->data;
    do_work(g);
}

static void do_work_udp(
    uv_udp_t *handle,
    ssize_t nread,
    const uv_buf_t* buf,
    const struct sockaddr* addr,
    unsigned flags
){
    (void) flags;
    (void) buf;
    globals_t *g = handle->loop->data;
    // check for socket errors
    if(nread < 0 || (nread < 0 && addr == NULL)){
        close_loop(g);
        return;
    }

    do_work(g);
}

static void udp_alloc(uv_handle_t *handle, size_t suggest, uv_buf_t *buf){
    (void)suggest;
    (void)handle;

    // give out the same buffer over and over, we never read it.
    static char dummy_buffer[4096];

    buf->base = dummy_buffer;
    buf->len = sizeof(dummy_buffer);

    return;
}

static derr_t keysync_service(void){
    derr_t e = E_OK;

    globals_t g;

    int ret = uv_loop_init(&g.loop);
    if(ret < 0){
        TRACE(&e, "uv_loop_init: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "error initializing loop");
    }
    g.loop.data = &g;

    // init the wakeup async
    ret = uv_async_init(&g.loop, &g.wakeup, do_work_async);
    if(ret < 0){
        TRACE(&e, "uv_async_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing async", fail_loop);
    }

    // init the timer
    ret = uv_timer_init(&g.loop, &g.timer);
    if(ret < 0){
        TRACE(&e, "uv_timer_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing timer", fail_async);
    }

    // start the timer
    ret = uv_timer_start(&g.timer, do_work_timer, 5000, 0);
    if(ret < 0){
        TRACE(&e, "uv_timer_start: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error starting timer", fail_timer);
    }

    // init the udp handle
    ret = uv_udp_init(&g.loop, &g.udp);
    if(ret < 0){
        TRACE(&e, "uv_udp_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing udp", fail_timer);
    }

    // bind to 127.0.0.1:8764
    const char *addr = "127.0.0.1";
    unsigned short port = 8764;
    struct sockaddr_in sai = {0};
    sai.sin_family = AF_INET;
    sai.sin_addr.s_addr = inet_addr(addr);
    sai.sin_port = htons(port);
    ret = uv_udp_bind_sockaddr_in(&g.udp, &sai, UV_UDP_REUSEADDR);
    if(ret < 0){
        TRACE(&e, "uv_udp_bind: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error binding udp", fail_udp);
    }

    ret = uv_udp_recv_start(&g.udp, udp_alloc, do_work_udp);
    if(ret < 0){
        TRACE(&e, "uv_udp_bind: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error binding udp", fail_udp);
    }

fail_udp:
    if(is_error(e))  uv_udp_close(&g.udp, NULL);
fail_timer:
    if(is_error(e))  uv_timer_close(&g.timer, NULL);
fail_async:
    if(is_error(e))  uv_async_close(&g.wakeup, NULL);
fail_loop:
    if(is_error(e)) uv_loop_close(&g.loop);

    ret = uv_run(&g.loop, UV_RUN_DEFAULT);
    if(ret < 0){
        TRACE(&e, "uv_run: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "uv_run error");
    }

    return e;
}


int main(void){
    derr_t e = E_OK;

    PROP_GO(&e, keysync_service(), fail);
    return 0;

fail:
    DUMP(e);
    DROP_VAR(&e);
    return 1;
}
