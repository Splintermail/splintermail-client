#include "test/bioconn.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>

#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/x509v3.h>

derr_t connection_new(connection_t* conn,
                      const char* addr, unsigned int port){
    derr_t e = E_OK;
    derr_t e2;
    // combine address and port
    DSTR_VAR(addr_port, 256);
    e2 = FMT(&addr_port, "%x:%x", FS(addr), FU(port));
    CATCH(&e2, E_FIXEDSIZE){
        TRACE(&e2, "address too long\n");
        RETHROW(&e, &e2, E_PARAM);
    }else PROP(&e, e2);

    // make connection
    conn->bio = BIO_new_connect(addr_port.data);
    if(!conn->bio){
        ORIG(&e, E_NOMEM, "error creating bio: %x", FSSL);
    }

    // connect BIO
    // BIO_do_connect is a macro which includes a -Wconversion-unsafe cast
    // so we will use the low-level function, taken from openssl/bio.h
    //int ret = BIO_do_connect(conn->bio);
    long ret = BIO_ctrl(conn->bio, BIO_C_DO_STATE_MACHINE, 0, NULL);
    if(ret != 1){
        ORIG_GO(&e, E_CONN, "error making connection: %x", fail, FSSL);
    }

    return e;

fail:
    BIO_free_all(conn->bio);
    conn->bio = NULL;
    return e;
}

derr_t connection_new_ssl(connection_t* conn, ssl_context_t* ctx,
                          const char* addr, unsigned int port){
    derr_t e = E_OK;
    derr_t e2;
    // combine address and port
    DSTR_VAR(addr_port, 256);
    e2 = FMT(&addr_port, "%x:%x", FS(addr), FU(port));
    CATCH(&e2, E_FIXEDSIZE){
        TRACE(&e2, "address too long\n");
        RETHROW(&e, &e2, E_PARAM);
    }else PROP(&e, e2);

    // make encrypted connection
    conn->bio = BIO_new_ssl_connect(ctx->ctx);
    if(!conn->bio){
        ORIG(&e, E_NOMEM, "error creating bio: %x", FSSL);
    }

    // get ssl pointer from bio
    SSL* ssl = NULL;
    BIO_get_ssl(conn->bio, &ssl);
    if(!ssl){
        ORIG_GO(&e,
            E_INTERNAL, "error getting ssl from bio: %x", fail_1, FSSL
        );
    }

    // set hostname and port, always returns 1
    BIO_set_conn_hostname(conn->bio, addr_port.data);

    // connect BIO
    // BIO_do_connect is a macro which includes a -Wconversion-unsafe cast
    // so we will use the low-level function, taken from openssl/bio.h
    //int ret = BIO_do_connect(conn->bio);
    long ret = BIO_ctrl(conn->bio, BIO_C_DO_STATE_MACHINE, 0, NULL);
    if(ret != 1){
        /* in macos, something about the way we set the callback causes the
           verification error here instead */
        long lret = SSL_get_verify_result(ssl);
        derr_type_t etype = lret != X509_V_OK ? E_SSL : E_CONN;
        ORIG_GO(&e, etype, "error making connection: %x", fail_1, FSSL);
    }

    // is SSL certificate verified (cryptographically sound)?
    long lret = SSL_get_verify_result(ssl);
    if(lret != X509_V_OK){
        ORIG_GO(&e, E_SSL, "unverified certificate: %x", fail_1, FSSL);
    }

    // check name on certificate
    X509* peer = SSL_get_peer_certificate(ssl);
    if(!peer){
        /* according to man page, a client-side call to SSL_get_peer_certificate
           can only return NULL if we are using anonymous ciphers or we are not
           connected */
        ORIG_GO(&e,
            E_INTERNAL, "error getting peer certificate: %x", fail_1, FSSL
        );
    }
    // X509_NAME* name_on_cert = X509_get_subject_name(peer);
    // char namebuf[1024];
    // X509_NAME_oneline(name_on_cert, namebuf, 1024 - 1);
    // LOG_DEBUG("ssl connected to %x\n", FS(namebuf));
    ret = X509_check_host(peer, addr, strlen(addr), 0, NULL);
    if(ret == 0){
        ORIG_GO(&e,
            E_SSL, "server name does not match certificate: %x", fail_2, FSSL
        );
    }else if(ret != 1){
        ORIG_GO(&e,
            E_INTERNAL, "library error checking server name: %x", fail_2, FSSL
        );
    }

    X509_free(peer);
    return e;

fail_2:
    X509_free(peer);
fail_1:
    BIO_free_all(conn->bio);
    conn->bio = NULL;
    return e;
}

derr_t connection_read(connection_t* conn, dstr_t* out, size_t *amount_read){
    derr_t e = E_OK;
    int ret;
    if(out->fixed_size){
        // try to fill buffer
        int bytes_to_read = (int)MIN(out->size - out->len, INT_MAX);
        if(bytes_to_read == 0) ORIG(&e, E_FIXEDSIZE, "can't read to full buffer");
        ret = BIO_read(conn->bio, out->data + out->len, bytes_to_read);
        if(ret > 0) out->len += (size_t)ret;
    }else{
        // just read an arbitrary chunk
        DSTR_VAR(temp, 4096);
        int bytes_to_read = (int)MIN(temp.size, INT_MAX);
        ret = BIO_read(conn->bio, temp.data, bytes_to_read);
        if(ret > 0){
            temp.len = (size_t)ret;
            PROP(&e, dstr_append(out, &temp) );
        }
    }
    // amount_read == NULL means "please throw error on broken connection"
    if(!amount_read && ret <= 0){
        ORIG(&e, E_CONN, "broken connection: %x", FSSL);
    }
    if(amount_read) *amount_read = ret < 0 ? 0 : (size_t)ret;
    return e;
}

