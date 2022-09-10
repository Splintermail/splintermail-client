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
    // if it was in the list, remove it
    link_remove(&x->link);
    // place it at the end of the list
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

void duv_scheduler_close(duv_scheduler_t *s){
    if(s->closed) return;
    s->closed = true;
    uv_loop_t *loop = s->timer.loop;
    if(uv_loop_alive(loop)){
        LOG_FATAL(
            "duv_scheduler_close() must not be called while loop is active\n"
        );
    }
    // close the timer
    duv_timer_close(&s->timer, close_cb);
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

void manual_scheduler_prep(manual_scheduler_t *s){
    *s = (manual_scheduler_t){
        .iface = { .schedule = manual_schedule },
    };
    link_init(&s->scheduled);
}

void manual_scheduler_run(manual_scheduler_t *s){
    drain(&s->scheduled);
}
