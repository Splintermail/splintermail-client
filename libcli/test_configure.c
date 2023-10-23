#include "libcli/libcli.h"

#include "test/test_utils.h"

typedef enum {
    IO_INIT,
    IO_CLOSE,
    SC_CHECK,
    ACME_DIR,
    ADD_INST,
    PRINT,
    PROMPT_ONE_OF,
    USER_PROMPT,
    MKDIRS,
    DIR_RW_ACCESS,
    WRITE_INST,
} call_e;

static char *call_type_names[] = {
    "IO_INIT",
    "IO_CLOSE",
    "SC_CHECK",
    "ACME_DIR",
    "ADD_INST",
    "PRINT",
    "PROMPT_ONE_OF",
    "USER_PROMPT",
    "MKDIRS",
    "DIR_RW_ACCESS",
    "WRITE_INST",
};

typedef union {
    bool io_init; // sc
    dstr_t *acme_dir; // resp
    struct {
        char *user;
        char *pass;
    } add_inst;
    char *print;
    struct {
        char *msg;
        char *opts;
    } prompt_one_of;
    struct {
        char *msg;
        bool hide;
    } user_prompt;
    char *mkdirs;
    char *dir_rw_access;
    char *write_inst; // path
} call_u;

typedef union {
    bool io_close;
    char prompt_one_of;
    char *user_prompt;
    derr_type_t mkdirs;
    bool dir_rw_access;
} resp_u;

typedef struct {
    call_e type;
    call_u arg;
    resp_u resp;
} call_t;

typedef struct {
    manual_scheduler_t m;

    configure_t c;
    configure_i iface;
    dstr_t *acme_dir_resp;
    json_t *add_inst_resp;
    int retval;

    call_t calls[8];
    size_t nexp;
    size_t ngot;

    derr_t e;
    bool started;
    bool forceclose;
} test_t;

DEF_CONTAINER_OF(test_t, iface, configure_i)

static bool expect_call(test_t *t, call_e type, call_t *out){
    *out = (call_t){0};
    if(is_error(t->e)) return false;
    if(t->ngot >= t->nexp){
        for(size_t i = 0; i < t->nexp; i++){
            TRACE(&t->e,
                "after expected call to %x,\n",
                FS(call_type_names[t->calls[i].type])
            );
        }
        TRACE(&t->e,
            "got unexpected expected call to %x!\n",
            FS(call_type_names[type])
        );
        TRACE_ORIG(&t->e, E_VALUE, "too many calls");
        LOG_ERROR("failure occured\n");
        return false;
    }
    size_t i = t->ngot++;
    call_t call = t->calls[i];
    if(call.type != type){
        TRACE_ORIG(&t->e,
            E_VALUE,
            "expected call[%x] to be %x but got %x",
            FU(i),
            FS(call_type_names[call.type]),
            FS(call_type_names[type])
        );
        LOG_ERROR("failure occured\n");
        return false;
    }
    *out = call;
    return true;
}

static derr_t fake_io_init(configure_i *iface, bool sc){
    test_t *t = CONTAINER_OF(iface, test_t, iface);
    call_t call;
    if(!expect_call(t, IO_INIT, &call)) return E_OK;
    EXPECT_B_GO(&t->e, "io_init(sc=)", sc, call.arg.io_init, done);
done:
    if(is_error(t->e)) LOG_ERROR("failure occured\n");
    return E_OK;
}

static bool fake_io_close(configure_i *iface){
    test_t *t = CONTAINER_OF(iface, test_t, iface);
    // if we are forceclosing, just shutdown immediately
    if(t->forceclose) return false;
    call_t call;
    // if we aren't expecting io to close, just shut down immediately
    if(!expect_call(t, IO_CLOSE, &call)) return false;
    return call.resp.io_close;
}

static void fake_status_client_check(configure_i *iface){
    test_t *t = CONTAINER_OF(iface, test_t, iface);
    call_t call;
    expect_call(t, SC_CHECK, &call);
}

static void fake_get_acme_dir(configure_i *iface, dstr_t *resp){
    test_t *t = CONTAINER_OF(iface, test_t, iface);
    t->acme_dir_resp = resp;
    call_t call;
    expect_call(t, ACME_DIR, &call);
}

