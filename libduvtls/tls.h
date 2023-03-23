// a tls-encryption layer on a base stream

// duv_tls_t-specific errors (base stream errors may also be passed thru)
// notice that we can return ENOMEM/ECANCELED ourselves, so we include aliases
#define DUV_TLS_ERRNO_MAP(XX) \
    XX(E_NOCERT, "peer did not provide a certificate") \
    XX(E_CAUNK, "peer certificate authority is unknown") \
    XX(E_SELFSIGN, "peer certificate is self-signed but not trusted") \
    XX(E_CERTBAD, "peer certificate is broken or invalid") \
    XX(E_SIGBAD, "peer signature failure") \
    XX(E_CERTUNSUP, "peer certificate or CA does not support this purpose") \
    XX(E_CERTNOTYET, "peer certificate is not yet valid") \
    XX(E_CERTEXP, "peer certificate is expired") \
    XX(E_CERTREV, "peer certificate is revoked") \
    XX(E_EXTUNSUP, "peer certificate uses unrecognized extensions") \
    XX(E_HOSTNAME, "peer certificate does not match hostname") \
    XX(E_HANDSHAKE, "tls handshake failed for unknown reasons") \

#define DUV_TLS_ERR_DECL(e, msg) extern derr_type_t e;
DUV_TLS_ERRNO_MAP(DUV_TLS_ERR_DECL)
#undef DUV_TLS_ERR_DECL

typedef struct {
    // the stream we provide
    stream_i iface;

    // the stream we are encrypting
    stream_i *base;
    stream_await_cb original_base_await_cb;

    SSL *ssl;
    BIO *rawout;
    BIO *rawin;
    bool client;
    bool want_verify;
    scheduler_i *scheduler;
    schedulable_t schedulable;

    // buffers for reading and writing from/to the base stream
    dstr_t read_buf;
    stream_read_t read_req;
    dstr_t write_buf;
    stream_write_t write_req;

    link_t reads;  // stream_read_t->link
    link_t writes;  // stream_write_t->link
    stream_shutdown_cb shutdown_cb;
    stream_await_cb await_cb;

    // state machine
    derr_t e;

    size_t nbufswritten;
    size_t nwritten;

    bool base_failing : 1;

    bool need_read : 1;  /* WANT_READ from SSL_{do_handshake,write,shutdown}
                            need read prevents us from doing anything without
                            more data to read */
    bool read_wants_read : 1;  /* WANT_READ returned from SSL_read.  Reads are
                                  blocked but other operations can continue */
    bool read_pending : 1;

    bool write_pending : 1;

    bool handshake_done : 1;
    bool shutdown : 1;
    bool base_canceled : 1;
    bool base_awaited : 1;
} duv_tls_t;
DEF_CONTAINER_OF(duv_tls_t, iface, stream_i)
DEF_CONTAINER_OF(duv_tls_t, schedulable, schedulable_t)

// wrap an existing stream_i* in tls, returning an encrypted stream_i*
// duv_tls_t will await the base stream and reserves use of base->wrapper_data
derr_t duv_tls_wrap_client(
    duv_tls_t *t,
    SSL_CTX *ssl_ctx,
    const dstr_t verify_name,
    scheduler_i *scheduler,
    stream_i *base,
    stream_i **out
);

derr_t duv_tls_wrap_server(
    duv_tls_t *t,
    SSL_CTX *ssl_ctx,
    scheduler_i *scheduler,
    stream_i *base,
    const dstr_t preinput,  // for starttls
    stream_i **out
);
