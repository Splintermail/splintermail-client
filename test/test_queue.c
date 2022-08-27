#include <libdstr/libdstr.h>
#include <libengine/libengine.h>

#include "test_utils.h"

#define NELEMS 10

typedef struct {
    size_t id;
    link_t link;
    queue_cb_t qcb;
} qtest_t;
DEF_CONTAINER_OF(qtest_t, link, link_t)

static derr_t test_queue_simple(void){
    derr_t e = E_OK;
    // create a list of qtest_t's
    qtest_t qlist[NELEMS];
    for(size_t i = 0; i < NELEMS; i++){
        qlist[i].id = i;
        link_init(&qlist[i].link);
        queue_cb_prep(&qlist[i].qcb);
    }

    queue_t q;
    PROP(&e, queue_init(&q) );
    link_t *resp;

    // check append/pop_first
    for(size_t i = 0; i < NELEMS; i++)
        queue_append(&q, &qlist[i].link);
    if(q.len != NELEMS) ORIG_GO(&e, E_VALUE, "wrong len of queue", cu_q);
    for(size_t i = 0; i < NELEMS; i++){
        resp = queue_pop_first(&q, false);
        if(resp == NULL)
            ORIG_GO(&e, E_VALUE, "unexpected null\n", cu_q);
        if(resp != &qlist[i].link)
            ORIG_GO(&e, E_VALUE, "mismatched value\n", cu_q);
    }
    if(q.len != 0) ORIG_GO(&e, E_VALUE, "wrong len of queue", cu_q);
    resp = queue_pop_first(&q, false);
    if(resp != NULL)
        ORIG_GO(&e, E_VALUE, "unexpected non-null\n", cu_q);
    if(q.len != 0) ORIG_GO(&e, E_VALUE, "wrong len of queue", cu_q);

    // check prepend/pop_first
    for(size_t i = 0; i < NELEMS; i++)
        queue_prepend(&q, &qlist[i].link);
    if(q.len != NELEMS) ORIG_GO(&e, E_VALUE, "wrong len of queue", cu_q);
    for(size_t i = NELEMS-1; i < NELEMS; i--){
        resp = queue_pop_first(&q, false);
        if(resp == NULL)
            ORIG_GO(&e, E_VALUE, "unexpected null\n", cu_q);
        if(resp != &qlist[i].link)
            ORIG_GO(&e, E_VALUE, "mismatched value\n", cu_q);
    }
    if(q.len != 0) ORIG_GO(&e, E_VALUE, "wrong len of queue", cu_q);
    resp = queue_pop_first(&q, false);
    if(resp != NULL)
        ORIG_GO(&e, E_VALUE, "unexpected non-null\n", cu_q);
    if(q.len != 0) ORIG_GO(&e, E_VALUE, "wrong len of queue", cu_q);

    // check prepend/pop_last
    for(size_t i = 0; i < NELEMS; i++)
        queue_prepend(&q, &qlist[i].link);
    if(q.len != NELEMS) ORIG_GO(&e, E_VALUE, "wrong len of queue", cu_q);
    for(size_t i = 0; i < NELEMS; i++){
        resp = queue_pop_last(&q, false);
        if(resp == NULL)
            ORIG_GO(&e, E_VALUE, "unexpected null\n", cu_q);
        if(resp != &qlist[i].link)
            ORIG_GO(&e, E_VALUE, "mismatched value\n", cu_q);
    }
    if(q.len != 0) ORIG_GO(&e, E_VALUE, "wrong len of queue", cu_q);
    resp = queue_pop_first(&q, false);
    if(resp != NULL)
        ORIG_GO(&e, E_VALUE, "unexpected non-null\n", cu_q);
    if(q.len != 0) ORIG_GO(&e, E_VALUE, "wrong len of queue", cu_q);

    // check append/pop_last
    for(size_t i = 0; i < NELEMS; i++)
        queue_append(&q, &qlist[i].link);
    if(q.len != NELEMS) ORIG_GO(&e, E_VALUE, "wrong len of queue", cu_q);
    for(size_t i = NELEMS-1; i < NELEMS; i--){
        resp = queue_pop_last(&q, false);
        if(resp == NULL)
            ORIG_GO(&e, E_VALUE, "unexpected null\n", cu_q);
        if(resp != &qlist[i].link)
            ORIG_GO(&e, E_VALUE, "mismatched value\n", cu_q);
    }
    if(q.len != 0) ORIG_GO(&e, E_VALUE, "wrong len of queue", cu_q);
    resp = queue_pop_first(&q, false);
    if(resp != NULL)
        ORIG_GO(&e, E_VALUE, "unexpected non-null\n", cu_q);
    if(q.len != 0) ORIG_GO(&e, E_VALUE, "wrong len of queue", cu_q);

cu_q:
    queue_free(&q);
    return e;
}

static size_t pre_wait_calls = 0;
static void pre_wait_cb(queue_cb_t *qcb_data){
    (void)qcb_data;
    pre_wait_calls++;
}

static size_t new_data_calls = 0;
static void new_data_cb(queue_cb_t* qcb_data, link_t *new_data){
    (void)qcb_data; (void)new_data;
    new_data_calls++;
}

