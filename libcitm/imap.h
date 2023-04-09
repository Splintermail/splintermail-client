/*
    imap_server_t:
      - sends greeting
      - if starttls is configured, processes starttls
      - relays cmds and resps thereafter

    imap_client_t:
      - receives greeting
      - if starttls is configured, calls starttls
      - XXX: check capabilities after starttls established?
      - relays cmds and resps thereafter

     XXX: we should add supported citm versions in the capabilities, since a
          simple version in the greeting isn't that expressive, and also doesn't
          generalize well to this abstraction

     XXX: make the imap client and imap server reschedule after writes if they
          have space in their write buffer, before submitting to lower layers
*/

#define DECLARE_STRUCT(x) struct x; typedef struct x x
DECLARE_STRUCT(imap_server_t);
DECLARE_STRUCT(imap_client_t);
DECLARE_STRUCT(imap_server_read_t);
DECLARE_STRUCT(imap_server_write_t);
DECLARE_STRUCT(imap_client_read_t);
DECLARE_STRUCT(imap_client_write_t);
#undef DECLARE_STRUCT

// eof is never returned, either the client has logged out or it is a failure
typedef void (*imap_server_read_cb)(
    imap_server_t *s, imap_server_read_t *req, imap_cmd_t *cmd
);

typedef void (*imap_server_write_cb)(
    imap_server_t *s, imap_server_write_t *req
);

// the await cb is the last chance to access memory in *server
typedef void (*imap_server_await_cb)(
    imap_server_t *s, derr_t e, link_t *reads, link_t *writes
);

typedef void (*imap_client_read_cb)(
    imap_client_t *c, imap_client_read_t *req, imap_resp_t *resp
);

typedef void (*imap_client_write_cb)(
    imap_client_t *c, imap_client_write_t *req
);

// the await cb is the last chance to access memory in *client
typedef void (*imap_client_await_cb)(
    imap_client_t *c, derr_t e, link_t *reads, link_t *writes
);

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

bool imap_server_read(
    imap_server_t *s, imap_server_read_t *req, imap_server_read_cb cb
);

bool imap_server_write(
    imap_server_t *s,
    imap_server_write_t *req,
    imap_resp_t *resp,
    imap_server_write_cb cb
);

bool imap_client_read(
    imap_client_t *c, imap_client_read_t *req, imap_client_read_cb cb
);

bool imap_client_write(
    imap_client_t *c,
    imap_client_write_t *req,
    imap_cmd_t *cmd,
    imap_client_write_cb cb
);

#define MUST(func, ...) do { \
    if(!func(__VA_ARGS__)) LOG_FATAL(#func " failed\n"); \
} while(0)

#define imap_server_must_read(s, req, cb) \
    MUST(imap_server_read, (s), (req), (cb))
#define imap_client_must_write(c, req, cmd, cb) \
    MUST(imap_client_write, (c), (req), (cmd), (cb))
#define imap_client_must_read(s, req, cb) \
    MUST(imap_client_read, (s), (req), (cb))
#define imap_server_must_write(c, req, resp, cb) \
    MUST(imap_server_write, (c), (req), (resp), (cb))

derr_t imap_server_new(
    imap_server_t **out, scheduler_i *scheduler, citm_conn_t *conn
);
derr_t imap_client_new(
    imap_client_t **out, scheduler_i *scheduler, citm_conn_t *conn
);

// retruns ok=false if await_cb has been called
// out may be NULL
bool imap_server_await(
    imap_server_t *s, imap_server_await_cb cb, imap_server_await_cb *out
);
bool imap_client_await(
    imap_client_t *c, imap_client_await_cb cb, imap_client_await_cb *out
);

#define imap_server_must_await(s, cb, out) \
    MUST(imap_server_await, (s), (cb), (out))
#define imap_client_must_await(c, cb, out) \
    MUST(imap_client_await, (c), (cb), (out))

// call after submitting your final response to the server
// await_cb will be called with E_OK
void imap_server_logged_out(imap_server_t *s);

// idempotent
void imap_server_cancel(imap_server_t *s);
void imap_client_cancel(imap_client_t *c);

// if not awaited, it will stay alive long enough to await itself
// returns ok=false if freeing fails due to pending IO
bool imap_server_free(imap_server_t **server);
bool imap_client_free(imap_client_t **client);

#define imap_server_must_free(s) MUST(imap_server_free, (s))
#define imap_client_must_free(c) MUST(imap_client_free, (c))

struct imap_server_t {
    void *data;  // user data
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

    link_t cmds;  // imap_cmd_t->link
    link_t resps;  // imap_resp_t->link
    link_t reads;  // imap_server_read_t->link
    link_t writes;  // imap_server_write_t->link

    imap_server_await_cb await_cb;

    derr_t e;
    size_t write_skip;
    size_t nwritten;

    bool starttls : 1;
    bool starttls_started : 1;
    bool starttls_done : 1;

    bool greeting_started : 1;
    bool greeting_done : 1;

    /* after we start relaying, all responses original from write requests,
       meaning that when we transition we queue the resp from each write req,
       and thereafter imap_server_write() queues resp straight to s->resps */
    bool relay_started : 1;

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
DEF_STEAL_PTR(imap_server_t)
DEF_CONTAINER_OF(imap_server_t, link, link_t)
DEF_CONTAINER_OF(imap_server_t, schedulable, schedulable_t)

struct imap_client_t {
    void *data;  // user data
    imap_resp_reader_t reader;
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

    link_t cmds;  // imap_cmd_t->link
    link_t resps;  // imap_resp_t->link
    link_t reads;  // imap_client_read_t->link
    link_t writes;  // imap_client_write_t->link

    imap_client_await_cb await_cb;

    derr_t e;
    size_t write_skip;
    size_t nwritten;

    bool wbuf_needs_zero : 1;

    bool starttls : 1;
    bool starttls_started : 1;
    bool starttls_done : 1;

    bool greeting_done : 1;

    bool relay_started : 1;

    bool read_started : 1;
    bool read_done : 1;

    bool write_started : 1;
    bool write_done : 1;

    bool canceled : 1;
    bool failed : 1;
    bool awaited : 1;
};
DEF_STEAL_PTR(imap_client_t)
DEF_CONTAINER_OF(imap_client_t, link, link_t)
DEF_CONTAINER_OF(imap_client_t, schedulable, schedulable_t)
