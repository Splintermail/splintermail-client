// duv_root_t is mostly just the combination of a uv_loop_t and a scheduler_i

// it helps you transition from `int main()` into advance_state

typedef struct {
    uv_loop_t loop;
    duv_scheduler_t scheduler;
    schedulable_t schedulable;
} duv_root_t;

// starts loop and scheduler, advances to completion, propagates advancer.e
derr_t duv_root_run(duv_root_t *root, advancer_t *advancer);
