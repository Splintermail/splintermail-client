// main.c is mostly for testing; user-facing entrypoint is splintermail.c/ui.c
#include "libcitm.h"

// defaults
DSTR_STATIC(d_listen, "insecure://[::1]:1993");
DSTR_STATIC(d_remote, "tls://splintermail.com:993");
DSTR_STATIC(d_maildirs, "/tmp/maildir_root");

static void print_help(FILE *f){
    FFMT_QUIET(f, NULL,
        "usage: citm [OPTIONS]\n"
        "\n"
        "where OPTIONS are any of:\n"
        "  -h, --help\n"
        "  -l, --listen=ARG    (default: %x)\n"
        "  -r, --remote=ARG    (default: %x)\n"
        "  -k, --key=ARG       (default: none)\n"
        "  -c, --cert=ARG      (default: none)\n"
        "  -m, --maildirs=ARG  (default: %x)\n",
        FD(&d_listen),
        FD(&d_remote),
        FD(&d_maildirs)
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

int main(int argc, char **argv){
    // ignore SIGPIPE, required to work with OpenSSL
    // see https://mta.openssl.org/pipermail/openssl-users/2017-May/005776.html
    // (but SIGPIPE doesnt exist in windows)
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    // add logger
    logger_add_fileptr(LOG_LVL_INFO, stdout);
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
    opt_spec_t o_maildirs = {'m', "maildirs",true};

    opt_spec_t* spec[] = {
        &o_help,
        &o_listen,
        &o_remote,
        &o_key,
        &o_cert,
        &o_maildirs,
    };
    size_t speclen = sizeof(spec) / sizeof(*spec);
    int newargc;
    {
        derr_t e = opt_parse(argc, argv, spec, speclen, &newargc);
        CATCH(e, E_ANY){
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

    derr_t e = E_OK;

    PROP_GO(&e, ssl_library_init(), done);

    // resolve options
    if(listeners.len == 0){
        listeners.specs[0] = must_parse_addrspec(&d_listen);
        listeners.len = 1;
    }
    dstr_t remotestr = o_remote.found ? o_remote.val : d_remote;
    addrspec_t remote;
    PROP_GO(&e, parse_addrspec(&remotestr, &remote), cu);
    const char *key = o_key.found ? o_key.val.data : NULL;
    const char *cert = o_cert.found ? o_cert.val.data : NULL;
    const dstr_t *maildirs = o_maildirs.found ? &o_maildirs.val : &d_maildirs;

    string_builder_t maildir_root = SB(FD(maildirs));

    PROP_GO(&e,
         uv_citm(
            listeners.specs,
            listeners.len,
            remote,
            key,
            cert,
            maildir_root,
            // indicate when the listener is ready
            true
        ),
    cu);

cu:
    ssl_library_close();

done:

    CATCH(e, E_ANY){
        DUMP(e);
        DROP_VAR(&e);
        return 1;
    }

    return 0;
}
