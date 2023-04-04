#include "libduv/fake_stream.h"
#include "libcitm/libcitm.h"
#include "libcitm/fake_citm.h"

#include "test/test_utils.h"

typedef struct {
    size_t *count;
    citm_conn_t **conn_dn;
    citm_conn_t **conn_up;
} ptrs_t;

static void cb(
    void *data, citm_conn_t *conn_dn, citm_conn_t *conn_up
){
    ptrs_t *ptrs = data;
    (*ptrs->count)++;
    *ptrs->conn_dn = conn_dn;
    *ptrs->conn_up = conn_up;
}

static derr_t test_io_pair(void){
    derr_t e = E_OK;

    fake_stream_t sdn;
    fake_stream_t sup;
    fake_citm_conn_t fcdn;
    fake_citm_conn_t fcup;
    citm_conn_t *conn_dn = NULL;
    citm_conn_t *conn_up = NULL;
    fake_citm_connect_t fcnct;
    fake_citm_io_t fio;
    link_t *link;

    citm_io_i *io = fake_citm_io(&fio);

    size_t cb_count = 0;
    citm_conn_t *result_dn = NULL;
    citm_conn_t *result_up = NULL;
    ptrs_t ptrs = { &cb_count, &result_dn, &result_up };

    link_t io_pairs = {0};

    // success test
    conn_dn = fake_citm_conn(
        &fcdn, fake_stream(&sdn), IMAP_SEC_INSECURE, NULL, DSTR_LIT("")
    );
    conn_up = fake_citm_conn(
        &fcup, fake_stream(&sup), IMAP_SEC_INSECURE, NULL, DSTR_LIT("")
    );
    fake_citm_connect_prep(&fcnct);
    link_list_append(&fio.fcncts, &fcnct.link);
    io_pair_new(io, conn_dn, cb, &ptrs, &io_pairs);
    EXPECT_LIST_LENGTH_GO(&e, "io_pairs", &io_pairs, 1, cu);
    EXPECT_LIST_LENGTH_GO(&e, "fcncts", &fio.fcncts, 0, cu);
    EXPECT_U_GO(&e, "cb_count", cb_count, 0, cu);
    PROP_GO(&e, fake_citm_connect_finish(&fcnct, conn_up, E_NONE), cu);
    EXPECT_U_GO(&e, "cb_count", cb_count, 1, cu);
    EXPECT_P_GO(&e, "result_dn", result_dn, conn_dn, cu);
    EXPECT_P_GO(&e, "result_up", result_up, conn_up, cu);

    // failure test
    conn_dn = fake_citm_conn(
        &fcdn, fake_stream(&sdn), IMAP_SEC_INSECURE, NULL, DSTR_LIT("")
    );
    fake_citm_connect_prep(&fcnct);
    link_list_append(&fio.fcncts, &fcnct.link);
    io_pair_new(io, conn_dn, cb, &ptrs, &io_pairs);
    EXPECT_LIST_LENGTH_GO(&e, "io_pairs", &io_pairs, 1, cu);
    EXPECT_LIST_LENGTH_GO(&e, "fcncts", &fio.fcncts, 0, cu);
    PROP_GO(&e, fake_citm_connect_finish(&fcnct, NULL, E_VALUE), cu);
    // no callback should have been made
    EXPECT_U_GO(&e, "cb_count", cb_count, 1, cu);
    EXPECT_B_GO(&e, "conn_dn.is_closed", fcdn.is_closed, true, cu);

    // cancel test
    conn_dn = fake_citm_conn(
        &fcdn, fake_stream(&sdn), IMAP_SEC_INSECURE, NULL, DSTR_LIT("")
    );
    fake_citm_connect_prep(&fcnct);
    link_list_append(&fio.fcncts, &fcnct.link);
    io_pair_new(io, conn_dn, cb, &ptrs, &io_pairs);
    EXPECT_LIST_LENGTH_GO(&e, "io_pairs", &io_pairs, 1, cu);
    EXPECT_LIST_LENGTH_GO(&e, "fcncts", &fio.fcncts, 0, cu);
    // cancel the io_pair
    io_pair_cancel(link_list_pop_first(&io_pairs));
    PROP_GO(&e, fake_citm_connect_finish(&fcnct, NULL, E_CANCELED), cu);
    // no callback should have been made
    EXPECT_U_GO(&e, "cb_count", cb_count, 1, cu);
    EXPECT_B_GO(&e, "conn_dn.is_closed", fcdn.is_closed, true, cu);

cu:
    // cancel any io_pairs
    while((link = link_list_pop_first(&io_pairs))){
        io_pair_cancel(link);
    }
    // finish canceling any citm_connect_i's
    DROP_CMD( fake_citm_connect_finish(&fcnct, NULL, E_CANCELED) );
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

    ssl_library_close();
    return exit_code;
}
