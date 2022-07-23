#include <uv.h>

#include "libdstr/libdstr.h"
#include "server/dns/dns.h"

// owns membuf
derr_t handle_packet(globals_t *g, membuf_t *membuf){
    derr_t e = E_OK;

    dns_pkt_t pkt;
    bool ok = parse_pkt(&pkt, membuf->base, membuf->len);
    if(!ok){
        printf("bad packet!\n");
    }else{
        printf("good packet!\n");
        print_pkt(pkt);
    }

    membuf_return(g, &membuf);

    return e;
}
