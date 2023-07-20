#include "libdstr/libdstr.h"
#include "libweb/libweb.h"
#include "libduv/libduv.h"
#include "libhttp/libhttp.h"
#include "libacme/libacme.h"
#include "libacme/pebble.h"

#include <string.h>

typedef struct {
    duv_http_t *http;
    acme_t **acme;
    bool success;
    derr_t e;
} globals_t;

static derr_t parse_timestamp(dstr_t d, time_t *out){
    derr_t e = E_OK;

    // calculate seconds offset between localtime and UTC with stdlib functions
    static time_t offset = LONG_MAX;
    if(offset == LONG_MAX){
        // get current epoch seconds
        time_t now;
        PROP(&e, dtime(&now) );
        // convert to utc calendar time
        struct tm utc;
        #ifdef _WIN32
        gmtime_s(&utc, &now);
        #else
        gmtime_r(&now, &utc);
        #endif
        // convert utc calendar time to epoch time, to measure offset
        offset = now - mktime(&utc);
    }

    DSTR_VAR(buf, 32);
    PROP(&e, dstr_copy(&d, &buf) );
    PROP(&e, dstr_null_terminate(&buf) );

    // example time: 2000-01-12T13:14:15Z
    struct tm tm = {0};
    int ret = sscanf(
        buf.data,
        "%d-%d-%dT%d:%d:%dZ",
        &tm.tm_year,
        &tm.tm_mon,
        &tm.tm_mday,
        &tm.tm_hour,
        &tm.tm_min,
        &tm.tm_sec
    );
    if(ret != 6) ORIG(&e, E_PARAM, "unable to parse timestamp: %x\n", FD(d));

    // year 0 is 1900
    tm.tm_year -= 1900;
    // month 0 is january
    tm.tm_mon -= 1;

    // get the timestamp in epoch seconds (as if the time were local)
    time_t stamp = mktime(&tm);
    if(stamp == (time_t)-1){
        ORIG(&e, E_PARAM, "mktime(%x): %x\n", FD(d), FE(errno));
    }

    // apply offset
    *out = stamp + offset;

    return e;
}

static void _new_order_cb(
    void *data,
    derr_t err,
    dstr_t order,
    dstr_t expires,
    dstr_t authorization,
    dstr_t finalize
){
    derr_t e = E_OK;

    globals_t *g = data;

    PROP_VAR_GO(&e, &err, done);

    jdump_i *obj =  DOBJ(
        DKEY("order", DD(order)),
        DKEY("expires", DD(expires)),
        DKEY("authorization", DD(authorization)),
        DKEY("finalize", DD(finalize)),
    );
    PROP_GO(&e, jdump(obj, WF(stdout), 2), done);

    time_t now;
    PROP_GO(&e, dtime(&now), done);
    time_t stamp;
    PROP_GO(&e, parse_timestamp(expires, &stamp), done);

    FFMT(stderr,
        "now = %x, stamp = %x, diff=%x\n", FI(now), FI(stamp), FI(stamp - now)
    );

    g->success = true;

done:
    dstr_free(&order);
    dstr_free(&expires);
    dstr_free(&authorization);
    dstr_free(&finalize);
    acme_close(*g->acme, NULL);
    duv_http_close(g->http, NULL);
    TRACE_PROP_VAR(&g->e, &e);
}


static derr_t new_order(
    dstr_t directory,
    char *acct_file,
    dstr_t domain,
    SSL_CTX *ctx
){
    derr_t e = E_OK;

    uv_loop_t loop = {0};
    duv_scheduler_t scheduler = {0};
    duv_http_t http = {0};
    acme_t *acme = NULL;
    acme_account_t acct = {0};
    globals_t g = { &http, &acme };

    PROP(&e, duv_loop_init(&loop) );

    PROP_GO(&e, duv_scheduler_init(&scheduler, &loop), fail);

    PROP_GO(&e, duv_http_init(&http, &loop, &scheduler, ctx), fail);

    PROP_GO(&e, acme_new(&acme, &http, directory, NULL), fail);

    PROP_GO(&e, acme_account_from_file(&acct, acct_file, acme), fail);

    acme_new_order(acct, domain, _new_order_cb, &g);

    derr_t e2 = duv_run(&loop);
    TRACE_PROP_VAR(&e, &e2);
    TRACE_PROP_VAR(&e, &g.e);
    if(!is_error(e) && !g.success){
        TRACE_ORIG(&e, E_INTERNAL, "finished but did not succeed");
    }

fail:
    acme_account_free(&acct);
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
        "new-order: create a new ACME order\n"
        "\n"
        "usage: new-order [OPTIONS] ACCOUNT.JSON DOMAIN > order.json\n"
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

    if(newargc < 3){
        fprintf(stderr, "try `%s --help` for usage\n", argv[0]);
        return 1;
    }

    char *acct = argv[1];
    dstr_t domain = dstr_from_cstr(argv[2]);

    dstr_t directory = DSTR_LIT(LETSENCRYPT);
    if(o_pebble.found) directory = DSTR_LIT("https://localhost:14000/dir");
    if(o_dir.found) directory = o_dir.val;

    const char *ca = o_ca.found ? o_ca.val.data : NULL;

    ssl_context_t ssl_ctx = {0};

    PROP_GO(&e, ssl_library_init(), fail);

    PROP_GO(&e, ssl_context_new_client_ex(&ssl_ctx, true, &ca, !!ca), fail);

    if(o_pebble.found) PROP_GO(&e, trust_pebble(ssl_ctx.ctx), fail);

    // create the order
    PROP_GO(&e, new_order(directory, acct, domain, ssl_ctx.ctx), fail);

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
