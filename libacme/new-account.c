#include "libdstr/libdstr.h"
#include "libweb/libweb.h"
#include "libduv/libduv.h"
#include "libhttp/libhttp.h"
#include "libacme/libacme.h"

#include "test/certs.h"

#include <string.h>

typedef struct {
    duv_http_t *http;
    acme_t **acme;
    bool success;
    derr_t e;
} globals_t;

static void _new_account_cb(
    void *data, derr_t err, acme_account_t acct
){
    derr_t e = E_OK;

    globals_t *g = data;

    PROP_VAR_GO(&e, &err, done);

    // dump key info and exit
    jdump_i *obj =  DOBJ(
        DKEY("key", DJWKPVT(acct.key)),
        DKEY("kid", DD(acct.kid)),
        DKEY("orders", DD(acct.orders)),
    );
    PROP_GO(&e, jdump(obj, WF(stdout), 2), done);

    g->success = true;

done:
    acme_account_free(&acct);
    acme_close(*g->acme, NULL, NULL);
    duv_http_close(g->http, NULL);
    TRACE_PROP_VAR(&g->e, &e);
}

static derr_t new_account(
    dstr_t contact_email,
    key_i **key,
    dstr_t directory,
    char *verify_name,
    SSL_CTX *ctx
){
    derr_t e = E_OK;

    uv_loop_t loop = {0};
    duv_scheduler_t scheduler = {0};
    duv_http_t http = {0};
    acme_t *acme = NULL;
    globals_t g = { &http, &acme };

    PROP(&e, duv_loop_init(&loop) );

    PROP_GO(&e, duv_scheduler_init(&scheduler, &loop), fail);

    PROP_GO(&e, duv_http_init(&http, &loop, &scheduler, ctx), fail);

    PROP_GO(&e, acme_new_ex(&acme, &http, directory, verify_name), fail);

    // request a new account
    acme_new_account(acme, key, contact_email, _new_account_cb, &g);

    derr_t e2 = duv_run(&loop);
    TRACE_PROP_VAR(&e, &e2);
    TRACE_PROP_VAR(&e, &g.e);
    if(!is_error(e) && !g.success){
        TRACE_ORIG(&e, E_INTERNAL, "finished but did not succeed");
    }

fail:
    acme_free(&acme);
    duv_http_close(&http, NULL);
    DROP_CMD( duv_run(&loop) );
    duv_scheduler_close(&scheduler);
    uv_loop_close(&loop);
    DROP_CMD( duv_run(&loop) );
    return e;
}

static void print_help(void){
    fprintf(stdout,
        "new-account: create an ACME account\n"
        "\n"
        "usage: new-account [OPTIONS] CONTACT_EMAIL > account.json\n"
        "\n"
        "where OPTIONS may contain:\n"
        "\n"
        "  -d --dir [URL]  Set the acme directory to URL.\n"
        "                  Default: " LETSENCRYPT "\n"
        "     --ca [PATH]  Include a certificate authority from PATH.\n"
        "     --pebble     Trust pebble's certificate, and change default\n"
        "                  --dir to localhost:14000\n"
        "\n"
    );
}

int main(int argc, char **argv){
    derr_t e = E_OK;
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    logger_add_fileptr(LOG_LVL_INFO, stderr);

    opt_spec_t o_help = {'h', "help", false};
    opt_spec_t o_dir = {'d', "dir", true};
    opt_spec_t o_ca = {'\0', "ca", true};
    opt_spec_t o_pebble = {'\0', "pebble", false};

    opt_spec_t* spec[] = { &o_help, &o_dir, &o_ca, &o_pebble };
    size_t speclen = sizeof(spec) / sizeof(*spec);
    int newargc;

    // parse options
    derr_t e2 = opt_parse(argc, argv, spec, speclen, &newargc);
    CATCH_ANY(&e2){
        DROP_VAR(&e2);
        fprintf(stderr, "try `%s --help` for usage\n", argv[0]);
        return 1;
    }

    // help option
    if(o_help.found){
        print_help();
        return 0;
    }

    if(newargc < 2){
        fprintf(stderr, "try `%s --help` for usage\n", argv[0]);
        return 1;
    }

    char *contact_str = argv[1];

    dstr_t contact_email;
    DSTR_WRAP(contact_email, contact_str, strlen(contact_str), true);

    dstr_t directory = DSTR_LIT(LETSENCRYPT);
    if(o_pebble.found) directory = DSTR_LIT("https://localhost:14000/dir");
    if(o_dir.found) directory = o_dir.val;

    const char *ca = o_ca.found ? o_ca.val.data : NULL;

    ssl_context_t ssl_ctx = {0};
    key_i *k = NULL;

    PROP_GO(&e, ssl_library_init(), fail);

    PROP_GO(&e, ssl_context_new_client_ex(&ssl_ctx, true, &ca, !!ca), fail);

    char *verify_name = NULL;
    if(o_pebble.found){
        PROP_GO(&e, trust_pebble(ssl_ctx.ctx), fail);
        verify_name = "pebble";
    }

    // generate a new key
    PROP_GO(&e, gen_es256(&k), fail);

    PROP_GO(&e,
        new_account(contact_email, &k, directory, verify_name, ssl_ctx.ctx),
    fail);

fail:
    (void)main;
    int retval = 0;
    if(is_error(e)){
        retval = 1;
        DUMP(e);
        DROP_VAR(&e);
    }

    ssl_context_free(&ssl_ctx);
    if(k) k->free(k);

    ssl_library_close();

    return retval;
}
