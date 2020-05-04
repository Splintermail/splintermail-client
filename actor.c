#include "loop.h"
#include "uv_util.h"
#include "refs.h"
#include "actor.h"


static void actor_advance_onthread(actor_t *actor);


// actor_async_cb is a uv_async_cb
static void actor_async_cb(uv_async_t *async){
    async_spec_t *spec = async->data;
    actor_t *actor = CONTAINER_OF(spec, actor_t, advance_spec);

    actor_advance_onthread(actor);
}


// actor_async_close_cb is a async_spec_t->close_cb
static void actor_async_close_cb(async_spec_t *spec){
    actor_t *actor = CONTAINER_OF(spec, actor_t, advance_spec);

    // free resources from the actor_t
    refs_free(&actor->refs);

    // shutdown, part three of three: report ourselves as dead
    actor->iface.dead_onthread(actor);
}


// work_cb is a uv_work_cb
static void work_cb(uv_work_t *req){
    actor_t *actor = CONTAINER_OF(req, actor_t, uv_work);

    derr_t e = E_OK;
    IF_PROP(&e, actor->iface.do_work(actor) ){
        actor->iface.failure(actor, e);
        PASSED(e);
    }
}


// after_work_cb is a uv_after_work_cb
static void after_work_cb(uv_work_t *req, int status){
    actor_t *actor = CONTAINER_OF(req, actor_t, uv_work);

    // throw an error if the thread was canceled
    if(status < 0){
        derr_t e = E_OK;
        // close actor with error
        TRACE(&e, "uv work request failed: %x\n", FUV(&status));
        TRACE_ORIG(&e, uv_err_type(status), "work request failed");
        actor->iface.failure(actor, e);
        PASSED(e);
    }

    // we are no longer executing
    actor->executing = false;

    // check if we need to re-enqueue ourselves
    actor_advance_onthread(actor);
}


static void actor_advance_onthread(actor_t *actor){
    // if a worker is executing, do nothing; we'll check again in work_done()
    if(actor->executing) return;

    // shutdown, part two of three: close the async
    if(actor->dead){
        uv_async_close(&actor->advance_async, async_handle_close_cb);
        return;
    }

    if(actor->closed){
        if(!actor->closed_onthread){
            actor->closed_onthread = true;
            actor->iface.close_onthread(actor);
        }
        return;
    }

    // is there more work to do?
    if(!actor->iface.more_work(actor)) return;

    // try to enqueue work
    int ret = uv_queue_work(actor->uv_loop, &actor->uv_work, work_cb,
            after_work_cb);
    if(ret < 0){
        // capture error
        derr_t e = E_OK;
        TRACE(&e, "uv_queue_work: %x\n", FUV(&ret));
        TRACE_ORIG(&e, uv_err_type(ret), "failed to enqueue work");

        actor->iface.failure(actor, e);
        PASSED(e);

        return;
    }

    // work is enqueued
    actor->executing = true;
}


// actor_finalize is a finalizer_t
static void actor_finalize(refs_t *refs){
    actor_t *actor = CONTAINER_OF(refs, actor_t, refs);

    // shutdown, part one of three: make the final call to actor_advance
    /* since actor_dead() is required to be the last call to the actor, this
       actor_advance() will be the last call to the uv_async_t. Therefore, even
       if right now there are any pending calls to the uv_async_t, we are
       guaranteed to have none in flight after one more trip through the loop.
       Then we will call uv_close(). */
    actor->dead = true;
    actor_advance(actor);
}


derr_t actor_init(actor_t *actor, uv_loop_t *uv_loop, actor_i iface){
    derr_t e = E_OK;

    // start by zeroizing everything and storing static values
    *actor = (actor_t){
        .iface = iface,
        .uv_loop = uv_loop,
    };

    PROP(&e, refs_init(&actor->refs, 1, actor_finalize) );

    actor->advance_spec = (async_spec_t){
        .close_cb = actor_async_close_cb,
    };

    actor->advance_async.data = &actor->advance_spec;
    int ret = uv_async_init(uv_loop, &actor->advance_async, actor_async_cb);
    if(ret < 0){
        TRACE(&e, "uv_async_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing advance_async",
                fail_refs);
    }

    return e;

fail_refs:
    refs_free(&actor->refs);
    return e;
}


// callable from any thread
void actor_advance(actor_t *actor){
    // alert the loop thread to try to enqueue the worker
    int ret = uv_async_send(&actor->advance_async);
    if(ret < 0){
        /* ret != 0 is only possible under some specific circumstances:
             - if the async handle is not an async type (should never happen)
             - if uv_close was called on the async handle (should never happen
               because we don't close uv_handles until all imap_sessions have
               closed, so there won't be anyone to call this function)

           Therefore, it is safe to not "properly" handle this error.  But, we
           will at least log it since we are relying on undocumented behavior.
        */
        LOG_ERROR("uv_async_send: %x\n", FUV(&ret));
        LOG_ERROR("uv_async_send should never fail!\n");
    }
}


/* call this one time and it will result in a call to .close_onthread(),
   after which there won't be any futher calls to .do_work(), .more_work(), or
   .failure().  You may skip this call in the special case mentioned for
   actor_init(). */
void actor_close(actor_t *actor){
    actor->closed = true;
    actor_advance(actor);
}


void actor_ref_up(actor_t *actor){
    ref_up(&actor->refs);
}

void actor_ref_dn(actor_t *actor){
    ref_dn(&actor->refs);
}
