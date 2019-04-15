#include <pthread.h>

#include <common.h>
#include <logger.h>
#include <queue.h>

#include "test_utils.h"

#define NELEMS 10

typedef struct {
    size_t id;
    queue_elem_t qe;
    queue_cb_t qcb;
} qtest_t;

static derr_t test_queue_simple(void){
    derr_t error = E_OK;
    // create a list of qtest_t's
    qtest_t qlist[NELEMS];
    for(size_t i = 0; i < NELEMS; i++){
        qlist[i].id = i;
        queue_elem_prep(&qlist[i].qe, &qlist[i]);
        queue_cb_prep(&qlist[i].qcb, &qlist[i]);
    }

    queue_t q;
    PROP( queue_init(&q) );
    void *resp;

    // check append/pop_first
    for(size_t i = 0; i < NELEMS; i++)
        queue_append(&q, &qlist[i].qe);
    if(q.len != NELEMS) ORIG_GO(E_VALUE, "wrong len of queue", cu_q);
    for(size_t i = 0; i < NELEMS; i++){
        resp = queue_pop_first(&q, false);
        if(resp == NULL)
            ORIG_GO(E_VALUE, "unexpected null\n", cu_q);
        if(resp != &qlist[i])
            ORIG_GO(E_VALUE, "mismatched value\n", cu_q);
    }
    if(q.len != 0) ORIG_GO(E_VALUE, "wrong len of queue", cu_q);
    resp = queue_pop_first(&q, false);
    if(resp != NULL)
        ORIG_GO(E_VALUE, "unexpected non-null\n", cu_q);
    if(q.len != 0) ORIG_GO(E_VALUE, "wrong len of queue", cu_q);

    // check prepend/pop_first
    for(size_t i = 0; i < NELEMS; i++)
        queue_prepend(&q, &qlist[i].qe);
    if(q.len != NELEMS) ORIG_GO(E_VALUE, "wrong len of queue", cu_q);
    for(size_t i = NELEMS-1; i < NELEMS; i--){
        resp = queue_pop_first(&q, false);
        if(resp == NULL)
            ORIG_GO(E_VALUE, "unexpected null\n", cu_q);
        if(resp != &qlist[i])
            ORIG_GO(E_VALUE, "mismatched value\n", cu_q);
    }
    if(q.len != 0) ORIG_GO(E_VALUE, "wrong len of queue", cu_q);
    resp = queue_pop_first(&q, false);
    if(resp != NULL)
        ORIG_GO(E_VALUE, "unexpected non-null\n", cu_q);
    if(q.len != 0) ORIG_GO(E_VALUE, "wrong len of queue", cu_q);

    // check prepend/pop_last
    for(size_t i = 0; i < NELEMS; i++)
        queue_prepend(&q, &qlist[i].qe);
    if(q.len != NELEMS) ORIG_GO(E_VALUE, "wrong len of queue", cu_q);
    for(size_t i = 0; i < NELEMS; i++){
        resp = queue_pop_last(&q, false);
        if(resp == NULL)
            ORIG_GO(E_VALUE, "unexpected null\n", cu_q);
        if(resp != &qlist[i])
            ORIG_GO(E_VALUE, "mismatched value\n", cu_q);
    }
    if(q.len != 0) ORIG_GO(E_VALUE, "wrong len of queue", cu_q);
    resp = queue_pop_first(&q, false);
    if(resp != NULL)
        ORIG_GO(E_VALUE, "unexpected non-null\n", cu_q);
    if(q.len != 0) ORIG_GO(E_VALUE, "wrong len of queue", cu_q);

    // check append/pop_last
    for(size_t i = 0; i < NELEMS; i++)
        queue_append(&q, &qlist[i].qe);
    if(q.len != NELEMS) ORIG_GO(E_VALUE, "wrong len of queue", cu_q);
    for(size_t i = NELEMS-1; i < NELEMS; i--){
        resp = queue_pop_last(&q, false);
        if(resp == NULL)
            ORIG_GO(E_VALUE, "unexpected null\n", cu_q);
        if(resp != &qlist[i])
            ORIG_GO(E_VALUE, "mismatched value\n", cu_q);
    }
    if(q.len != 0) ORIG_GO(E_VALUE, "wrong len of queue", cu_q);
    resp = queue_pop_first(&q, false);
    if(resp != NULL)
        ORIG_GO(E_VALUE, "unexpected non-null\n", cu_q);
    if(q.len != 0) ORIG_GO(E_VALUE, "wrong len of queue", cu_q);

cu_q:
    queue_free(&q);
    return error;
}

size_t pre_wait_calls = 0;
static void pre_wait_cb(void *qcb_data){
    (void)qcb_data;
    pre_wait_calls++;
}

size_t new_data_calls = 0;
static void new_data_cb(void* qcb_data, void *new_data){
    (void)qcb_data; (void)new_data;
    new_data_calls++;
}

