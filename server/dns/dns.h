#define NMEMBUFS 256
#define MEMBUFSIZE 4096

typedef struct {
    uv_loop_t loop;
    uv_udp_t udp;
    link_t membufs;  // membuf_t->link
    bool closing;
    derr_t close_reason;
} globals_t;

typedef struct {
    char base[MEMBUFSIZE];
    size_t len;
    link_t link; // globals_t->membufs;
} membuf_t;
DEF_CONTAINER_OF(membuf_t, base, char);
DEF_CONTAINER_OF(membuf_t, link, link_t);
DEF_STEAL_PTR(membuf_t);

// main.c //

// only to be called from the top-level libuv callbacks
void dns_close(globals_t *g, derr_t e);

// membuf.c //

void membuf_free(membuf_t **ptr);
derr_t membuf_init(membuf_t **ptr);
void membufs_free(link_t *membufs);
derr_t membufs_init(link_t *membufs, size_t n);
void membuf_return(globals_t *g, membuf_t **ptr);

// dns.c //

// owns membuf
derr_t handle_packet(globals_t *g, membuf_t *membuf);
