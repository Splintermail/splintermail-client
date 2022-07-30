#include <uv.h>

#include "libdstr/libdstr.h"
#include "server/dns/dns.h"

// owns membuf
derr_t handle_packet(globals_t *g, membuf_t *membuf){
    derr_t e = E_OK;

    dns_pkt_t pkt;
    size_t pret = parse_pkt(&pkt, membuf->base, membuf->len);
    if(pret == BAD_PARSE){
        printf("bad packet!\n");
    }else{
        printf("good packet!\n");
        print_pkt(pkt);
    }

    membuf_return(g, &membuf);

    return e;
}
