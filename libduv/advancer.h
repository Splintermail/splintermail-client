// advancer_t is a helper for writing the advance_state pattern.

// it automates scheduling and idempotent closing

struct advancer_t;
typedef struct advancer_t advancer_t;

struct advancer_t {
    scheduler_i *scheduler;
    schedulable_t schedulable;
    // advance_state errors will automatically propagate to here
    derr_t e;
    derr_t (*advance_up)(advancer_t*);
    /* as a convenience, the derr_t* points to advancer.e, since it is expected
       that advance_down will deal with e */
    void (*advance_down)(advancer_t*, derr_t*);
    // up_done means advance_up successfully finished whatever work it had
    bool up_done;
    // down_done means we will not let ourselves be scheduled again
    bool down_done;
};

void advancer_prep(
    advancer_t *a,
    scheduler_i *scheduler,
    derr_t (*advance_up)(advancer_t*),
    void (*advance_down)(advancer_t*, derr_t*)
);

// schedule again, possibly with a new error to report
// also logs errors passed in based on where advancer_scheudule() was called
void _advancer_schedule(
    advancer_t *a, derr_t e, const char *file, const char *func, int line
);
#define advancer_schedule(a, e) _advancer_schedule(a, e, FILE_LOC)

// call when you're done with advance_up and want to advance_down
void advancer_up_done(advancer_t *a);

// call once when you're all done
void advancer_down_done(advancer_t *a);

// ONCE will enter a block exactly once, based on the provided flag
#define ONCE(x) if(!x && (x = true))
// FINISH will finish a block exactly once, based on the provided flag
#define FINISH(x) for(; !x; x = true)

// TONCE and TFINISH are for for tests; they just logs what they're doing
#define TONCE(x) if(!x && (x = true && !LOG_INFO("ONCE("#x")\n")))
#define TFINISH(x) \
    for(; !x && !LOG_INFO("FINISH("#x")\n"); x = !LOG_INFO("FINISHED("#x")\n"))
