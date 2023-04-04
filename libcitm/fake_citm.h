typedef struct {
    citm_conn_t iface;
    bool is_closed;
} fake_citm_conn_t;

citm_conn_t *fake_citm_conn(
    fake_citm_conn_t *f,
    stream_i *stream,
    imap_security_e security,
    SSL_CTX *ctx,
    dstr_t verify_name
);

derr_t fake_citm_conn_cleanup(
    manual_scheduler_t *m, fake_citm_conn_t *f, fake_stream_t *s
);

// automatically closes canceled streams, which usually exposes helpful errors
void _advance_fakes(manual_scheduler_t *m, fake_stream_t **f, size_t nf);
#define ADVANCE_FAKES(...) \
    _advance_fakes( \
        &scheduler, \
        (fake_stream_t*[]){__VA_ARGS__}, \
        sizeof((fake_stream_t*[]){__VA_ARGS__}) / sizeof(fake_stream_t*) \
    )

typedef struct {
    citm_connect_i iface;
    citm_conn_cb cb;
    void *data;
    bool canceled;
    bool done;
    link_t link; // fake_citm_io_t->fcncts
} fake_citm_connect_t;

void fake_citm_connect_prep(fake_citm_connect_t *fcnct);

derr_t fake_citm_connect_finish(
    fake_citm_connect_t *fcnct, citm_conn_t *conn, derr_type_t etype
);

typedef struct {
    citm_io_i iface;
    link_t fcncts;  // fake_citm_connect_t->link
} fake_citm_io_t;

citm_io_i *fake_citm_io(fake_citm_io_t *fio);

// libcitm test test utilities

derr_t ctx_setup(const char *test_files, SSL_CTX **s_out, SSL_CTX **c_out);