static void fake_add_inst(
    configure_i *iface, dstr_t user, dstr_t pass, json_t *resp
){
    test_t *t = CONTAINER_OF(iface, test_t, iface);
    t->add_inst_resp = resp;
    call_t call;
    if(!expect_call(t, ADD_INST, &call)) return;
    EXPECT_DS_GO(&t->e, "add_inst(user=)", user, call.arg.add_inst.user, done);
    EXPECT_DS_GO(&t->e, "add_inst(pass=)", pass, call.arg.add_inst.pass, done);
done:
    if(is_error(t->e)) LOG_ERROR("failure occured\n");
    return;
}

static void fake_print(configure_i *iface, const char *fstr, const fmt_i *arg){
    test_t *t = CONTAINER_OF(iface, test_t, iface);
    call_t call;
    if(!expect_call(t, PRINT, &call)) return;
    const fmt_i *args[] = {arg};
    size_t nargs = !!arg;
    DSTR_VAR(buf, 4096);
    _fmt_quiet(WD(&buf), fstr, args, nargs);
    if(!dstr_contains(buf, dstr_from_cstr(call.arg.print))){
        ORIG_GO(&t->e,
            E_VALUE,
            "expected print(msg=) to contain \"%x\"\n but got \"%x\"",
            done,
            FS_DBG(call.arg.print),
            FD_DBG(buf)
        );
    }
done:
    if(is_error(t->e)) LOG_ERROR("failure occured\n");
    return;
}

static derr_t fake_prompt_one_of(
    configure_i *iface, dstr_t msg, const char *opts, size_t *ret
){
    test_t *t = CONTAINER_OF(iface, test_t, iface);
    call_t call;
    *ret = 0;
    if(!expect_call(t, PROMPT_ONE_OF, &call)) return E_OK;
    if(!dstr_contains(msg, dstr_from_cstr(call.arg.prompt_one_of.msg))){
        ORIG_GO(&t->e,
            E_VALUE,
            "expected prompt_one_of(msg=) to contain \"%x\"\n"
            "but got \"%x\"",
            done,
            FS_DBG(call.arg.prompt_one_of.msg),
            FD_DBG(msg)
        );
    }
    EXPECT_S_GO(&t->e,
        "prompt_one_of(opts=)", opts, call.arg.prompt_one_of.opts, done
    );
    for(size_t i = 0; i < strlen(opts); i++){
        if(call.resp.prompt_one_of != opts[i]) continue;
        *ret = i;
        return E_OK;
    }
    ORIG_GO(&t->e, E_INTERNAL, "unrecognized ret value in test", done);

done:
    if(is_error(t->e)) LOG_ERROR("failure occured\n");
    return E_OK;
}

static derr_t fake_user_prompt(
    configure_i *iface, dstr_t msg, dstr_t *out, bool hide
){
    test_t *t = CONTAINER_OF(iface, test_t, iface);
    *out = (dstr_t){0};
    call_t call;
    if(!expect_call(t, USER_PROMPT, &call)) return E_OK;
    if(!dstr_contains(msg, dstr_from_cstr(call.arg.user_prompt.msg))){
        ORIG_GO(&t->e,
            E_VALUE,
            "expected user_prompt(msg=) to contain \"%x\"\nbut got \"%x\"",
            done,
            FS_DBG(call.arg.user_prompt.msg),
            FD_DBG(msg)
        );
    }
    EXPECT_B_GO(&t->e,
        "user_prompt(hide=)", hide, call.arg.user_prompt.hide, done
    );

    derr_t e = E_OK;
    dstr_t resp = dstr_from_cstr(call.resp.user_prompt);
    PROP(&e, dstr_copy2(resp, out) );

done:
    if(is_error(t->e)) LOG_ERROR("failure occured\n");
    return E_OK;
}

static derr_t fake_mkdirs(configure_i *iface, string_builder_t path){
    test_t *t = CONTAINER_OF(iface, test_t, iface);
    call_t call;
    if(!expect_call(t, MKDIRS, &call)) return E_OK;
    EXPECT_SBS_GO(&t->e, "mkdirs(path=)", path, call.arg.mkdirs, done);

    return (derr_t){ .type = call.resp.mkdirs };

done:
    if(is_error(t->e)) LOG_ERROR("failure occured\n");
    return E_OK;
}

