// main.c is mostly for testing; user-facing entrypoint is splintermail.c/ui.c
#include "libcitm.h"

#include "test/certs.h"

// defaults
DSTR_STATIC(d_listen, "insecure://[::1]:1993");
DSTR_STATIC(d_remote, "tls://splintermail.com:993");
DSTR_STATIC(d_sm_dir, "/tmp/sm_dir");
DSTR_STATIC(d_rest, "https://splintermail.com");

static void print_help(FILE *f){
    FFMT_QUIET(f,
        "usage: citm [OPTIONS]\n"
        "\n"
        "where OPTIONS are any of:\n"
        "  -h, --help\n"
        "  -l, --listen ARG    (default: %x)\n"
        "  -r, --remote ARG    (default: %x)\n"
        "  -k, --key ARG       (default: none)\n"
        "  -c, --cert ARG      (default: none)\n"
        "  -d, --sm-dir ARG    (default: %x)\n"
        "  -a, --acme ARG      (default: " LETSENCRYPT ")\n"
        "      --rest ARG      (default: %x)\n"
        "      --ca ARG        (deault: none)\n"
        "  -p, --pebble        trust pebble's certificate, and change\n"
        "                      default --acme to localhost:14000\n",
        FD(d_listen),
        FD(d_remote),
        FD(d_sm_dir),
        FD(d_rest)
    );
}

typedef struct {
    dstr_t *dstrs;
    addrspec_t *specs;
    size_t len;
    size_t cap;
} listener_list_t;

static derr_t listener_cb(void *data, dstr_t val){
    derr_t e = E_OK;

    listener_list_t *l = data;

    size_t idx = l->len++;

    if(l->len > l->cap){
        ORIG(&e, E_FIXEDSIZE, "too many --listener flags, limit 8");
    }

    // val.data is persisted, but the dstr_t box is not
    l->dstrs[idx] = val;
    PROP(&e, parse_addrspec(&l->dstrs[idx], &l->specs[idx]) );

    return e;
}

static void indicate_ready(void *data, uv_citm_t *uv_citm){
    (void)data;
    (void)uv_citm;
    LOG_INFO("all listeners ready\n");
}

int main(int argc, char **argv){
    // ignore SIGPIPE, required to work with OpenSSL
    // see https://mta.openssl.org/pipermail/openssl-users/2017-May/005776.html
    // (but SIGPIPE doesnt exist in windows)
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    // add logger
    logger_add_fileptr(LOG_LVL_DEBUG, stdout);
    auto_log_flush(true);

    // support multiple listeners
    dstr_t dstrs[8] = {0};
    addrspec_t specs[8] = {0};
    listener_list_t listeners = { .dstrs = dstrs, .specs = specs, .cap = 8 };

    // options
    opt_spec_t o_help     = {'h', "help",    false};
    opt_spec_t o_listen   = {'l', "listen",  true, listener_cb, &listeners};
    opt_spec_t o_remote   = {'r', "remote",  true};
    opt_spec_t o_key      = {'k', "key",     true};
    opt_spec_t o_cert     = {'c', "cert",    true};
    opt_spec_t o_sm_dir   = {'d', "splintermail-dir", true};
    opt_spec_t o_acme     = {'a', "acme",    true};
    opt_spec_t o_rest     = {'\0',"rest",    true};
    opt_spec_t o_ca       = {'\0',"ca",      true};
    opt_spec_t o_pebble   = {'p', "pebble",  false};

    opt_spec_t* spec[] = {
        &o_help,
        &o_listen,
        &o_remote,
        &o_key,
        &o_cert,
        &o_sm_dir,
        &o_acme,
        &o_rest,
        &o_ca,
        &o_pebble,
    };
    size_t speclen = sizeof(spec) / sizeof(*spec);
    int newargc;
    {
        derr_t e = opt_parse(argc, argv, spec, speclen, &newargc);
        CATCH_ANY(&e){
            DUMP(e);
            DROP_VAR(&e);
            print_help(stderr);
            return 1;
        }
    }

    if(o_help.found){
        print_help(stdout);
        return 0;
    }

    dstr_t acme_dirurl = DSTR_LIT(LETSENCRYPT);
    if(o_pebble.found) acme_dirurl = DSTR_LIT("https://localhost:14000/dir");
    if(o_acme.found) acme_dirurl = o_acme.val;

    const char *ca = o_ca.found ? o_ca.val.data : NULL;

    derr_t e = E_OK;

    ssl_context_t ssl_ctx = {0};

    PROP_GO(&e, ssl_library_init(), done);

    PROP_GO(&e, ssl_context_new_client_ex(&ssl_ctx, true, &ca, !!ca), cu);

    char *acme_verify_name = NULL;
    if(o_pebble.found){
        PROP_GO(&e, trust_pebble(ssl_ctx.ctx), cu);
        acme_verify_name = "pebble";
    }

    if(listeners.len == 0){
        listeners.specs[0] = must_parse_addrspec(&d_listen);
        listeners.len = 1;
    }
    dstr_t remotestr = o_remote.found ? o_remote.val : d_remote;
    addrspec_t remote;
    PROP_GO(&e, parse_addrspec(&remotestr, &remote), cu);
    const char *key = o_key.found ? o_key.val.data : NULL;
    const char *cert = o_cert.found ? o_cert.val.data : NULL;
    string_builder_t sm_dir = SBD(o_sm_dir.found ? o_sm_dir.val : d_sm_dir);
    dstr_t sm_baseurl = o_rest.found ? o_rest.val : d_rest;

    PROP_GO(&e,
         uv_citm(
            listeners.specs,
            listeners.len,
            remote,
            key,
            cert,
            acme_dirurl,
            acme_verify_name,
            sm_baseurl,
            ssl_ctx.ctx,
            sm_dir,
            indicate_ready,
            NULL, // user_async_hook
            NULL
        ),
    cu);

cu:
    ssl_context_free(&ssl_ctx);
    ssl_library_close();

done:

    CATCH_ANY(&e){
        DUMP(e);
        DROP_VAR(&e);
        return 1;
    }

    return 0;
}
