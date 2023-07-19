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

static derr_type_t dump_orders(void *data, jdump_list_i *item){
    derr_type_t etype;
    LIST(dstr_t) *orders = data;
    for(size_t i = 0; i < orders->len; i++){
        etype = item->item(item, DD(orders->data[i]));
        if(etype) return etype;
    }
    return E_NONE;
}

static void _list_orders_cb(
    void *data, derr_t err, LIST(dstr_t) orders
){
    derr_t e = E_OK;

    globals_t *g = data;

    PROP_VAR_GO(&e, &err, done);

    // dump key info and exit
    jdump_i *list =  DLIST(dump_orders, &orders);
    PROP_GO(&e, jdump(list, WF(stdout), 2), done);

    g->success = true;

done:
    for(size_t i = 0; i < orders.len; i++){
        dstr_free(&orders.data[i]);
    }
    LIST_FREE(dstr_t, &orders);
    duv_http_close(g->http, http_close_cb);
    TRACE_PROP_VAR(&g->e, &e);
}

static derr_t list_orders(
    dstr_t directory,
    char *acct_file,
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

    acme_list_orders(acct, _list_orders_cb, &g);

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
        "list-orders: list ACME orders\n"
        "\n"
        "usage: list-orders [OPTIONS] ACCOUNT.JSON > orders.json\n"
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

    if(newargc < 2){
        fprintf(stderr, "try `%s --help` for usage\n", argv[0]);
        return 1;
    }

    char *acct = argv[1];

    dstr_t directory = o_dir.found ? o_dir.val : DSTR_LIT(LETSENCRYPT);
    const char *ca = o_ca.found ? o_ca.val.data : NULL;

    ssl_context_t ssl_ctx = {0};

    PROP_GO(&e, ssl_library_init(), fail);

    PROP_GO(&e, ssl_context_new_client_ex(&ssl_ctx, true, &ca, !!ca), fail);

    PROP_GO(&e, list_orders(directory, acct, ssl_ctx.ctx), fail);

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