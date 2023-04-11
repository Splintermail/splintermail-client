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

// tests other than imap_client,imap_server only use plaintext
citm_conn_t *fake_citm_conn_insec(fake_citm_conn_t *f, stream_i *stream);

derr_t fake_citm_conn_cleanup(
    manual_scheduler_t *m, fake_citm_conn_t *f, fake_stream_t *s
);

// automatically closes canceled streams, which usually exposes helpful errors
void _advance_fakes(manual_scheduler_t *m, fake_stream_t **f, size_t nf);
#define ADVANCE_FAKES(m, ...) \
    _advance_fakes( \
        (m), \
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

extern dstr_t mykey_pem;
extern dstr_t mykey_priv;
extern dstr_t mykey_fpr;
extern dstr_t peer1_pem;
extern dstr_t peer1_fpr;
extern dstr_t peer2_pem;
extern dstr_t peer2_fpr;
extern dstr_t peer3_pem;
extern dstr_t peer3_fpr;

typedef struct {
    keydir_i iface;
    keypair_t *mykey;
    link_t peers;  // keypair_t->link
} fake_keydir_t;

derr_t fake_keydir(fake_keydir_t *fkd, const dstr_t mykey_pem, keydir_i **out);
derr_t fake_keydir_add_peer(fake_keydir_t *fkd, const dstr_t pem);

// libcitm test utilities

derr_t ctx_setup(const char *test_files, SSL_CTX **s_out, SSL_CTX **c_out);

derr_t establish_imap_client(manual_scheduler_t *m, fake_stream_t *fs);

derr_t establish_imap_server(manual_scheduler_t *m, fake_stream_t *fs);
