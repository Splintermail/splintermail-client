/* scheduler_i: an interface for scheduling work "sometime later"

   Useful for "fast" callbacks.

   In a libuv loop, the scheduler itself sets a zero-length timer, and
   continually invokes actions in its queue until there are none left, all
   without letting libuv make syscalls to check for IO. */

struct scheduler_i;
typedef struct scheduler_i scheduler_i;

struct schedulable_t;
typedef struct schedulable_t schedulable_t;

struct duv_scheduler_t;
typedef struct duv_scheduler_t duv_scheduler_t;

typedef void (*scheduler_schedule_cb)(schedulable_t*);

struct schedulable_t {
    link_t link;
    scheduler_schedule_cb cb;
};
DEF_CONTAINER_OF(schedulable_t, link, link_t)

void schedulable_prep(schedulable_t *s, scheduler_schedule_cb cb);

void schedulable_cancel(schedulable_t *s);

struct scheduler_i {
    void (*schedule)(scheduler_i*, schedulable_t*);
};

struct duv_scheduler_t {
    scheduler_i iface;
    uv_timer_t timer;
    bool timer_set;
    link_t scheduled;  // schedulable_t->link
    bool closed;
};
DEF_CONTAINER_OF(duv_scheduler_t, iface, scheduler_i)
DEF_CONTAINER_OF(duv_scheduler_t, timer, uv_timer_t)

derr_t duv_scheduler_init(duv_scheduler_t *s, uv_loop_t *loop);

/* watch out! scheduler_close() must be called outside of uv_run(), and it will
   call uv_close() on its timer and uv_run() to finish cleaning up resources */
void duv_scheduler_close(duv_scheduler_t *s);

// run manually, rather than automatically on the timer
/* mostly useful for passthru_t which has to convert from libuv semantics of
   alloc_cb/read_cb/read_stop/read_start to libduv semantics of read() without
   extraneous read_stop/read_start calls */
void duv_scheduler_run(duv_scheduler_t *s);

/* a libuv-free implementation of the scheduler_i, which relies on somebody
   manually calling manual_scheduler_run() periodically */
typedef struct {
    scheduler_i iface;
    link_t scheduled;
} manual_scheduler_t;
DEF_CONTAINER_OF(manual_scheduler_t, iface, scheduler_i)

scheduler_i *manual_scheduler(manual_scheduler_t *s);
void manual_scheduler_run(manual_scheduler_t *s);
