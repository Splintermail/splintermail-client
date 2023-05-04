// libuv-specific bits that connect the io-agnostic application to actual io

typedef struct {
    uv_tcp_t tcp;
    duv_scheduler_t *scheduler;
    imap_security_e security;
    SSL_CTX *ctx;
} citm_listener_t;

typedef struct {
    citm_io_i iface;
    addrspec_t remote;
    uv_loop_t loop;
    duv_scheduler_t scheduler;
    citm_t citm;
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
