#include "libduv/libduv.h"

// starts loop and scheduler, advances to completion, propagates advancer.e
derr_t duv_root_run(duv_root_t *root, advancer_t *advancer){
    derr_t e = E_OK;

    bool loop = false;
    bool scheduler = false;

    PROP_GO(&e, duv_loop_init(&root->loop), cu);
    loop = true;

    PROP_GO(&e, duv_scheduler_init(&root->scheduler, &root->loop), cu);
    scheduler = true;

    // run the advancer to completion
    advancer_schedule(advancer, E_OK);
    uv_run(&root->loop, UV_RUN_DEFAULT);
    if(!advancer->down_done){
        // this is always a bug, and we can't pretend we know how to clean up
        LOG_FATAL("advancer fell out of loop without finishing\n");
    }

    // loop run was successful; return the advancer's error
    e = advancer->e;
    advancer->e = (derr_t){0};

cu:
    if(scheduler) duv_scheduler_close(&root->scheduler);
    if(loop) uv_loop_close(&root->loop);

    return e;
}
