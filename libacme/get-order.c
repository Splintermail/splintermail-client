#include "libdstr/libdstr.h"
#include "libweb/libweb.h"
#include "libduv/libduv.h"
#include "libhttp/libhttp.h"
#include "libacme/libacme.h"

#include <string.h>

static void http_close_cb(duv_http_t *http){
    (void)http;
}

typedef struct {
    duv_http_t *http;
    bool success;
    derr_t e;
} globals_t;

static void _get_order_cb(
    void *data,
    derr_t err,
    dstr_t domain,
    dstr_t status,
    dstr_t expires,
    dstr_t authorization,
    dstr_t finalize
){
    derr_t e = E_OK;

    globals_t *g = data;

    PROP_VAR_GO(&e, &err, done);

    // dump key info and exit
    jdump_i *obj =  DOBJ(
        DKEY("domain", DD(domain)),
        DKEY("status", DD(status)),
        DKEY("expires", DD(expires)),
        DKEY("authorization", DD(authorization)),
        DKEY("finalize", DD(finalize)),
    );
    PROP_GO(&e, jdump(obj, WF(stdout), 2), done);

    g->success = true;

done:
    dstr_free(&domain);
    dstr_free(&status);
    dstr_free(&expires);
    dstr_free(&authorization);
    dstr_free(&finalize);
    duv_http_close(g->http, http_close_cb);
    TRACE_PROP_VAR(&g->e, &e);
}

static derr_t get_order(
    dstr_t directory,
    char *acct_file,
    const dstr_t order,
    SSL_CTX *ctx
){
    derr_t e = E_OK;

    uv_loop_t loop = {0};
    duv_scheduler_t scheduler = {0};
    duv_http_t http = {0};
    acme_t *acme = NULL;
    acme_account_t acct = {0};
    globals_t g = { &http };

    PROP(&e, duv_loop_init(&loop) );

    PROP_GO(&e, duv_scheduler_init(&scheduler, &loop), fail);

    PROP_GO(&e, duv_http_init(&http, &loop, &scheduler, ctx), fail);

    PROP_GO(&e, acme_new(&acme, &http, directory, NULL), fail);

    PROP_GO(&e, acme_account_from_file(&acct, acct_file, acme), fail);

    acme_get_order(acct, order, _get_order_cb, &g);

    derr_t e2 = duv_run(&loop);
    TRACE_PROP_VAR(&e, &e2);
    TRACE_PROP_VAR(&e, &g.e);
    if(!is_error(e) && !g.success){
        TRACE_ORIG(&e, E_INTERNAL, "finished but did not succeed");
    }

fail:
    acme_account_free(&acct);
    acme_free(&acme);
    duv_http_close(&http, http_close_cb);
    DROP_CMD( duv_run(&loop) );

    duv_scheduler_close(&scheduler);
    uv_loop_close(&loop);
    DROP_CMD( duv_run(&loop) );
    return e;
}

static void print_help(void){
    fprintf(stdout,
        "get-order: get an existing ACME order\n"
        "\n"
        "usage: get-order [OPTIONS] ACCOUNT.JSON ORDER > order.json\n"
        "\n"
        "where OPTIONS may contain:\n"
        "\n"
        "  -d --dir [URL]  Set the acme directory to URL.\n"
        "                  Default: " LETSENCRYPT "\n"
        "     --ca [PATH]  Include a certificate authority from PATH.\n"
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

    opt_spec_t* spec[] = { &o_help, &o_dir, &o_ca };
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

    if(newargc < 3){
        fprintf(stderr, "try `%s --help` for usage\n", argv[0]);
        return 1;
    }

    char *acct = argv[1];
    dstr_t order = dstr_from_cstr(argv[2]);

    dstr_t directory = o_dir.found ? o_dir.val : DSTR_LIT(LETSENCRYPT);
    const char *ca = o_ca.found ? o_ca.val.data : NULL;

    ssl_context_t ssl_ctx = {0};

    PROP_GO(&e, ssl_library_init(), fail);

    PROP_GO(&e, ssl_context_new_client_ex(&ssl_ctx, true, &ca, !!ca), fail);

    PROP_GO(&e, get_order(directory, acct, order, ssl_ctx.ctx), fail);

fail:
    (void)main;
    int retval = 0;
    if(is_error(e)){
        retval = 1;
        DUMP(e);
        DROP_VAR(&e);
    }

    ssl_context_free(&ssl_ctx);

    ssl_library_close();

    return retval;
}
