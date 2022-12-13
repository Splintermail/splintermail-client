#include "libdstr/libdstr.h"
#include "liburl/liburl.h"
#include "libduv/libduv.h"
#include "libhttp/libhttp.h"

#include "server/acme/libacme.h"

#include <string.h>

#include <openssl/ec.h>
#include <openssl/evp.h>

static uv_loop_t loop;
static duv_scheduler_t scheduler;
static duv_http_t http;
derr_t E = E_OK;

typedef struct {
    scheduler_i *scheduler;
    schedulable_t schedulable;
    derr_t e;

    dstr_t contact_email;
    key_i *key;
    dstr_t eab_kid;
    dstr_t eab_hmac_key;
    dstr_t directory_url;

    // step 1: lookup urls
    bool have_urls;
    bool requested_urls;
    acme_urls_t urls;

    // step 2: get a nonce
    bool have_nonce;
    bool requested_nonce;
    dstr_t nonce;

    // step 3: create an account
    bool have_account;
    bool requested_account;
    dstr_t kid;
    dstr_t orders;

    bool success;
    bool closed;
} new_account_t;
DEF_CONTAINER_OF(new_account_t, schedulable, schedulable_t)

static void new_account_free(new_account_t *n){
    acme_urls_free(&n->urls);
    dstr_free(&n->nonce);
    dstr_free(&n->kid);
    dstr_free(&n->orders);
}

static void advance_state(new_account_t *n, derr_t e);

static void schedule_cb(schedulable_t *s){
    new_account_t *n = CONTAINER_OF(s, new_account_t, schedulable);
    advance_state(n, E_OK);
}

static void schedule(new_account_t *n){
    n->scheduler->schedule(n->scheduler, &n->schedulable);
}

static void new_account_prep(
    new_account_t *n,
    scheduler_i *scheduler,
    const dstr_t contact_email,
    key_i *key,
    const dstr_t eab_kid,
    const dstr_t eab_hmac_key,
    const dstr_t directory_url
){
    *n = (new_account_t){
        .scheduler = scheduler,
        .contact_email = contact_email,
        .key = key,
        .eab_kid = eab_kid,
        .eab_hmac_key = eab_hmac_key,
        .directory_url = directory_url,
    };
    schedulable_prep(&n->schedulable, schedule_cb);
}

static void http_close_cb(duv_http_t *http){
    (void)http;
}

static void _get_directory_cb(void *data, acme_urls_t urls, derr_t e){
    new_account_t *n = data;

    n->urls = urls;
    n->have_urls = true;

    if(is_error(e)) TRACE_PROP(&e);

    advance_state(n, e);
}

static void _new_nonce_cb(void *data, dstr_t nonce, derr_t e){
    new_account_t *n = data;

    n->nonce = nonce;
    n->have_nonce = true;

    if(is_error(e)) TRACE_PROP(&e);

    advance_state(n, e);
}

static void _post_new_account_cb(
    void *data, dstr_t kid, dstr_t orders, derr_t e
){
    new_account_t *n = data;

    n->kid = kid;
    n->orders = orders;
    n->have_account = true;

    if(is_error(e)) TRACE_PROP(&e);

    advance_state(n, e);
}

static void advance_state(new_account_t *n, derr_t e){
    if(n->closed){
        DROP_VAR(&e);
        return;
    }

    if(is_error(n->e)){
        DROP_VAR(&e);
        goto failing;
    }else{
        PROP_VAR_GO(&n->e, &e, failing);
    }

    // don't do anything without urls
    if(!n->have_urls){
        if(!n->requested_urls){
            n->requested_urls = true;
            PROP_GO(&n->e,
                get_directory(n->directory_url, &http, _get_directory_cb, n),
            failing);
        }
        return;
    }

    // need a nonce before requesting an account
    if(!n->have_nonce){
        if(!n->requested_nonce){
            n->requested_nonce = true;
            PROP_GO(&n->e,
                new_nonce(n->urls.new_nonce, &http, _new_nonce_cb, n),
            failing);
        }
        return;
    }

    if(!n->have_account){
        if(!n->requested_account){
            n->requested_account = true;
            PROP_GO(&n->e,
                post_new_account(
                    n->urls.new_account,
                    &http,
                    n->key,
                    n->nonce,
                    n->contact_email,
                    n->eab_kid,
                    n->eab_hmac_key,
                    _post_new_account_cb,
                    n
                ),
            failing);
        }
        return;
    }

    // dump key info and exit
    DSTR_VAR(pvt, 256);
    PROP_GO(&n->e, n->key->to_jwk_pvt(n->key, &pvt), failing);

    PROP_GO(&n->e,
        FFMT(stdout, NULL,
            "{"
                "\"key\":%x,"
                "\"kid\":\"%x\","
                "\"orders\":\"%x\""
            "}",
            FD(&pvt),
            FD_JSON(&n->kid),
            FD_JSON(&n->orders),
        ),
    failing);

    // done!
    n->success = true;

failing:
    new_account_free(n);
    duv_http_close(&http, http_close_cb);
    n->closed = true;
}

