// libuv-specific bits that connect the io-agnostic application to actual io

typedef struct {
    uv_tcp_t listener;
    duv_scheduler_t *scheduler;
    imap_security_e security;
    SSL_CTX *ctx;
    link_t link;
} citm_listener_t;
DEF_CONTAINER_OF(citm_listener_t, link, link_t)

typedef struct {
    citm_io_i iface;
    uv_loop_t loop;
    duv_scheduler_t scheduler;
    citm_t *citm;
} uv_citm_t;
