#include "libcli/libcli.h"

#include <config.h>

#include "test/test_utils.h"

static status_update_cb g_update_cb = NULL;
static status_done_cb g_done_cb = NULL;
static void *g_cb_data = NULL;
static int g_init_called = 0;

static bool g_done_cb_coming = false;
static int g_close_called = 0;

static char g_buf[4096];
static dstr_t g_msg = { .data = g_buf, .size = sizeof(g_buf) };
static int g_print_called = 0;

static void g_reset(void){
    g_update_cb = NULL;
    g_done_cb = NULL;
    g_cb_data = NULL;
    g_init_called = 0;
    g_done_cb_coming = false;
    g_close_called = 0;
    g_msg.len = 0;
    g_print_called = 0;
}

static derr_t sc_init(
    status_client_t *sc,
    uv_loop_t *loop,
    scheduler_i *scheduler,
    string_builder_t status_sock,
    status_update_cb update_cb,
    status_done_cb done_cb,
    void *cb_data
){
    (void)sc;
    (void)loop;
    (void)scheduler;
    (void)status_sock;
    g_update_cb = update_cb;
    g_done_cb = done_cb;
    g_cb_data = cb_data;
    g_init_called++;
    g_done_cb_coming = true;
    return E_OK;
}

static bool sc_close(status_client_t *sc){
    (void)sc;
    g_close_called++;
    return g_done_cb_coming;
}

static derr_t fake_print(dstr_t buf){
    g_print_called++;
    return dstr_copy2(buf, &g_msg);
}

static void force_close_status(status_t *s, manual_scheduler_t *m){
    if(s->advancer.down_done) return;

    // force-close the status_t
    advancer_schedule(&s->advancer, (derr_t){.type = E_CANCELED});
    g_done_cb_coming = false;
    manual_scheduler_run(m);
    if(!s->advancer.down_done){
        // no remediation is possible
        LOG_FATAL("status refused to exit\n");
    }
}

#define EXPECT_LINES_GO(e, msg, text, label, ...) do { \
    dstr_t _text = (text); \
    dstr_t _line; \
    char *_lines[] = { __VA_ARGS__ }; \
    size_t _nlines = sizeof(_lines)/sizeof(*_lines); \
    for(size_t i = 0; i < _nlines; i++){ \
        dstr_split2_soft(_text, DSTR_LIT("\n"), NULL, &_line, &_text); \
        dstr_t _exp_line = dstr_from_cstr(_lines[i]); \
        TRACE((e), "i=%x\n", FU(i)); \
        EXPECT_D3_GO((e), "line of " msg, _line, _exp_line, label); \
        DROP_VAR((e)); \
    } \
    EXPECT_DS_GO((e), "remainder", _text, "", label); \
} while(0)

static derr_t test_status(bool follow){
    derr_t e = E_OK;

    manual_scheduler_t m;
    scheduler_i *sched = manual_scheduler(&m);

    // root is unused by mock status client
    duv_root_t root = {0};
    status_t s = {
        .status_sock = SBS(""),
        .follow = follow,
        .root = &root,
        .sc_init = sc_init,
        .sc_close = sc_close,
        .print = fake_print,
    };
    advancer_prep(
        &s.advancer, sched, status_advance_up, status_advance_down
    );

    citm_status_t status;

    // start the status_t
    advancer_schedule(&s.advancer, E_OK);
    manual_scheduler_run(&m);
    EXPECT_I_GO(&e, "g_init_called", g_init_called, 1, cu);

    // send an update
    PROP_GO(&e,
        citm_status_init(&status,
            SM_VER_MAJ,
            SM_VER_MIN,
            SM_VER_PAT,
            dstr_from_cstr("dom"),
            dstr_from_cstr("maj"),
            dstr_from_cstr("min"),
            TRI_YES,
            TRI_YES
        ),
    cu);
    g_update_cb(g_cb_data, status);
    manual_scheduler_run(&m);
    EXPECT_I_GO(&e, "g_print_called", g_print_called, 1, cu);

    if(!follow){
        EXPECT_I_GO(&e, "g_close_called", g_close_called, 1, cu);

        EXPECT_LINES_GO(&e, "g_msg", g_msg, cu,
            "splintermail server version: " SM_VER_STR,
            "subdomain: dom",
            "status: maj: min",
        );

        // send the done callback
        g_done_cb(g_cb_data, E_OK);
        g_done_cb_coming = false;
        manual_scheduler_run(&m);
        EXPECT_B_GO(&e, "down_done", s.advancer.down_done, true, cu);
        goto cu;
    }

    // follow == true
    EXPECT_I_GO(&e, "g_close_called", g_close_called, 0, cu);

    EXPECT_LINES_GO(&e, "g_msg", g_msg, cu,
        "splintermail server version: " SM_VER_STR,
        "subdomain: dom",
        "status: maj: min",
        "---",
    );
    g_msg.len = 0;

    // send two updates in quick succession
    PROP_GO(&e,
        citm_status_init(&status,
            SM_VER_MAJ,
            SM_VER_MIN,
            SM_VER_PAT,
            dstr_from_cstr("x"),
            dstr_from_cstr("y"),
            dstr_from_cstr("z"),
            TRI_YES,
            TRI_YES
        ),
    cu);
    g_update_cb(g_cb_data, status);

    PROP_GO(&e,
        citm_status_init(&status,
            SM_VER_MAJ,
            SM_VER_MIN,
            SM_VER_PAT,
            dstr_from_cstr("DOM"),
            dstr_from_cstr("MAJ"),
            dstr_from_cstr("MIN"),
            TRI_YES,
            TRI_YES
        ),
    cu);
    g_update_cb(g_cb_data, status);

    // expect only the mutable bits, and only from only the second one
    manual_scheduler_run(&m);
    EXPECT_I_GO(&e, "g_close_called", g_close_called, 0, cu);
    EXPECT_I_GO(&e, "g_print_called", g_print_called, 2, cu);
    EXPECT_LINES_GO(&e, "g_msg", g_msg, cu,
        "status: MAJ: MIN",
        "---",
    );

    // throw an error from the status_client
    g_done_cb(g_cb_data, (derr_t){ .type = E_CONN });
    g_done_cb_coming = false;
    manual_scheduler_run(&m);
    EXPECT_E_VAR_GO(&e, "status error", &s.advancer.e, E_CONN, cu);

cu:
    force_close_status(&s, &m);
    if(is_error(e) && is_error(s.advancer.e)){
        LOG_ERROR("s.advancer.e error will be dropped:\n");
        DUMP(s.advancer.e);
        LOG_ERROR("-- end s.advancer.e error --\n");
    }
    TRACE_MULTIPROP_VAR(&e, &s.advancer.e);

    g_reset();

    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;
    int exit_code = 1;

    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    TRACE(&e, "with follow=false:\n");
    PROP_GO(&e, test_status(false), cu);
    DROP_VAR(&e);

    TRACE(&e, "with follow=true:\n");
    PROP_GO(&e, test_status(true), cu);
    DROP_VAR(&e);

    exit_code = 0;

cu:
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
    }

    LOG_ERROR(exit_code ? "FAIL\n" : "PASS\n");
    return exit_code;
}