static derr_t new_account(
    const dstr_t contact_email,
    key_i *key,
    const dstr_t eab_kid,
    const dstr_t eab_hmac_key,
    const dstr_t directory_url
){
    derr_t e = E_OK;

    ssl_context_t client_ctx;
    PROP(&e, ssl_context_new_client(&client_ctx) );

    PROP_GO(&e, duv_loop_init(&loop), cu_ctx);

    PROP_GO(&e, duv_scheduler_init(&scheduler, &loop), fail_loop);

    PROP_GO(&e, duv_http_init(&http, &loop, &scheduler, NULL), fail_scheduler);

    // start our state machine
    new_account_t n;
    new_account_prep(&n,
        &scheduler.iface,
        contact_email,
        key,
        eab_kid,
        eab_hmac_key,
        directory_url
    );
    schedule(&n);

    derr_t e2 = duv_run(&loop);
    TRACE_PROP_VAR(&e, &e2);
    TRACE_PROP_VAR(&e, &n.e);
    if(!is_error(e) && !n.success){
        TRACE_ORIG(&e, E_INTERNAL, "finished but did not succeed");
    }

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
        "new-account: create an ACME account with zerossl\n"
        "\n"
        "usage: new-account [-d DIRS_URL] CONTACT_EMAIL [EAB] > account.json\n"
        "\n"
        "  -d --dirs [URL]  Set the acme DIRS url.\n"
        "                   Default: " ZEROSSL_DIRS "\n"
        "\n"
        "Note that EAB is required for zerossl. A suitable EAB file may be\n"
        "generated with:\n"
        "\n"
        "    get-zerossl-eab ACCESS_KEY > eab.json\n"
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
    opt_spec_t o_dirs = {'d', "dirs", true};

    opt_spec_t* spec[] = { &o_help, &o_dirs };
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

    // no positional arguments
    if(newargc < 2 || newargc > 3){
        fprintf(stderr, "try `%s --help` for usage\n", argv[0]);
        return 1;
    }

    char *contact_str = argv[1];
    char *eabpath = newargc > 2 ? argv[2] : NULL;

    dstr_t contact_email;
    DSTR_WRAP(contact_email, contact_str, strlen(contact_str), true);

    dstr_t directory_url = o_dirs.found ? o_dirs.val : DSTR_LIT(ZEROSSL_DIRS);

    PROP_GO(&e, ssl_library_init(), fail);

    key_i *key = NULL;

    // generate a new key
    PROP_GO(&e, gen_es256(&key), fail);

    // load the eab from file
    DSTR_VAR(eab_kid, 256);
    DSTR_VAR(eab_hmac_key, 256);
    if(eabpath){
        DSTR_VAR(eabbuf, 256);
        PROP_GO(&e, dstr_read_path(&SB(FS(eabpath)), &eabbuf), fail);
        DSTR_VAR(jtext, 256);
        json_node_t nodemem[8];
        size_t nnodes = sizeof(nodemem)/sizeof(*nodemem);
        json_t json;
        json_prep_preallocated(&json, &jtext, nodemem, nnodes, true);
        PROP_GO(&e, json_parse(eabbuf, &json), fail);
        dstr_t eab_hmac_key_b64;
        jspec_t *jspec = JOBJ(false,
            JKEY("eab_hmac_key", JDREF(&eab_hmac_key_b64)),
            JKEY("eab_kid", JDCPY(&eab_kid)),
        );
        PROP_GO(&e, jspec_read(jspec, json.root), fail);
        PROP_GO(&e, b64url2bin(eab_hmac_key_b64, &eab_hmac_key), fail);
    }

    PROP_GO(&e,
        new_account(contact_email, key, eab_kid, eab_hmac_key, directory_url),
    fail);

fail:
    int retval = 0;
    if(is_error(e)){
        retval = 1;
        DUMP(e);
        DROP_VAR(&e);
    }

    if(key) key->free(key);

    ssl_library_close();

    return retval;
}