static derr_t fake_dir_rw_access(
    configure_i *iface, string_builder_t path, bool *ret
){
    test_t *t = CONTAINER_OF(iface, test_t, iface);
    *ret = true;
    call_t call;
    if(!expect_call(t, DIR_RW_ACCESS, &call)) return E_OK;
    EXPECT_SBS_GO(&t->e,
        "dir_rw_access(path=)", path, call.arg.dir_rw_access, done
    );

    *ret = call.resp.dir_rw_access;

done:
    if(is_error(t->e)) LOG_ERROR("failure occured\n");
    return E_OK;
}

static derr_t fake_write_inst(
    configure_i *iface, installation_t inst, string_builder_t path
){
    (void)inst;
    test_t *t = CONTAINER_OF(iface, test_t, iface);
    call_t call;
    if(!expect_call(t, WRITE_INST, &call)) return E_OK;
    EXPECT_SBS_GO(&t->e, "write_inst(path=)", path, call.arg.write_inst, done);

done:
    if(is_error(t->e)) LOG_ERROR("failure occured\n");
    return E_OK;
}

static void add_call(test_t *t, call_e type, call_u arg, resp_u resp){
    size_t cap = sizeof(t->calls) / sizeof(*t->calls);
    if(t->nexp >= cap){
        for(size_t i = 0; i < t->nexp; i++){
            LOG_ERROR("- %x\n", FS(call_type_names[t->calls[i].type]));
        }
        LOG_FATAL("too many expected calls (%x)!", FS(call_type_names[type]));
    }
    t->calls[t->nexp++] = (call_t){type, arg, resp};
}
#define CALL_U(...) (call_u){__VA_ARGS__}
#define RESP_U(...) (resp_u){__VA_ARGS__}

#define EXPECT_IO_INIT(t, sc) \
    add_call(t, IO_INIT, CALL_U(.io_init = sc), RESP_U(0))
#define EXPECT_IO_CLOSE(t, wait) \
    add_call(t, IO_CLOSE, CALL_U(0), RESP_U(.io_close = wait))
#define EXPECT_SC_CHECK(t) \
    add_call(t, SC_CHECK, CALL_U(0), RESP_U(0))
#define EXPECT_ACME_DIR(t) \
    add_call(t, ACME_DIR, CALL_U(0), RESP_U(0))
#define EXPECT_ADD_INST(t, u, p) \
    add_call(t, ADD_INST, CALL_U(.add_inst={.user=u, .pass=p}), RESP_U(0))
#define EXPECT_PRINT(t, text) \
    add_call(t, PRINT, CALL_U(.print = text), RESP_U(0))
#define EXPECT_PROMPT_ONE_OF(t, m, o, ret) \
    add_call(t, \
        PROMPT_ONE_OF, \
        CALL_U(.prompt_one_of={.msg=m, .opts=o}), \
        RESP_U(.prompt_one_of=ret) \
    )
#define EXPECT_USER_PROMPT(t, m, h, ret) \
    add_call(t, \
        USER_PROMPT, \
        CALL_U(.user_prompt={.msg=m, .hide=h}), \
        RESP_U(.user_prompt=ret) \
    )
#define EXPECT_MKDIRS(t, path, err) \
    add_call(t, MKDIRS, CALL_U(.mkdirs = path), RESP_U(.mkdirs = err))
#define EXPECT_DIR_RW_ACCESS(t, path, ok) \
    add_call(t, \
        DIR_RW_ACCESS, \
        CALL_U(.dir_rw_access = path), \
        RESP_U(.dir_rw_access = ok) \
    )
#define EXPECT_WRITE_INST(t, path) \
    add_call(t, WRITE_INST, CALL_U(.write_inst = path), RESP_U(0))

static derr_t tinit(
    test_t *t,
    char *smdir,
    char *user,
    bool force
){
    derr_t e = E_OK;

    *t = (test_t){
        .iface = {
            .io_init = fake_io_init,
            .io_close = fake_io_close,
            .status_client_check = fake_status_client_check,
            .get_acme_dir = fake_get_acme_dir,
            .add_inst = fake_add_inst,
            .print = fake_print,
            .prompt_one_of = fake_prompt_one_of,
            .user_prompt = fake_user_prompt,
            .mkdirs = fake_mkdirs,
            .dir_rw_access = fake_dir_rw_access,
            .write_inst = fake_write_inst,
        },
        .retval = 98,
    };
    scheduler_i *sched = manual_scheduler(&t->m);

    PROP(&e,
        configure_init(
            &t->c,
            &t->iface,
            sched,
            SBS(smdir),
            dstr_from_cstr(user),
            force,
            &t->retval
        )
    );

    advancer_schedule(&t->c.advancer, E_OK);

    t->started = true;

    return e;
}

