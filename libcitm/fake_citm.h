typedef struct {
    citm_conn_t iface;
    bool is_closed;
} fake_citm_conn_t;

citm_conn_t *fake_citm_conn(
    fake_citm_conn_t *f,
    stream_i *stream,
    imap_security_e security,
    SSL_CTX *ctx
);

derr_t fake_citm_conn_cleanup(
    manual_scheduler_t *m, fake_citm_conn_t *f, fake_stream_t *s
);
