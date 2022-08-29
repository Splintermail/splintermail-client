// a tls-encryption layer on a base stream

/* 60xx is reserved for stream API errors.
   61xx is reserved for duv_tls_t errors. */

// duv_tls_t-specific errors (base stream errors may also be passed thru)
// notice that we can return ENOMEM/ECANCELED ourselves, so we include aliases
#define DUV_TLS_ERRNO_MAP(XX) \
    XX(ENOMEM, UV_ENOMEM, "cannot allocate memory") \
    XX(ECANCELED, UV_ECANCELED, "operation canceled") \
    XX(ETLS, -6100, "TLS protocol error (or other unrecognized error)") \
    XX(EUNEXPECTED, -6101, "an unexpected error from the tls library") \
    XX(ENOCERT, -6102, "peer did not provide a certificate") \
    XX(ECAUNK, -6103, "peer certificate authority is unknown") \
    XX(ESELFSIGN, -6104, "peer certificate is self-signed but not trusted") \
    XX(ECERTBAD, -6105, "peer certificate is broken or invalid") \
    XX(ESIGBAD, -6106, "peer signature failure") \
    XX(ECERTUNSUP, -6107, "peer certificate or CA does not support this purpose") \
    XX(ECERTNOTYET, -6108, "peer certificate is not yet valid") \
    XX(ECERTEXP, -6109, "peer certificate is expired") \
    XX(ECERTREV, -6110, "peer certificate is revoked") \
    XX(EEXTUNSUP, -6111, "peer certificate uses unrecognized extensions") \
    XX(EHOSTNAME, -6112, "peer certificate does not match hostname") \
    XX(EHANDSHAKE, -6113, "tls handshake failed for unknown reasons") \

#define DUV_TLS_ERROR_ENUM_DEF(e, val, msg) DUV_TLS_##e = val,
enum _duv_tls_error_e {
    DUV_TLS_ERRNO_MAP(DUV_TLS_ERROR_ENUM_DEF)
};

typedef struct {
    // the stream we provide
    stream_i iface;
    void *data;

    // the stream we are encrypting
    stream_i *base;

    SSL *ssl;
    BIO *rawout;
    BIO *rawin;
    bool client;
    bool want_verify;
    uv_async_t async;

    // buffers for reading and writing from/to the base stream
    uv_buf_t read_buf;
    bool using_read_buf;
    uv_buf_t write_buf;
    bool using_write_buf;
    stream_write_t write_req;

    // control signals (from user api calls)
    struct {
        bool read;
        stream_alloc_cb alloc_cb;
        stream_read_cb read_cb;
        bool shutdown;
        stream_shutdown_cb shutdown_cb;
        bool close;
        stream_close_cb close_cb;
        link_t writes;  // duv_tls_write_t->link
    } signal;

    // state machine
    int failing;  // the reason we're closing (UV_ECANCELED = user closed us)

    uv_buf_t allocated;  // a buffer allocated from the user

    size_t nbufswritten;
    size_t nwritten;

    bool need_read : 1;  // WANT_READ from SSL_{do_handshake,write,shutdown}
    bool read_wants_read : 1;  // WANT_READ from SSL_read
    bool reading_base : 1;

    bool handshake_done : 1;
    bool shutdown : 1;
    bool base_closing : 1;
    bool base_closed : 1;
    bool async_closing : 1;
    bool async_closed : 1;
    bool closed : 1;
    bool tls_eof : 1;
} duv_tls_t;
DEF_CONTAINER_OF(duv_tls_t, iface, stream_i);

// wrap an existing stream_i* in tls, returning an encrypted stream_i*
/* you give up control over the *base entirely, including control over the
   set_data() and get_data() methods.  duv_tls_t guarantees that when its own
   close_cb is called that *base will also be fully closed */
derr_t duv_tls_wrap_client(
    duv_tls_t *t,
    SSL_CTX *ssl_ctx,
    const dstr_t verify_name,
    uv_loop_t *loop,
    stream_i *base,
    stream_i **out
);

derr_t duv_tls_wrap_server(
    duv_tls_t *t,
    SSL_CTX *ssl_ctx,
    uv_loop_t *loop,
    stream_i *base,
    stream_i **out
);
