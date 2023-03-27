// libuv-specific bits that connect the io-agnostic application to actual io

typedef struct {
    uv_tcp_t tcp;
    duv_scheduler_t *scheduler;
    imap_security_e security;
    SSL_CTX *ctx;
    bool configured;
} citm_listener_t;

typedef struct {
    derr_t e;
    citm_io_i iface;
    addrspec_t remote;
    imap_security_e client_sec;
    SSL_CTX *client_ctx;
    dstr_t remote_verify_name;
    uv_loop_t loop;
    uv_async_t async_cancel;
    duv_scheduler_t scheduler;
    citm_t citm;
    citm_listener_t listeners[8];
    size_t nlisteners;
} uv_citm_t;

derr_t uv_citm(
    const addrspec_t *lspecs,
    size_t nlspecs,
    const addrspec_t remote,
    const char *key,
    const char *cert,
    string_builder_t maildir_root,
    bool indicate_ready
);

// for the windows service handler
void citm_stop_service(void);
