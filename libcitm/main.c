// main.c is mostly for testing; user-facing entrypoint is splintermail.c/ui.c
#include "libcitm.h"

// defaults
static const char *d_l_host   = "127.0.0.1";
static const char *d_l_port   = "1993";
static const char *d_r_host   = "127.0.0.1";
static const char *d_r_port   = "993";
static const char *d_tls_key  = "../c/test/files/ssl/good-key.pem";
static const char *d_tls_cert = "../c/test/files/ssl/good-cert.pem";
DSTR_STATIC(d_maildirs, "/tmp/maildir_root");

static void print_help(FILE *f){
    DROP_CMD(
        FFMT(f, NULL,
            "usage: citm [OPTIONS]\n"
            "\n"
            "where OPTIONS are any of:\n"
            "  -h, --help\n"
            "      --local-host=ARG    (default: %x)\n"
            "      --local-port=ARG    (default: %x)\n"
            "      --remote-host=ARG   (default: %x)\n"
            "      --remote-port=ARG   (default: %x)\n"
            "      --tls-key=ARG       (default: %x)\n"
            "      --tls-cert=ARG      (default: %x)\n"
            "      --maildirs=ARG      (default: %x)\n",
            FS(d_l_host),
            FS(d_l_port),
            FS(d_r_host),
            FS(d_r_port),
            FS(d_tls_key),
            FS(d_tls_cert),
            FD(&d_maildirs)
        )
    );
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


    // options
    opt_spec_t o_help     = {'h',  "help",        false};
    opt_spec_t o_l_host   = {'\0', "local-host",  true};
    opt_spec_t o_l_port   = {'\0', "local-port",  true};
    opt_spec_t o_r_host   = {'\0', "remote-host", true};
    opt_spec_t o_r_port   = {'\0', "remote-port", true};
    opt_spec_t o_tls_key  = {'\0', "tls-key",     true};
    opt_spec_t o_tls_cert = {'\0', "tls-cert",    true};
    opt_spec_t o_maildirs = {'\0', "maildirs",    true};

    opt_spec_t* spec[] = {
        &o_help,
        &o_l_host,
        &o_l_port,
        &o_r_host,
        &o_r_port,
        &o_tls_key,
        &o_tls_cert,
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
    const char *l_host = o_l_host.found ? o_l_host.val.data : d_l_host;
    const char *l_port = o_l_port.found ? o_l_port.val.data : d_l_port;
    const char *r_host = o_r_host.found ? o_r_host.val.data : d_r_host;
    const char *r_port = o_r_port.found ? o_r_port.val.data : d_r_port;
    const char *tls_key = o_tls_key.found ? o_tls_key.val.data : d_tls_key;
    const char *tls_cert = o_tls_cert.found ? o_tls_cert.val.data : d_tls_cert;
    const dstr_t *maildirs = o_maildirs.found ? &o_maildirs.val : &d_maildirs;

    string_builder_t maildir_root = SB(FD(maildirs));

    PROP_GO(&e,
         citm(
            l_host,
            l_port,
            tls_key,
            tls_cert,
            r_host,
            r_port,
            &maildir_root,
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
