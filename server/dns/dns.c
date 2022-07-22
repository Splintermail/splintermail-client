#include <uv.h>

#include "libdstr/libdstr.h"
#include "server/dns/dns.h"

// owns membuf
derr_t handle_packet(globals_t *g, membuf_t *membuf){
    derr_t e = E_OK;

    printf("handle_packet: %.*s\n", (int)membuf->len, membuf->base);
    membuf_return(g, &membuf);

    return e;
}
