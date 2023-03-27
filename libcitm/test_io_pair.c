#include "libduv/fake_stream.h"
#include "libcitm/libcitm.h"
#include "libcitm/fake_citm.h"

#include "test/test_utils.h"

typedef struct {
    size_t *count;
    size_t *successes;
    citm_conn_t **conn_dn;
    citm_conn_t **conn_up;
    derr_t *e;
} ptrs_t;

static void cb(
    void *data, derr_t e, citm_conn_t *conn_dn, citm_conn_t *conn_up
){
    ptrs_t *ptrs = data;
    (*ptrs->count)++;
    (*ptrs->successes) += !is_error(e);
    *ptrs->conn_dn = conn_dn;
    *ptrs->conn_up = conn_up;
    *ptrs->e = e;
}

static derr_t test_io_pair(void){
    derr_t e = E_OK;

    manual_scheduler_t m;
    fake_stream_t sdn;
    fake_stream_t sup;
    fake_citm_conn_t fcdn;
    fake_citm_conn_t fcup;
    citm_conn_t *conn_dn = NULL;
    citm_conn_t *conn_up = NULL;
    fake_citm_connect_t fcnct;
    fake_citm_io_t fio;

    scheduler_i *sched = manual_scheduler(&m);
    citm_io_i *io = fake_citm_io(&fio);

    size_t cb_count = 0;
    size_t cb_successes = 0;
    citm_conn_t *result_dn = NULL;
    citm_conn_t *result_up = NULL;
    derr_t result_e = E_OK;
    ptrs_t ptrs = {
        &cb_count, &cb_successes, &result_dn, &result_up, &result_e
    };

    link_t io_pairs = {0};

    // success test
    conn_dn = fake_citm_conn_insec(&fcdn, fake_stream(&sdn));
    conn_up = fake_citm_conn_insec(&fcup, fake_stream(&sup));
    fake_citm_connect_prep(&fcnct);
    link_list_append(&fio.fcncts, &fcnct.link);
    PROP_GO(&e, io_pair_new(sched, io, conn_dn, cb, &ptrs, &io_pairs), cu);
    EXPECT_LIST_LENGTH_GO(&e, "io_pairs", &io_pairs, 1, cu);
    EXPECT_LIST_LENGTH_GO(&e, "fcncts", &fio.fcncts, 0, cu);
    EXPECT_U_GO(&e, "cb_count", cb_count, 0, cu);
    EXPECT_U_GO(&e, "cb_successes", cb_successes, 0, cu);
    PROP_GO(&e, fake_citm_connect_finish(&fcnct, conn_up, E_NONE), cu);
    EXPECT_U_GO(&e, "cb_count", cb_count, 1, cu);
    EXPECT_U_GO(&e, "cb_successes", cb_successes, 1, cu);
    EXPECT_P_GO(&e, "result_dn", result_dn, conn_dn, cu);
    EXPECT_P_GO(&e, "result_up", result_up, conn_up, cu);
    EXPECT_E_VAR_GO(&e, "result_e", &result_e, E_NONE, cu);

    // cancel test, pre-connect
    conn_dn = fake_citm_conn(
        &fcdn, fake_stream(&sdn), IMAP_SEC_INSECURE, NULL, DSTR_LIT("")
    );
    fake_citm_connect_prep(&fcnct);
    link_list_append(&fio.fcncts, &fcnct.link);
    PROP_GO(&e, io_pair_new(sched, io, conn_dn, cb, &ptrs, &io_pairs), cu);
    EXPECT_LIST_LENGTH_GO(&e, "io_pairs", &io_pairs, 1, cu);
    EXPECT_LIST_LENGTH_GO(&e, "fcncts", &fio.fcncts, 0, cu);
    // cancel the io_pair
    io_pair_cancel(io_pairs.next);
    PROP_GO(&e, fake_citm_connect_finish(&fcnct, NULL, E_CANCELED), cu);
    // callback should have failed
    EXPECT_U_GO(&e, "cb_count", cb_count, 2, cu);
    EXPECT_U_GO(&e, "cb_successes", cb_successes, 1, cu);
    EXPECT_E_VAR_GO(&e, "result_e", &result_e, E_CANCELED, cu);

    // failure test
    conn_dn = fake_citm_conn(
        &fcdn, fake_stream(&sdn), IMAP_SEC_INSECURE, NULL, DSTR_LIT("")
    );
    fake_citm_connect_prep(&fcnct);
    link_list_append(&fio.fcncts, &fcnct.link);
    PROP_GO(&e, io_pair_new(sched, io, conn_dn, cb, &ptrs, &io_pairs), cu);
    EXPECT_LIST_LENGTH_GO(&e, "io_pairs", &io_pairs, 1, cu);
    EXPECT_LIST_LENGTH_GO(&e, "fcncts", &fio.fcncts, 0, cu);
    PROP_GO(&e, fake_citm_connect_finish(&fcnct, NULL, E_VALUE), cu);
    // expect the failure message
    DSTR_STATIC(msg, "* BYE failed to connect to upstream server\r\n");
    PROP_GO(&e, fake_stream_expect_read(&m, &sdn, msg), cu);
    // invoke the shutdown cb
    fake_stream_shutdown(&sdn);
    ADVANCE_FAKES(&m, &sdn);
    // callback should have failed
    EXPECT_U_GO(&e, "cb_count", cb_count, 3, cu);
    EXPECT_U_GO(&e, "cb_successes", cb_successes, 1, cu);
    EXPECT_E_VAR_GO(&e, "result_e", &result_e, E_VALUE, cu);
    DROP_VAR(&result_e);

    // failure test with post-failure cancel
    conn_dn = fake_citm_conn(
        &fcdn, fake_stream(&sdn), IMAP_SEC_INSECURE, NULL, DSTR_LIT("")
    );
    fake_citm_connect_prep(&fcnct);
    link_list_append(&fio.fcncts, &fcnct.link);
    PROP_GO(&e, io_pair_new(sched, io, conn_dn, cb, &ptrs, &io_pairs), cu);
    EXPECT_LIST_LENGTH_GO(&e, "io_pairs", &io_pairs, 1, cu);
    EXPECT_LIST_LENGTH_GO(&e, "fcncts", &fio.fcncts, 0, cu);
    PROP_GO(&e, fake_citm_connect_finish(&fcnct, NULL, E_VALUE), cu);
    // cancel the io_pair
    io_pair_cancel(io_pairs.next);
    ADVANCE_FAKES(&m, &sdn);
    // callback should have failed
    EXPECT_U_GO(&e, "cb_count", cb_count, 4, cu);
    EXPECT_U_GO(&e, "cb_successes", cb_successes, 1, cu);
    EXPECT_E_VAR_GO(&e, "result_e", &result_e, E_VALUE, cu);
    DROP_VAR(&result_e);

cu:
    // cancel the io_pair
    if(!link_list_isempty(&io_pairs)){
        io_pair_cancel(io_pairs.next);
        ADVANCE_FAKES(&m, &sdn);
    }
    // finish canceling any citm_connect_i's
    DROP_CMD( fake_citm_connect_finish(&fcnct, NULL, E_CANCELED) );
    DROP_VAR(&result_e);
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    int exit_code = 0;

    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    PROP_GO(&e, test_io_pair(), cu);

cu:
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
        LOG_ERROR("FAIL\n");
        exit_code = 1;
    }else{
        LOG_ERROR("PASS\n");
    }

    return exit_code;
}