static derr_t trun(test_t *t){
    derr_t e = E_OK;

    manual_scheduler_run(&t->m);

    // check for existing error
    PROP_VAR(&e, &t->e);
    // ensure we made the right number of calls
    if(t->ngot < t->nexp){
        for(size_t i = t->ngot; i < t->nexp; i++){
            TRACE(&e,
                "expected %x call but didn't see it\n",
                FS(call_type_names[t->calls[i].type])
            );
        }
        ORIG(&e, E_VALUE, "missing expected calls");
    }
    // reset for next call
    t->ngot = 0;
    t->nexp = 0;

    return e;
}

static derr_t tfinish(test_t *t, derr_type_t etype, int retval){
    derr_t e = E_OK;

    EXPECT_IO_CLOSE(t, false);
    PROP(&e, trun(t) );
    EXPECT_B(&e, "down_done", t->c.advancer.down_done, true);
    EXPECT_E_VAR(&e, "c.advancer.e", &t->c.advancer.e, etype);
    EXPECT_I(&e, "retval", t->retval, retval);

    return e;
}

static void tforceclose(test_t *t){
    if(!t->started) return;
    t->forceclose = true;
    derr_t err = { .type = E_CANCELED };
    advancer_schedule(&t->c.advancer, err);
    manual_scheduler_run(&t->m);
    if(!t->c.advancer.down_done){
        LOG_FATAL("configure refused to exit\n");
    }
    if(t->c.advancer.e.type == E_CANCELED){
        DROP_VAR(&t->c.advancer.e);
    }
}

#define MKSTATUS(s, dom, smaj, smin, conf, rdy) \
    citm_status_init(s, \
        SPLINTERMAIL_VERSION_MAJOR, \
        SPLINTERMAIL_VERSION_MINOR, \
        SPLINTERMAIL_VERSION_PATCH, \
        dstr_from_cstr(dom), \
        dstr_from_cstr(smaj), \
        dstr_from_cstr(smin), \
        conf, \
        rdy \
    )

