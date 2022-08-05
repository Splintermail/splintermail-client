#include <stdlib.h>

#include <uv.h>

#include "libdstr/libdstr.h"
#include "server/dns/dns.h"


void membuf_free(membuf_t **ptr){
    membuf_t *membuf = *ptr;
    if(!membuf) return;
    free(membuf);
    *ptr = NULL;
}

derr_t membuf_init(membuf_t **ptr){
    derr_t e = E_OK;
    *ptr = NULL;

    membuf_t *membuf = DMALLOC_STRUCT_PTR(&e, membuf);
    CHECK(&e);

    *membuf = (membuf_t){0};
    link_init(&membuf->link);

    *ptr = membuf;

    return e;
}

void membufs_free(link_t *membufs){
    link_t *link;
    while((link = link_list_pop_first(membufs))){
        membuf_t *membuf = CONTAINER_OF(link, membuf_t, link);
        membuf_free(&membuf);
    }
}

derr_t membufs_init(link_t *membufs, size_t n){
    derr_t e = E_OK;

    link_init(membufs);

    for(size_t i = 0; i < n; i++){
        membuf_t *membuf;
        PROP_GO(&e, membuf_init(&membuf), fail);
        link_list_append(membufs, &membuf->link);
    }

    return e;

fail:
    membufs_free(membufs);
    return e;
}

membuf_t *membufs_pop(link_t *membufs){
    link_t *link = link_list_pop_first(membufs);
    if(!link) return NULL;
    membuf_t *membuf = CONTAINER_OF(link, membuf_t, link);
    membuf->pool = membufs;
    return membuf;
}

void membuf_return(membuf_t **ptr){
    membuf_t *membuf = *ptr;
    link_list_append(membuf->pool, &membuf->link);
    membuf->pool = NULL;
    *ptr = NULL;
}
