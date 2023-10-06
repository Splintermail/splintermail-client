// libuv-specific bits that connect the io-agnostic application to actual io

typedef struct {
    uv_tcp_t tcp;
    duv_scheduler_t *scheduler;
    imap_security_e security;
    SSL_CTX *ctx;
    bool configured;
} citm_listener_t;

struct uv_citm_t;
typedef struct uv_citm_t uv_citm_t;

struct uv_citm_t {
    derr_t e;
    citm_io_i iface;
    addrspec_t remote;
    imap_security_e client_sec;
    SSL_CTX *client_ctx;
    dstr_t remote_verify_name;
    uv_loop_t loop;
    uv_async_t async_cancel;
    uv_async_t async_user;
    void (*user_async_hook)(void*, uv_citm_t*);
    void *user_data;
    duv_scheduler_t scheduler;
    citm_t citm;
    citm_listener_t listeners[8];
    size_t nlisteners;
    link_t stubs;  // stub_t->link
    uv_acme_manager_t *uvam;  // NULL if acme not in use
    status_server_t ss;
};

derr_t uv_citm(
    const addrspec_t *lspecs,
    size_t nlspecs,
    const addrspec_t remote,
    const char *key,   // explicit --key (disables acme)
    const char *cert,  // explicit --cert (disables acme)
    dstr_t acme_dirurl,
    char *acme_verify_name,  // may be "pebble" in some test scenarios
    dstr_t sm_baseurl,
    string_builder_t sockpath,
    SSL_CTX *client_ctx,
    string_builder_t sm_dir,
    // function pointers, mainly for instrumenting tests:
    void (*indicate_ready)(void*, uv_citm_t*),
    void (*user_async_hook)(void*, uv_citm_t*),
    void *user_data
);

// for the windows service handler
void citm_stop_service(void);

// for instrumenting tests
void uv_citm_async_user(uv_citm_t *uv_citm);
