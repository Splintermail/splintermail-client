#include "libdstr/libdstr.h"
#include "libweb/libweb.h"
#include "libduv/libduv.h"
#include "libhttp/libhttp.h"
#include "libacme/libacme.h"
#include "libacme/pebble.h"

#include <string.h>

#include <openssl/pem.h>

typedef struct {
    duv_http_t *http;
    acme_t **acme;
    dstr_t order;
    EVP_PKEY *pkey;
    acme_account_t acct;
    dstr_t finalize;
    dstr_t domain;
    bool success;
    derr_t e;
} globals_t;

static void _finalize_cb(void *data, derr_t err, dstr_t cert){
    derr_t e = E_OK;

    globals_t *g = data;

    PROP_VAR_GO(&e, &err, done);

    // dump cert and exit
    PROP_GO(&e, FFMT(stdout, "%x", FD(cert)), done);

    g->success = true;

done:
    dstr_free(&cert);
    acme_close(*g->acme, NULL);
    duv_http_close(g->http, NULL);
    TRACE_PROP_VAR(&g->e, &e);
}

static void _get_order_cb(
    void *data,
    derr_t err,
    dstr_t domain,
    dstr_t status,
    dstr_t expires,
    dstr_t authorization,
    dstr_t finalize,
    dstr_t certurl,
    time_t retry_after
){
    derr_t e = E_OK;

    globals_t *g = data;

    bool in_prog = dstr_eq(status, DSTR_LIT("processing"))
                || dstr_eq(status, DSTR_LIT("valid"));

    // discard
    dstr_free(&status);
    dstr_free(&expires);
    dstr_free(&authorization);
    dstr_free(&certurl);
    // keep
    g->finalize = finalize;
    g->domain = domain;

    PROP_VAR_GO(&e, &err, fail);

    if(!in_prog){
        // get started finalizing
        acme_finalize(
            g->acct, g->order, g->finalize, g->domain, g->pkey, _finalize_cb, g
        );
    }else{
        // finish an in-progress finalize
        acme_finalize_continue(
            g->acct, g->order, retry_after, _finalize_cb, g
        );
    }

    return;

fail:
    acme_close(*g->acme, NULL);
    duv_http_close(g->http, NULL);
    TRACE_PROP_VAR(&g->e, &e);
}

static derr_t finalize(
    dstr_t directory,
    char *acct_file,
    const dstr_t order,
    EVP_PKEY *pkey,
    SSL_CTX *ctx
){
    derr_t e = E_OK;

    uv_loop_t loop = {0};
    duv_scheduler_t scheduler = {0};
    duv_http_t http = {0};
    acme_t *acme = NULL;
    globals_t g = { &http, &acme, order, pkey};

    PROP(&e, duv_loop_init(&loop) );

    PROP_GO(&e, duv_scheduler_init(&scheduler, &loop), fail);

    PROP_GO(&e, duv_http_init(&http, &loop, &scheduler, ctx), fail);

    PROP_GO(&e, acme_new(&acme, &http, directory, NULL), fail);

    PROP_GO(&e, acme_account_from_file(&g.acct, acct_file, acme), fail);

    // get finalize url and domain from get_order() first
    acme_get_order(g.acct, order, _get_order_cb, &g);

    derr_t e2 = duv_run(&loop);
    TRACE_PROP_VAR(&e, &e2);
    TRACE_PROP_VAR(&e, &g.e);
    if(!is_error(e) && !g.success){
        TRACE_ORIG(&e, E_INTERNAL, "finished but did not succeed");
    }

fail:
    acme_account_free(&g.acct);
    acme_free(&acme);
    duv_http_close(&http, NULL);
    DROP_CMD( duv_run(&loop) );

    duv_scheduler_close(&scheduler);
    uv_loop_close(&loop);
    DROP_CMD( duv_run(&loop) );
    dstr_free(&g.finalize);
    dstr_free(&g.domain);
    return e;
}

static void print_help(void){
    fprintf(stdout,
        "finalize: finalize an ACME order\n"
        "\n"
        "usage: finalize [OPTIONS] ACCOUNT.JSON ORDER KEYOUT > cert.pem\n"
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
    CATCH(e2, E_ANY){
        DROP_VAR(&e2);
        fprintf(stderr, "try `%s --help` for usage\n", argv[0]);
        return 1;
    }

    // help option
    if(o_help.found){
        print_help();
        return 0;
    }

    if(newargc < 4){
        fprintf(stderr, "try `%s --help` for usage\n", argv[0]);
        return 1;
    }

    char *acct = argv[1];
    dstr_t order = dstr_from_cstr(argv[2]);
    const char *keyout = argv[3];

    dstr_t directory = DSTR_LIT(LETSENCRYPT);
    if(o_pebble.found) directory = DSTR_LIT("https://localhost:14000/dir");
    if(o_dir.found) directory = o_dir.val;

    const char *ca = o_ca.found ? o_ca.val.data : NULL;

    ssl_context_t ssl_ctx = {0};
    EVP_PKEY *pkey = NULL;

    PROP_GO(&e, ssl_library_init(), fail);

    // write a new key
    PROP_GO(&e, gen_key(2048, keyout), fail);

    // read it back
    FILE *f;
    PROP_GO(&e, dfopen(keyout, "r", &f), fail);
    pkey = PEM_read_PrivateKey(f, NULL, NULL, NULL);
    fclose(f);
    if(!pkey) ORIG_GO(&e, E_SSL, "PEM_read_PrivateKey: %x\n", fail, FSSL);

    PROP_GO(&e, ssl_context_new_client_ex(&ssl_ctx, true, &ca, !!ca), fail);

    if(o_pebble.found) PROP_GO(&e, trust_pebble(ssl_ctx.ctx), fail);

    PROP_GO(&e, finalize(directory, acct, order, pkey, ssl_ctx.ctx), fail);

fail:
    (void)main;
    int retval = 0;
    if(is_error(e)){
        retval = 1;
        DUMP(e);
        DROP_VAR(&e);
    }

    ssl_context_free(&ssl_ctx);

    if(pkey) EVP_PKEY_free(pkey);

    ssl_library_close();

    return retval;
}
