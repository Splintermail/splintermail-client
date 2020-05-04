struct actor_t;
typedef struct actor_t actor_t;

// function calls to provide to an actor
typedef struct {
    // is there more work to do?  (read-only, no other thread safety required)
    bool (*more_work)(actor_t*);
    // do some non-zero amount of work, no thread safety required
    derr_t (*do_work)(actor_t*);

    /* may be called many times, on- or off-thread, including when:
        - work fails to be enqueued
        - do_work() returns an error
        - after_work_cb() gets a failure status from libuv */
    void (*failure)(actor_t *, derr_t);

    // will be called one time, after actor_close() is called
    // not called if refs reaches zero without a call to actor_close()
    void (*close_onthread)(actor_t*);
    // will be called exactly one time, after the refcount reaches 0
    void (*dead_onthread)(actor_t*);
} actor_i;

// state of an actor
struct actor_t {
    actor_i iface;

    // use a refs_t to track when it is safe to close the actor
    refs_t refs;

    // every fetcher_t has only one uv_work_t, so it's single threaded
    uv_work_t uv_work;
    uv_loop_t *uv_loop;
    bool executing;
    bool closed;
    bool closed_onthread;
    bool dead;

    // we need an async to be able to call advance from any thread
    uv_async_t advance_async;
    async_spec_t advance_spec;
};
DEF_CONTAINER_OF(actor_t, advance_spec, async_spec_t);
DEF_CONTAINER_OF(actor_t, uv_work, uv_work_t);
DEF_CONTAINER_OF(actor_t, refs, refs_t);

/* during error handling, after actor_init(), but before any calls to
   actor_advance(), it is safe to simply downref the actor and wait for the
   call to dead_onthread() */
derr_t actor_init(actor_t *actor, uv_loop_t *uv_loop, actor_i iface);

// callable from any thread
void actor_advance(actor_t *actor);

/* call this one time and it will result in a call to .close_onthread(),
   after which there won't be any futher calls to .do_work(), .more_work(), or
   .failure().  You may skip this call in the special case mentioned for
   actor_init(). */
void actor_close(actor_t *actor);

void actor_ref_up(actor_t *actor);
void actor_ref_dn(actor_t *actor);