static derr_t test_queue_cb(void){
    derr_t e = E_OK;
    // create a list of qtest_t's
    qtest_t qlist[NELEMS];
    for(size_t i = 0; i < NELEMS; i++){
        qlist[i].id = i;
        link_init(&qlist[i].link);
        queue_cb_prep(&qlist[i].qcb);
        queue_cb_set(&qlist[i].qcb, pre_wait_cb, new_data_cb);
    }

    queue_t q;
    PROP(&e, queue_init(&q) );

    // set up some waiters
    queue_pop_first_cb(&q, &qlist[0].qcb);
    if(pre_wait_calls != 1) ORIG_GO(&e, E_VALUE, "missed pre-wait hook", cu_q);
    queue_pop_first_cb(&q, &qlist[1].qcb);
    if(pre_wait_calls != 2) ORIG_GO(&e, E_VALUE, "missed pre-wait hook", cu_q);
    queue_pop_first_cb(&q, &qlist[2].qcb);
    if(pre_wait_calls != 3) ORIG_GO(&e, E_VALUE, "missed pre-wait hook", cu_q);

    // append/prepend some elements
    if(q.len != 0) ORIG_GO(&e, E_VALUE, "wrong len of queue", cu_q);
    queue_append(&q, &qlist[3].link);
    if(new_data_calls != 1) ORIG_GO(&e, E_VALUE, "missed new-data hook", cu_q);
    if(q.len != 0) ORIG_GO(&e, E_VALUE, "wrong len of queue", cu_q);
    queue_append(&q, &qlist[4].link);
    if(new_data_calls != 2) ORIG_GO(&e, E_VALUE, "missed new-data hook", cu_q);
    if(q.len != 0) ORIG_GO(&e, E_VALUE, "wrong len of queue", cu_q);
    queue_prepend(&q, &qlist[5].link);
    if(new_data_calls != 3) ORIG_GO(&e, E_VALUE, "missed new-data hook", cu_q);
    if(q.len != 0) ORIG_GO(&e, E_VALUE, "wrong len of queue", cu_q);

    queue_prepend(&q, &qlist[6].link);
    if(new_data_calls != 3) ORIG_GO(&e, E_VALUE, "unexpected new-data hook", cu_q);
    if(q.len != 1) ORIG_GO(&e, E_VALUE, "wrong len of queue", cu_q);

cu_q:
    queue_free(&q);
    return e;
}

static size_t first_pops = 0;
static void *first_popper_thread(void *arg){
    queue_t *q = arg;
    qtest_t *qtest;
    do {
        qtest = CONTAINER_OF(queue_pop_first(q, true), qtest_t, link);
        first_pops++;
    } while(qtest->id != 4);
    return NULL;
}
static size_t last_pops = 0;
static void *last_popper_thread(void *arg){
    queue_t *q = arg;
    qtest_t *qtest;
    do {
        qtest = CONTAINER_OF(queue_pop_last(q, true), qtest_t, link);
        last_pops++;
    } while(qtest->id != 4);
    return NULL;
}

static derr_t test_queue_blocking(void){
    derr_t e = E_OK;
    // create a list of qtest_t's
    qtest_t qlist[NELEMS];
    for(size_t i = 0; i < NELEMS; i++){
        qlist[i].id = i;
        link_init(&qlist[i].link);
        queue_cb_prep(&qlist[i].qcb);
        queue_cb_set(&qlist[i].qcb, pre_wait_cb, new_data_cb);
    }

    queue_t q;
    PROP(&e, queue_init(&q) );

    dthread_t consumer;

    // spawn consumer thread
    dthread_create(&consumer, first_popper_thread, &q);
    // only append elements (non-FIFO behavior is difficult to test)
    queue_append(&q, &qlist[0].link);
    queue_append(&q, &qlist[1].link);
    queue_append(&q, &qlist[2].link);
    queue_append(&q, &qlist[3].link);
    // send the "special" element, consumer thread will quit
    queue_append(&q, &qlist[4].link);
    // join thread
    dthread_join(&consumer);

    if(first_pops != 5){
        TRACE(&e, "expected %x but got %x\n", FU(5), FU(first_pops));
        ORIG_GO(&e, E_VALUE, "wrong number of first pops", cu_q);
    }

    // spawn thread again
    dthread_create(&consumer, last_popper_thread, &q);
    // only prepend elements (non-FIFO behavior is difficult to test)
    queue_prepend(&q, &qlist[0].link);
    queue_prepend(&q, &qlist[1].link);
    queue_prepend(&q, &qlist[2].link);
    queue_prepend(&q, &qlist[3].link);
    // send the "special" element, consumer thread will quit
    queue_prepend(&q, &qlist[4].link);
    // join thread
    dthread_join(&consumer);

    if(last_pops != 5){
        TRACE(&e, "expected %x but got %x\n", FU(5), FU(last_pops));
        ORIG_GO(&e, E_VALUE, "wrong number of last pops", cu_q);
    }
cu_q:
    queue_free(&q);
    return e;
}


int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_queue_simple(), test_fail);
    PROP_GO(&e, test_queue_cb(), test_fail);
    PROP_GO(&e, test_queue_blocking(), test_fail);

test_fail:
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
        LOG_ERROR("FAIL\n");
        return 1;
    }
    LOG_ERROR("PASS\n");
    return 0;
}
