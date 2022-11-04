#include "libdstr/libdstr.h"
#include "liburl/liburl.h"
#include "libduv/libduv.h"
#include "libhttp/libhttp.h"

#include "server/acme/libacme.h"

#include <string.h>

#include <openssl/ec.h>
#include <openssl/evp.h>

#define EAB_URL "https://api.zerossl.com/acme/eab-credentials"

static uv_loop_t loop;
static duv_scheduler_t scheduler;
static duv_http_t http;
static duv_http_req_t req;
derr_t E = E_OK;
static stream_reader_t reader;
static bool have_status = false;
int status_code = 0;
bool success = false;
DSTR_VAR(rbuf, 4096);

static void http_close_cb(duv_http_t *http){
    (void)http;
}

static void hdr_cb(duv_http_req_t *req, const http_pair_t hdr){
    if(!have_status){
        have_status = true;
        LOG_INFO("HTTP/1.1 %x %x\n", FI(req->status), FD(&req->reason));
        status_code = req->status;
    }
    LOG_INFO("%x: %x\n", FD(&hdr.key), FD(&hdr.value));
}

static derr_t post_process(void){
    derr_t e = E_OK;

    // post-process
    DSTR_VAR(jtext, 4096);
    json_node_t nodemem[32];
    size_t nnodes = sizeof(nodemem) / sizeof(*nodemem);
    json_t json;
    json_prep_preallocated(&json, &jtext, nodemem, nnodes, true);

    PROP(&e, json_parse(rbuf, &json) );

    bool success;
    dstr_t kid;
    dstr_t hmac_key;
    jspec_t *jspec = JOBJ(false,
        JKEY("eab_hmac_key", JDREF(&hmac_key)),
        JKEY("eab_kid", JDREF(&kid)),
        JKEY("success", JB(&success)),
    );
    PROP(&e, jspec_read(jspec, json.root) );

    if(!success){
        ORIG(&e, E_RESPONSE, "response.success != true");
    }

    // key must be base64url-encoded
    DSTR_VAR(bin, 4096);
    derr_t e2 = b64url2bin(hmac_key, &bin);
    CATCH(e2, E_ANY){
        TRACE(&e2, "hmac key failed base64url decoding\n");
        RETHROW(&e, &e2, E_RESPONSE);
    }

    // now print the final output
    PROP(&e,
        FFMT(stdout, NULL,
            "{\"eab_kid\":\"%x\",\"eab_hmac_key\":\"%x\"}\n",
            FD_JSON(&kid),
            FD_JSON(&hmac_key)
        )
    );

    return e;
}

static void done_cb(stream_reader_t *r, derr_t e){
    (void)r;
    LOG_INFO("\n%x\n", FD(&rbuf));
    if(is_error(e)){
        PROP_VAR_GO(&E, &e, done);
    }
    if(status_code != 200){
        ORIG_GO(&E, E_VALUE, "non-200 status code", done);
    }

    PROP_GO(&E, post_process(), done);

    success = true;

done:
    duv_http_close(&http, http_close_cb);
    return;
}

static derr_t get_eab(const dstr_t account_key){
    derr_t e = E_OK;

    ssl_context_t client_ctx;
    PROP(&e, ssl_context_new_client(&client_ctx) );

    PROP_GO(&e, duv_loop_init(&loop), cu_ctx);

    PROP_GO(&e, duv_scheduler_init(&scheduler, &loop), fail_loop);

    PROP_GO(&e, duv_http_init(&http, &loop, &scheduler, NULL), fail_scheduler);

    // start our request
    http_pairs_t params = {
        .pair = { .key = DSTR_LIT("access_key"), .value = account_key }
    };
    rstream_i *r = duv_http_req(
        &req,
        &http,
        HTTP_METHOD_POST,
        must_parse_url(&DSTR_LIT(EAB_URL)),
        &params,
        NULL,
        (dstr_t){0},
        hdr_cb
    );
    stream_read_all(&reader, r, &rbuf, done_cb);

    derr_t e2 = duv_run(&loop);
    TRACE_PROP_VAR(&e, &e2);
    TRACE_PROP_VAR(&e, &E);

    duv_scheduler_close(&scheduler);
    uv_loop_close(&loop);
    goto cu_ctx;

fail_scheduler:
    duv_scheduler_close(&scheduler);
fail_loop:
    uv_loop_close(&loop);
    DROP_CMD( duv_run(&loop) );

cu_ctx:
    ssl_context_free(&client_ctx);
    return e;
}

static void print_help(void){
    fprintf(stdout,
        "get-eab: fetch externalAccountBinding from zerossl\n"
        "\n"
        "usage: get-eab ACCESS_KEY > eab\n"
        "\n"
    );
}

int main(int argc, char **argv){
    derr_t e = E_OK;
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    logger_add_fileptr(LOG_LVL_INFO, stderr);

    opt_spec_t o_help  = {'h', "help", false, OPT_RETURN_INIT};

    opt_spec_t* spec[] = {
        &o_help,
    };
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

    // require 1 positional argument
    if(newargc != 2){
        fprintf(stderr, "try `%s --help` for usage\n", argv[0]);
        return 1;
    }

    PROP_GO(&e, ssl_library_init(), fail);

    dstr_t account_key;
    DSTR_WRAP(account_key, argv[1], strlen(argv[1]), true);

    PROP_GO(&e, get_eab(account_key), fail);

fail:
    int retval = 0;
    if(is_error(e)){
        retval = 1;
        DUMP(e);
        DROP_VAR(&e);
    }

    ssl_library_close();

    return retval;
}
