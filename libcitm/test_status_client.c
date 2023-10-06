#include "libcli/libcli.h"

#include "test/test_utils.h"

typedef struct {
    duv_root_t root;
    advancer_t advancer;

    string_builder_t sock;

    status_server_t ss;
    status_client_t sc;

    int check_cbs;
    int exp_check_cbs;
    int update_cbs;
    int exp_update_cbs;
    bool exp_ss_done;
    bool exp_sc_done;

    citm_status_t exp_status;

    bool init : 1;
    bool check_sent : 1;
    bool update_sent : 1;
} test_client_t;

DEF_CONTAINER_OF(test_client_t, advancer, advancer_t)

static void tc_ss_check_cb(void *data){
    derr_t e = E_OK;
    test_client_t *tc = data;
    tc->check_cbs++;
    EXPECT_I_GO(&e, "check_cbs", tc->check_cbs, tc->exp_check_cbs, done);
done:
    advancer_schedule(&tc->advancer, e);
}

static void tc_ss_done_cb(void *data, derr_t err){
    derr_t e = E_OK;
    test_client_t *tc = data;
    EXPECT_E_VAR_GO(&e, "ss_done_cb(err=)", &err, E_CANCELED, done);
    EXPECT_B_GO(&e, "exp_ss_done", tc->exp_ss_done, true, done);
done:
    advancer_schedule(&tc->advancer, e);
}

static void tc_sc_update_cb(void *data, citm_status_t status){
    derr_t e = E_OK;
    test_client_t *tc = data;
    tc->update_cbs++;
    EXPECT_I_GO(&e,
        "exp_sc_updates", tc->update_cbs, tc->exp_update_cbs, done
    );
    EXPECT_D3_GO(&e,
        "sc_update_cb(domain=)",
        status.fulldomain,
        tc->exp_status.fulldomain,
        done
    );
    EXPECT_D3_GO(&e,
        "sc_update_cb(status_maj=)",
        status.status_maj,
        tc->exp_status.status_maj,
        done
    );
    EXPECT_D3_GO(&e,
        "sc_update_cb(status_min=)",
        status.status_min,
        tc->exp_status.status_min,
        done
    );
    EXPECT_I_GO(&e,
        "sc_update_cb(configured=)",
        status.configured,
        tc->exp_status.configured,
        done
    );
    EXPECT_I_GO(&e,
        "sc_update_cb(tls_ready=)",
        status.tls_ready,
        tc->exp_status.tls_ready,
        done
    );
done:
    citm_status_free(&status);
    advancer_schedule(&tc->advancer, e);
}

static void tc_sc_done_cb(void *data, derr_t err){
    derr_t e = E_OK;
    test_client_t *tc = data;
    EXPECT_E_VAR_GO(&e, "sc_done_cb(err=)", &err, E_CANCELED, done);
    EXPECT_B_GO(&e, "exp_sc_done", tc->exp_sc_done, true, done);
done:
    advancer_schedule(&tc->advancer, e);
}

static derr_t test_client_advance_up(advancer_t *advancer){
    test_client_t *tc = CONTAINER_OF(advancer, test_client_t, advancer);

    derr_t e = E_OK;

    ONCE(tc->init){
        // start server
        PROP(&e,
            status_server_init(
                &tc->ss,
                &tc->root.loop,
                &tc->root.scheduler.iface,
                tc->sock,
                STATUS_MAJ_TLS_RENEW,
                STATUS_MIN_CREATE_ORDER,
                DSTR_LIT("yo.com"), // fulldomain
                tc_ss_check_cb,
                tc_ss_done_cb,
                tc // cb_data
            )
        );

        // start client
        PROP(&e,
            status_client_init(
                &tc->sc,
                &tc->root.loop,
                &tc->root.scheduler.iface,
                tc->sock,
                tc_sc_update_cb,
                tc_sc_done_cb,
                tc // cb_data
            )
        );

        // expect an initial udpate
        tc->exp_update_cbs++;
        PROP(&e,
            citm_status_init(
                &tc->exp_status,
                SPLINTERMAIL_VERSION_MAJOR,
                SPLINTERMAIL_VERSION_MINOR,
                SPLINTERMAIL_VERSION_PATCH,
                DSTR_LIT("yo.com"),
                status_maj_dstr(STATUS_MAJ_TLS_RENEW),
                status_min_dstr(STATUS_MIN_CREATE_ORDER),
                TRI_YES,
                TRI_YES
            )
        );
    }
    // expect an initial update
    if(tc->update_cbs < 1) return e;

    // ask for a check
    ONCE(tc->check_sent){
        status_client_check(&tc->sc);
        tc->exp_check_cbs++;
    }

    // wait for it to appear server-side
    if(tc->check_cbs < 1) return e;

    // trigger an update
    ONCE(tc->update_sent){
        status_server_update(
            &tc->ss,
            STATUS_MAJ_TLS_RENEW,
            STATUS_MIN_GET_AUTHZ,
            DSTR_LIT("yo.com")
        );
        tc->exp_update_cbs++;
        citm_status_free(&tc->exp_status);
        PROP(&e,
            citm_status_init(
                &tc->exp_status,
                SPLINTERMAIL_VERSION_MAJOR,
                SPLINTERMAIL_VERSION_MINOR,
                SPLINTERMAIL_VERSION_PATCH,
                DSTR_LIT("yo.com"),
                status_maj_dstr(STATUS_MAJ_TLS_RENEW),
                status_min_dstr(STATUS_MIN_GET_AUTHZ),
                TRI_YES,
                TRI_YES
            )
        );
    }
    // expect a secondary update
    if(tc->update_cbs < 2) return e;

    advancer_up_done(&tc->advancer);
    return e;
}

static void test_client_advance_down(advancer_t *advancer, derr_t *E){
    test_client_t *tc = CONTAINER_OF(advancer, test_client_t, advancer);

    if(status_client_close(&tc->sc)){
        tc->exp_sc_done = true;
        return;
    }

    if(status_server_close(&tc->ss)){
        tc->exp_ss_done = true;
        return;
    }

    citm_status_free(&tc->exp_status);

    (void)E;
    advancer_down_done(&tc->advancer);
}

static derr_t test_status_client(void){
    derr_t e = E_OK;

    DSTR_VAR(temp, 256);
    string_builder_t sock;
    #ifdef _WIN32
    // windows
    DSTR_VAR(rando, 9);
    PROP(&e, random_bytes(&rando, 9) );
    PROP(&e, FMT(&temp, "\\\\.\\pipe\\test-status-client-%x", FB64D(rando)) );
    sock = SBD(temp);
    #else
    // unix
    PROP(&e, mkdir_temp("test-status-client", &temp) );
    sock = sb_append(&SBD(temp), SBS("sock"));
    #endif

    test_client_t tc = { .sock = sock };
    advancer_prep(
        &tc.advancer,
        &tc.root.scheduler.iface,
        test_client_advance_up,
        test_client_advance_down
    );
    PROP_GO(&e, duv_root_run(&tc.root, &tc.advancer), cu);

cu:
    #ifndef _WIN32
    // unix
    DROP_CMD( rm_rf(temp.data) );
    #endif

    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;
    int exit_code = 1;

    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    PROP_GO(&e, test_status_client(), cu);

    exit_code = 0;

cu:
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
    }

    LOG_ERROR(exit_code ? "FAIL\n" : "PASS\n");
    return exit_code;
}