static derr_t test_configure(void){
    derr_t e = E_OK;

    test_t t;
    citm_status_t status;

    // what if we don't have smdir access?
    PROP_GO(&e, tinit(&t, "sm", NULL, false), cu);
    EXPECT_DIR_RW_ACCESS(&t, "sm", false);
    EXPECT_PRINT(&t, "--splintermail-dir either does not exist or");
    PROP_GO(&e, tfinish(&t, E_NONE, 19), cu);

    // what if .configured is n/a? (.tls_ready must not matter)
    for(tri_e tri = TRI_NO; tri <= TRI_NA; tri++){
        PROP_GO(&e, tinit(&t, "sm", NULL, false), cu);
        EXPECT_DIR_RW_ACCESS(&t, "sm", true);
        EXPECT_IO_INIT(&t, true);
        PROP_GO(&e, trun(&t), cu);
        PROP_GO(&e, MKSTATUS(&status, "d", "j", "n", TRI_NA, tri), cu);
        configure_status_client_update(&t.c, status);
        EXPECT_PRINT(&t, "splintermail does not need an ACME configuration");
        PROP_GO(&e, tfinish(&t, E_NONE, 20), cu);
    }

    // what if we're already configured, and already have a cert?
    PROP_GO(&e, tinit(&t, "sm", NULL, false), cu);
    EXPECT_DIR_RW_ACCESS(&t, "sm", true);
    EXPECT_IO_INIT(&t, true);
    PROP_GO(&e, trun(&t), cu);
    PROP_GO(&e, MKSTATUS(&status, "dom", "maj", "min", TRI_YES, TRI_YES), cu);
    configure_status_client_update(&t.c, status);
    EXPECT_PRINT(&t, "server already ready.");
    PROP_GO(&e, tfinish(&t, E_NONE, 0), cu);

    // what if we're already configured, but don't have a cert yet?
    PROP_GO(&e, tinit(&t, "sm", NULL, false), cu);
    EXPECT_DIR_RW_ACCESS(&t, "sm", true);
    EXPECT_IO_INIT(&t, true);
    PROP_GO(&e, trun(&t), cu);
    PROP_GO(&e, MKSTATUS(&status, "dom", "maj", "min", TRI_YES, TRI_NO), cu);
    configure_status_client_update(&t.c, status);
    EXPECT_PRINT(&t, "server already configured.");
    EXPECT_PRINT(&t, "waiting for server to become ready...");
    PROP_GO(&e, trun(&t), cu);
    // provide a non-completed status update
    PROP_GO(&e, MKSTATUS(&status, "dom", "abc", "xyz", TRI_YES, TRI_NO), cu);
    configure_status_client_update(&t.c, status);
    EXPECT_PRINT(&t, "splintermail is abc: xyz");
    PROP_GO(&e, trun(&t), cu);
    // provide a completed status update
    PROP_GO(&e, MKSTATUS(&status, "dom", "maj", "min", TRI_YES, TRI_YES), cu);
    configure_status_client_update(&t.c, status);
    EXPECT_PRINT(&t, "server configured.");
    PROP_GO(&e, tfinish(&t, E_NONE, 0), cu);

    // what if we're starting totally fresh?
    // i = 0: reject tos
    // i = 1: provide user ahead of time
    // i = 2: provide user at runtime
    for(size_t i = 0; i < 3; i++){
        char *user = i == 1 ? "joeusr" : NULL;
        PROP_GO(&e, tinit(&t, "sm", user, false), cu);
        EXPECT_DIR_RW_ACCESS(&t, "sm", true);
        EXPECT_IO_INIT(&t, true);
        PROP_GO(&e, trun(&t), cu);
        PROP_GO(&e, MKSTATUS(&status, "d", "j", "n", TRI_NO, TRI_NO), cu);
        configure_status_client_update(&t.c, status);
        EXPECT_ACME_DIR(&t);
        PROP_GO(&e, trun(&t), cu);
        jdump_i *metadata_resp =
            DOBJ(DKEY("meta", DOBJ(DKEY("termsOfService", DS("q.com")))));
        PROP_GO(&e, jdump(metadata_resp, WD(t.acme_dir_resp), 0), cu);
        configure_get_acme_dir_done(&t.c, E_OK);
        EXPECT_PRINT(&t, "Read the Let's Encrypt Terms of Service: q.com");
        if(i == 0){
            // reject tos
            EXPECT_PROMPT_ONE_OF(&t, "agree to the Let's Encrypt", "yn", 'n');
            EXPECT_PRINT(&t, "configuration canceled");
            PROP_GO(&e, tfinish(&t, E_NONE, 22), cu);
            continue;
        }
        // accept tos
        EXPECT_PROMPT_ONE_OF(&t, "agree to the Let's Encrypt", "yn", 'y');
        EXPECT_PRINT(&t, "register a new installation");
        if(i != 1){
            EXPECT_USER_PROMPT(&t, "email address:", false, "joeusr");
        }
        EXPECT_USER_PROMPT(&t, "password:", true, "joepwd");
        EXPECT_PRINT(&t, "registering installation...");
        EXPECT_ADD_INST(&t, "joeusr", "joepwd");
        PROP_GO(&e, trun(&t), cu);
        // prep an api response
        jdump_i *apiresp = DOBJ(
            DKEY("status", DS("success")),
            DKEY("contents", DOBJ(
                DKEY("token", DI(777)),
                DKEY("secret", DS("randombytes")),
                DKEY("subdomain", DS("xyz")),
                DKEY("email", DS("xyz@acme.splintermail.com")),
            ))
        );
        DSTR_VAR(apibuf, 256);
        PROP_GO(&e, jdump(apiresp, WD(&apibuf), 2), cu);
        PROP_GO(&e, json_parse(apibuf, t.add_inst_resp), cu);
        configure_add_inst_done(&t.c, E_OK);
        EXPECT_PRINT(&t, "saving installation file...");
        EXPECT_WRITE_INST(&t, "sm/acme/installation.json");
        EXPECT_SC_CHECK(&t);
        EXPECT_PRINT(&t, "waiting for server to become ready...");
        PROP_GO(&e, trun(&t), cu);
        PROP_GO(&e, MKSTATUS(&status, "d", "j", "n", TRI_YES, TRI_YES), cu);
        configure_status_client_update(&t.c, status);
        EXPECT_PRINT(&t, "server configured.");
        PROP_GO(&e, tfinish(&t, E_NONE, 0), cu);
    }

cu:
    tforceclose(&t);
    if(is_error(e) && is_error(t.c.advancer.e)){
        LOG_ERROR("c.advancer.e error will be dropped:\n");
        DUMP(t.c.advancer.e);
        LOG_ERROR("-- end c.advancer.e error --\n");
    }
    TRACE_MULTIPROP_VAR(&e, &t.c.advancer.e);

    return e;
}

