#define NMEMBUFS 256
#define MEMBUFSIZE 4096

typedef struct {
    char base[MEMBUFSIZE];
    char resp[MEMBUFSIZE];
    link_t link; // globals_t->membufs;
    link_t *pool; // for easily returning a membuf to its pool
    // structs for sending the response
    uv_udp_send_t req;
    uv_buf_t uvbuf;
} membuf_t;
DEF_CONTAINER_OF(membuf_t, base, char)
DEF_CONTAINER_OF(membuf_t, link, link_t)
DEF_CONTAINER_OF(membuf_t, req, uv_udp_send_t)
DEF_STEAL_PTR(membuf_t)

void membuf_free(membuf_t **ptr);
derr_t membuf_init(membuf_t **ptr);
void membufs_free(link_t *membufs);
derr_t membufs_init(link_t *membufs, size_t n);
membuf_t *membufs_pop(link_t *membufs);
void membuf_return(membuf_t **ptr);
