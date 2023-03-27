#include <stdlib.h>

#include "libduv/libduv.h"


void schedulable_prep(schedulable_t *s, scheduler_schedule_cb cb){
    *s = (schedulable_t){ .cb = cb };
    link_init(&s->link);
}

void schedulable_cancel(schedulable_t *s){
    link_remove(&s->link);
}

// invoke scheduled items repeatedly until there's none left
static void drain(link_t *scheduled){
    link_t *link;
    while((link = link_list_pop_first(scheduled))){
        schedulable_t *x = CONTAINER_OF(link, schedulable_t, link);
        x->cb(x);
    }
}

static void schedule(link_t *scheduled, schedulable_t *x){
    // if we're already scheduled, do nothing
    if(!link_list_isempty(&x->link)) return;
    // otherwise place it at the end of the list
    link_list_append(scheduled, &x->link);
}

static void timer_cb(uv_timer_t *timer){
    duv_scheduler_t *s = CONTAINER_OF(timer, duv_scheduler_t, timer);

    drain(&s->scheduled);

    // scheduled is now empty; anything added after this needs to wake us up
    s->timer_set = false;
}

static void duv_scheduler_set_timer(duv_scheduler_t *s){
    if(s->timer_set) return;

    // schedule our own wakeup
    int ret = uv_timer_start(&s->timer, timer_cb, 0, 0);
    if(ret < 0){
        // should only happen if we called close, which we prevent
        LOG_FATAL("uv_timer_start: %x\n", FUV(&ret));
    }
    s->timer_set = true;
}

static void duv_schedule(scheduler_i *iface, schedulable_t *x){
    duv_scheduler_t *s = CONTAINER_OF(iface, duv_scheduler_t, iface);
    if(s->closed){
        // illegal calling behavior
        LOG_FATAL("schedule() called on closed scheduler\n");
    }
    schedule(&s->scheduled, x);
    duv_scheduler_set_timer(s);
}

derr_t duv_scheduler_init(duv_scheduler_t *s, uv_loop_t *loop){
    derr_t e = E_OK;

    *s = (duv_scheduler_t){
        .iface = { .schedule = duv_schedule },
    };
    link_init(&s->scheduled);
    int ret = uv_timer_init(loop, &s->timer);
    if(ret < 0){
        ORIG(&e, uv_err_type(ret), "uv_timer_init: %x", FUV(&ret));
    }
    s->timer.data = s;

    return e;
}

static void close_cb(uv_handle_t *handle){
    (void)handle;
}

static void detect_unclosed_handles(uv_handle_t *handle, void *data){
    bool *ok = data;
    if(uv_is_closing(handle)) return;
    switch(handle->type){
        case UV_UNKNOWN_HANDLE: LOG_ERROR("UNKNOWN_HANDLE\n"); break;
        case UV_ASYNC: LOG_ERROR("ASYNC\n"); break;
        case UV_CHECK: LOG_ERROR("CHECK\n"); break;
        case UV_FS_EVENT: LOG_ERROR("FS_EVENT\n"); break;
        case UV_FS_POLL: LOG_ERROR("FS_POLL\n"); break;
        case UV_HANDLE: LOG_ERROR("HANDLE\n"); break;
        case UV_IDLE: LOG_ERROR("IDLE\n"); break;
        case UV_NAMED_PIPE: LOG_ERROR("NAMED_PIPE\n"); break;
        case UV_POLL: LOG_ERROR("POLL\n"); break;
        case UV_PREPARE: LOG_ERROR("PREPARE\n"); break;
        case UV_PROCESS: LOG_ERROR("PROCESS\n"); break;
        case UV_STREAM: LOG_ERROR("STREAM\n"); break;
        case UV_TCP: LOG_ERROR("TCP\n"); break;
        case UV_TIMER: LOG_ERROR("TIMER\n"); break;
        case UV_TTY: LOG_ERROR("TTY\n"); break;
        case UV_UDP: LOG_ERROR("UDP\n"); break;
        case UV_SIGNAL: LOG_ERROR("SIGNAL\n"); break;
        case UV_FILE: LOG_ERROR("FILE\n"); break;
        case UV_HANDLE_TYPE_MAX: LOG_ERROR("HANDLE_TYPE_MAX\n"); break;
    }
    *ok = false;
}

void duv_scheduler_close(duv_scheduler_t *s){
    if(s->closed) return;
    s->closed = true;
    // close the timer
    duv_timer_close(&s->timer, close_cb);
    // make sure loop will not run forever
    uv_loop_t *loop = s->timer.loop;
    bool ok = true;
    uv_walk(loop, detect_unclosed_handles, &ok);
    if(!ok){
        LOG_FATAL(
            "duv_scheduler_close() must not be called "
            "while loop has unclosed handles\n"
        );
    }
    // run the loop to drain the close_cb
    uv_run(loop, UV_RUN_DEFAULT);
}

// invoke scheduled items repeatedly until there's none left
void duv_scheduler_run(duv_scheduler_t *s){
    if(s->timer_set){
        // we can cancel that wakeup
        int ret = uv_timer_stop(&s->timer);
        if(ret < 0){
            // should literally never happen
            LOG_FATAL("uv_timer_stop: %x\n", FUV(&ret));
        }
    }

    // disable uv_timer_t calls
    s->timer_set = true;

    drain(&s->scheduled);

    s->timer_set = false;
}

static void manual_schedule(scheduler_i *iface, schedulable_t *x){
    manual_scheduler_t *s = CONTAINER_OF(iface, manual_scheduler_t, iface);
    schedule(&s->scheduled, x);
}

scheduler_i *manual_scheduler(manual_scheduler_t *s){
    *s = (manual_scheduler_t){
        .iface = { .schedule = manual_schedule },
    };
    link_init(&s->scheduled);
    return &s->iface;
}

void manual_scheduler_run(manual_scheduler_t *s){
    drain(&s->scheduled);
}
