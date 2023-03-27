/*
    STARTTLS implementation:

    - duv_imap_t awaits passthru_t
    - imap sees STARTTLS command, then verifies:
        - it has no reads pending with the passthru_t
        - it has no writes pending with the passthru_t
        - passthru_t is not failing
        - its read buffer is spent (no additional plaintext sneaking through)
    - imap unawaits the passthru_t
    - imap intializes the duv_tls_t
    - imap awaits the duv_tls_t

    Note that this implies that the duv_imap_t owns the duv_tls_t, rather than
    being a normal stream which only interacts with the stream_i.
*/

/*
    imap_server_t:
      - sends greeting
      - if starttls is configured, processes starttls
      - supports reading cmds and writing responses thereafter

    imap_client_t:
      - receives greeting
      - if starttls is configured, calls starttls
      - supports reading responses and writing cmds thereafter

    XXX: we should add supported citm versions in the capabilities, since a
         simple version in the greeting isn't that expressive, and also doesn't
         generalize well to this abstraction
*/

#define DECLARE_STRUCT(x) struct x; typedef struct x x
DECLARE_STRUCT(imap_server_t);
DECLARE_STRUCT(imap_client_t);
DECLARE_STRUCT(imap_server_read_t);
DECLARE_STRUCT(imap_server_write_t);
DECLARE_STRUCT(imap_client_read_t);
DECLARE_STRUCT(imap_client_write_t);
#undef DECLARE_STRUCT

typedef void (*imap_server_read_cb)(
    imap_server_t *s, imap_server_read_t *req, imap_cmd_t *cmd, bool ok
);

typedef void (*imap_server_write_cb)(
    imap_server_t *s, imap_server_write_t *req, bool ok
);

// the await cb is the last chance to access memory in *server
typedef void (*imap_server_await_cb)(imap_server_t *s, derr_t e);

typedef void (*imap_client_read_cb)(
    imap_client_t *c, imap_client_read_t *req, imap_resp_t *resp, bool ok
);

typedef void (*imap_client_write_cb)(
    imap_client_t *c, imap_client_write_t *req, bool ok
);

// the await cb is the last chance to access memory in *client
typedef void (*imap_client_await_cb)(imap_client_t *c, derr_t e);

struct imap_server_read_t {
    imap_server_read_cb cb;
    link_t link;
};
DEF_CONTAINER_OF(imap_server_read_t, link, link_t)

struct imap_server_write_t {
    imap_server_write_cb cb;
    imap_resp_t *resp;
    link_t link;
};
DEF_CONTAINER_OF(imap_server_write_t, link, link_t)

struct imap_client_read_t {
    imap_client_read_cb cb;
    link_t link;
};
DEF_CONTAINER_OF(imap_client_read_t, link, link_t)

struct imap_client_write_t {
    imap_client_write_cb cb;
    imap_cmd_t *cmd;
    link_t link;
};
DEF_CONTAINER_OF(imap_client_write_t, link, link_t)

void imap_server_read(
    imap_server_t *s, imap_server_read_t *req, imap_server_read_cb cb
);

void imap_server_write(
    imap_server_t *s,
    imap_server_write_t *req,
    imap_resp_t *resp,
    imap_server_read_cb cb
);

void imap_client_read(
    imap_client_t *c, imap_client_read_t *req, imap_client_read_cb cb
);

void imap_client_write(
    imap_client_t *c,
    imap_client_write_t *req,
    imap_cmd_t *cmd,
    imap_client_read_cb cb
);

derr_t imap_server_new(
    imap_server_t **out, scheduler_i *scheduler, citm_conn_t *conn
);
derr_t imap_client_new(
    imap_client_t **out, scheduler_i *scheduler, stream_i *base
);

// must not have been awaited yet (that is, await_cb must not have been called)
imap_server_await_cb imap_server_await(
    imap_server_t *s, imap_server_await_cb cb
);
imap_client_await_cb imap_client_await(
    imap_client_t *c, imap_client_await_cb cb
);

// idempotent
void imap_server_cancel(imap_server_t *s);
void imap_client_cancel(imap_client_t *c);

// if not awaited, it will stay alive long enough to await itself
void imap_server_free(imap_server_t **server);
void imap_client_free(imap_server_t *client);

struct imap_server_t {
    imap_security_e security;
    imap_cmd_reader_t reader;
    scheduler_i *scheduler;
    schedulable_t schedulable;
    link_t link;

    extensions_t exts;

    citm_conn_t *conn;
    stream_i *stream;
    stream_await_cb original_base_await_cb;
    duv_tls_t tls;

    char rbufmem[4096];
    dstr_t rbuf;
    char wbufmem[4096];
    dstr_t wbuf;
    stream_read_t read_req;
    stream_write_t write_req;

    link_t cmds;
    link_t resps;
    link_t reads;
    link_t writes;

    imap_server_await_cb await_cb;

    derr_t e;
    size_t write_skip;
    size_t nwritten;

    bool starttls : 1;
    bool starttls_started : 1;
    bool starttls_done : 1;

    bool greeting_started : 1;
    bool greeting_done : 1;

    bool read_started : 1;
    bool read_done : 1;

    bool write_started : 1;
    bool write_done : 1;

    bool logged_out : 1;
    bool shutdown : 1;

    bool canceled : 1;
    bool failed : 1;
    bool awaited : 1;
};
DEF_CONTAINER_OF(imap_server_t, link, link_t)
DEF_CONTAINER_OF(imap_server_t, schedulable, schedulable_t)


/* duv_imap_t consumes a stream_i but does not provide one, though it has some
   stream_i-like mechanics, like awaiting and canceling */
typedef struct {
    // the stream interface we parse from and marshal to
    stream_i *base;
    stream_await_cb original_base_await_cb;

    /* the tls stream which we activate:
         - never, for IMAP_INSECURE
         - after the STARTTLS command, for IMAP_STARTTLS
         - in init(), for IMAP_TLS */
    duv_tls_t tls;
    stream_i *tls_stream;

    // either stream==base or stream==tls_stream
    stream_i *stream;

    imap_security_e security;
    scheduler_i *scheduler;
    schedulable_t schedulable;

    // buffers for reading and writing from/to the base stream
    dstr_t read_buf;
    stream_read_t read_req;
    dstr_t write_buf;
    stream_write_t write_req;

    link_t reads;  // stream_read_t->link
    link_t writes;  // stream_write_t->link

    // state machine
    derr_t e;

    size_t nbufswritten;
    size_t nwritten;

    bool base_failing : 1;

    bool read_pending : 1;

    bool write_pending : 1;

    // if starttls has actually happened or not
    bool starttls : 1;

    bool shutdown : 1;
    bool base_canceled : 1;
    bool base_awaited : 1;
} imap_common_t;


struct imap_client_t {
    imap_common_t common;
    imap_resp_reader_t reader;
    link_t link;
};