static derr_t test_configure_force(void){
    derr_t e = E_OK;

    test_t t;

    // what if we don't have smdir access?
    PROP_GO(&e, tinit(&t, "sm", NULL, true), cu);
    EXPECT_MKDIRS(&t, "sm/acme", E_FS);
    PROP_GO(&e, tfinish(&t, E_FS, 98), cu);

    // normal --force workflow
    // i = 0: reject tos
    // i = 1: provide user ahead of time
    // i = 2: provide user at runtime
    for(size_t i = 0; i < 2; i++){
        char *user = i == 1 ? "joeusr" : NULL;
        PROP_GO(&e, tinit(&t, "sm", user, true), cu);
        EXPECT_MKDIRS(&t, "sm/acme", E_NONE);
        EXPECT_IO_INIT(&t, false);
        EXPECT_ACME_DIR(&t);
        PROP_GO(&e, trun(&t), cu);
        jdump_i *metadata_resp =
            DOBJ(DKEY("meta", DOBJ(DKEY("termsOfService", DS("q.com")))));
        PROP_GO(&e, jdump(metadata_resp, WD(t.acme_dir_resp), 0), cu);
        configure_get_acme_dir_done(&t.c, E_OK);
        EXPECT_PRINT(&t, "Read the Let's Encrypt Terms of Service: q.com");
        if(i == 0){
            // reject tos
            EXPECT_PROMPT_ONE_OF(&t, "agree to the Let's Encrypt", "yn", 'n');
            EXPECT_PRINT(&t, "configuration canceled");
            PROP_GO(&e, tfinish(&t, E_NONE, 22), cu);
            continue;
        }
        // accept tos
        EXPECT_PROMPT_ONE_OF(&t, "agree to the Let's Encrypt", "yn", 'y');
        EXPECT_PRINT(&t, "register a new installation");
        if(i != 1){
            EXPECT_USER_PROMPT(&t, "email address:", false, "joeusr");
        }
        EXPECT_USER_PROMPT(&t, "password:", true, "joepwd");
        EXPECT_PRINT(&t, "registering installation...");
        EXPECT_ADD_INST(&t, "joeusr", "joepwd");
        PROP_GO(&e, trun(&t), cu);
        // prep an api response
        jdump_i *apiresp = DOBJ(
            DKEY("status", DS("success")),
            DKEY("contents", DOBJ(
                DKEY("token", DI(777)),
                DKEY("secret", DS("randombytes")),
                DKEY("subdomain", DS("xyz")),
                DKEY("email", DS("xyz@acme.splintermail.com")),
            ))
        );
        DSTR_VAR(apibuf, 256);
        PROP_GO(&e, jdump(apiresp, WD(&apibuf), 2), cu);
        PROP_GO(&e, json_parse(apibuf, t.add_inst_resp), cu);
        configure_add_inst_done(&t.c, E_OK);
        EXPECT_PRINT(&t, "saving installation file...");
        EXPECT_WRITE_INST(&t, "sm/acme/installation.json");
        PROP_GO(&e, tfinish(&t, E_NONE, 0), cu);
    }

cu:
    tforceclose(&t);
    if(is_error(e) && is_error(t.c.advancer.e)){
        LOG_ERROR("c.advancer.e error will be dropped:\n");
        DUMP(t.c.advancer.e);
        LOG_ERROR("-- end c.advancer.e error --\n");
    }
    TRACE_MULTIPROP_VAR(&e, &t.c.advancer.e);

    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;
    int exit_code = 1;

    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_configure(), cu);
    PROP_GO(&e, test_configure_force(), cu);

    exit_code = 0;

cu:
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
    }

    LOG_ERROR(exit_code ? "FAIL\n" : "PASS\n");
    return exit_code;
}