static derr_t test_queue_cb(void){
    derr_t error = E_OK;
    // create a list of qtest_t's
    qtest_t qlist[NELEMS];
    for(size_t i = 0; i < NELEMS; i++){
        qlist[i].id = i;
        queue_elem_prep(&qlist[i].qe, &qlist[i]);
        queue_cb_prep(&qlist[i].qcb, &qlist[i]);
        queue_cb_set(&qlist[i].qcb, pre_wait_cb, new_data_cb);
    }

    queue_t q;
    PROP( queue_init(&q) );

    // set up some waiters
    queue_pop_first_cb(&q, &qlist[0].qcb);
    if(pre_wait_calls != 1) ORIG_GO(E_VALUE, "missed pre-wait hook", cu_q);
    queue_pop_first_cb(&q, &qlist[1].qcb);
    if(pre_wait_calls != 2) ORIG_GO(E_VALUE, "missed pre-wait hook", cu_q);
    queue_pop_first_cb(&q, &qlist[2].qcb);
    if(pre_wait_calls != 3) ORIG_GO(E_VALUE, "missed pre-wait hook", cu_q);

    // append/prepend some elements
    if(q.len != 0) ORIG_GO(E_VALUE, "wrong len of queue", cu_q);
    queue_append(&q, &qlist[3].qe);
    if(new_data_calls != 1) ORIG_GO(E_VALUE, "missed new-data hook", cu_q);
    if(q.len != 0) ORIG_GO(E_VALUE, "wrong len of queue", cu_q);
    queue_append(&q, &qlist[4].qe);
    if(new_data_calls != 2) ORIG_GO(E_VALUE, "missed new-data hook", cu_q);
    if(q.len != 0) ORIG_GO(E_VALUE, "wrong len of queue", cu_q);
    queue_prepend(&q, &qlist[5].qe);
    if(new_data_calls != 3) ORIG_GO(E_VALUE, "missed new-data hook", cu_q);
    if(q.len != 0) ORIG_GO(E_VALUE, "wrong len of queue", cu_q);

    queue_prepend(&q, &qlist[6].qe);
    if(new_data_calls != 3) ORIG_GO(E_VALUE, "unexpected new-data hook", cu_q);
    if(q.len != 1) ORIG_GO(E_VALUE, "wrong len of queue", cu_q);

cu_q:
    queue_free(&q);
    return error;
}

size_t first_pops = 0;
static void *first_popper_thread(void *arg){
    queue_t *q = arg;
    qtest_t *qtest;
    do {
        qtest = queue_pop_first(q, true);
        first_pops++;
    } while(qtest->id != 4);
    return NULL;
}
size_t last_pops = 0;
static void *last_popper_thread(void *arg){
    queue_t *q = arg;
    qtest_t *qtest;
    do {
        qtest = queue_pop_last(q, true);
        last_pops++;
    } while(qtest->id != 4);
    return NULL;
}

static derr_t test_queue_blocking(void){
    derr_t error = E_OK;
    // create a list of qtest_t's
    qtest_t qlist[NELEMS];
    for(size_t i = 0; i < NELEMS; i++){
        qlist[i].id = i;
        queue_elem_prep(&qlist[i].qe, &qlist[i]);
        queue_cb_prep(&qlist[i].qcb, &qlist[i]);
        queue_cb_set(&qlist[i].qcb, pre_wait_cb, new_data_cb);
    }

    queue_t q;
    PROP( queue_init(&q) );

    pthread_t consumer;

    // spawn consumer thread
    pthread_create(&consumer, NULL, first_popper_thread, &q);
    // only append elements (non-FIFO behavior is difficult to test)
    queue_append(&q, &qlist[0].qe);
    queue_append(&q, &qlist[1].qe);
    queue_append(&q, &qlist[2].qe);
    queue_append(&q, &qlist[3].qe);
    // send the "special" element, consumer thread will start with pop_last'ing
    queue_append(&q, &qlist[4].qe);
    // join thread
    pthread_join(consumer, NULL);

    if(first_pops != 5){
        LOG_ERROR("expected %x but got %x\n", FU(5), FU(first_pops));
        ORIG_GO(E_VALUE, "wrong number of first pops", cu_q);
    }

    // spawn thread again
    pthread_create(&consumer, NULL, last_popper_thread, &q);
    // only prepend elements (non-FIFO behavior is difficult to test)
    queue_prepend(&q, &qlist[0].qe);
    queue_prepend(&q, &qlist[1].qe);
    queue_prepend(&q, &qlist[2].qe);
    queue_prepend(&q, &qlist[3].qe);
    // send the "special" element, consumer thread will quit
    queue_prepend(&q, &qlist[4].qe);
    // join thread
    pthread_join(consumer, NULL);

    if(last_pops != 5){
        LOG_ERROR("expected %x but got %x\n", FU(5), FU(last_pops));
        ORIG_GO(E_VALUE, "wrong number of last pops", cu_q);
    }
cu_q:
    queue_free(&q);
    return error;
}


int main(int argc, char** argv){
    derr_t error;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO( test_queue_simple(), test_fail);
    PROP_GO( test_queue_cb(), test_fail);
    PROP_GO( test_queue_blocking(), test_fail);

test_fail:
    if(error){
        LOG_ERROR("FAIL\n");
        return 1;
    }
    LOG_ERROR("PASS\n");
    return 0;
}