derr_t connection_write(connection_t* conn, const dstr_t* in){
    derr_t e = E_OK;
    // repeatedly try to write until we write the whole buffer
    size_t total = 0;
    while(total < in->len){
        int bytes_to_write = (int)MIN(in->len - total, INT_MAX);
        int written = BIO_write(conn->bio, in->data + total, bytes_to_write);
        // writing zero or fewer bytes is a failure mode
        if(written <= 0){
            ORIG(&e, E_CONN, "broken connection: %x", FSSL);
        }
        total += (size_t)written;
    }

    return e;
}

void connection_close(connection_t* conn){
    if(conn->bio){
        BIO_free_all(conn->bio);
        conn->bio = NULL;
    }
}




derr_t listener_new(listener_t* l, const char* addr, unsigned int port){
    derr_t e = E_OK;
    derr_t e2;
    // combine address and port
    DSTR_VAR(addr_port, 256);
    e2 = FMT(&addr_port, "%x:%x", FS(addr), FU(port));
    CATCH(&e2, E_FIXEDSIZE){
        TRACE(&e2, "address too long\n");
        RETHROW(&e, &e2, E_PARAM);
    }else PROP(&e, e2);

    // open a listener
    l->bio = BIO_new_accept(addr_port.data);
    if(!l->bio){
        ORIG(&e, E_SOCK, "unable to create listener bio: %x", FSSL);
    }

    // set SO_REUSEADDR
    // BIO_set_bind_mode is a macro which includes a -Wconversion-unsafe cast
    // so we will use the low-level function, taken from openssl/bio.h
    // int ret = BIO_set_bind_mode(l->bio, BIO_BIND_REUSEADDR);
    long ret = BIO_ctrl(l->bio, BIO_C_SET_BIND_MODE, BIO_BIND_REUSEADDR, NULL);
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "failed to set REUSEADDR: %x", fail, FSSL);
    }

    // the first time this is called it binds to the port
    // ret = BIO_do_accept(l->bio);
    ret = BIO_ctrl(l->bio, BIO_C_DO_STATE_MACHINE, 0, NULL);
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "failed to bind to port: %x", fail, FSSL);
    }

    return e;
fail:
    BIO_free_all(l->bio);
    l->bio = NULL;
    return e;
}

derr_t listener_new_ssl(listener_t* l, ssl_context_t* ctx,
                          const char* addr, unsigned int port){
    derr_t e = E_OK;
    // combine address and port
    DSTR_VAR(addr_port, 256);
    derr_t e2 = FMT(&addr_port, "%x:%x", FS(addr), FU(port));
    CATCH(&e2, E_FIXEDSIZE){
        TRACE(&e2, "address too long\n");
        RETHROW(&e, &e2, E_PARAM);
    }else PROP(&e, e2);

    // open a listener
    l->bio = BIO_new_accept(addr_port.data);
    if(!l->bio){
        ORIG(&e, E_SOCK, "unable to create listener bio: %x", FSSL);
    }

    // set SO_REUSEADDR
    // BIO_set_bind_mode is a macro which includes a -Wconversion-unsafe cast
    // so we will use the low-level function, taken from openssl/bio.h
    // int ret = BIO_set_bind_mode(l->bio, BIO_BIND_REUSEADDR);
    long ret = BIO_ctrl(l->bio, BIO_C_SET_BIND_MODE, BIO_BIND_REUSEADDR, NULL);
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "failed to set REUSEADDR: %x", fail, FSSL);
    }

    // build the SSL bio chain
    BIO* ssl_bio = BIO_new_ssl(ctx->ctx, 0);
    if(!ssl_bio){
        ORIG_GO(&e, E_SSL, "unable to create ssl_bio: %x", fail, FSSL);
    }

    // attach the SSL bio chain
    long lret = BIO_set_accept_bios(l->bio, ssl_bio);
    if(lret != 1){
        BIO_free_all(ssl_bio);
        ssl_bio = NULL;
        ORIG_GO(&e, E_SSL, "unable to attach ssl_bio: %x", fail, FSSL);
    }

    // the first time this is called it binds to the port
    // ret = BIO_do_accept(l->bio);
    ret = BIO_ctrl(l->bio, BIO_C_DO_STATE_MACHINE, 0, NULL);
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "failed to bind to port: %x", fail, FSSL);
    }

    return e;
fail:
    BIO_free_all(l->bio);
    l->bio = NULL;
    return e;
}

derr_t listener_accept(listener_t* l, connection_t* conn){
    derr_t e = E_OK;
    // after the first time, BIO_do_accept() waits for an incoming connection
    // int ret = BIO_do_accept(l->bio);
    long ret = BIO_ctrl(l->bio, BIO_C_DO_STATE_MACHINE, 0, NULL);
    if(ret != 1){
        ORIG(&e, E_SSL, "failed to accept connection: %x", FSSL);
    }

    conn->bio = BIO_pop(l->bio);
    if(!conn->bio){
        ORIG_GO(&e, E_SSL, "failed to pop accept connection: %x", fail, FSSL);
    }

    return e;

fail:
    /* TODO: there should probably be a cleanup step here after
       BIO_do_accept() but I am not sure what it would be */
    return e;
}

void listener_close(listener_t* l){
    if(l->bio){
        BIO_free_all(l->bio);
        l->bio = NULL;
    }
}
