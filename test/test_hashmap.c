#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <common.h>
#include <logger.h>
#include <hashmap.h>

#include "test_utils.h"

#define UINT_ELEMS 1000
#define DSTR_ELEMS (26*26)

typedef struct {
    unsigned int n;
    dstr_t d;
    hash_elem_t helem;
} hashable_t;

static derr_t test_hashmap(void){
    derr_t error;

    // all of the elements
    size_t num_elems = UINT_ELEMS + DSTR_ELEMS;
    hashable_t *elems = malloc(num_elems * sizeof(*elems));
    if(!elems) ORIG(E_NOMEM, "not enough memory");

    // init all of the elements
    for(unsigned int i = 0; i < num_elems; i++){
        elems[i].n = i;
        elems[i].d = (dstr_t){0};
        elems[i].helem.data = &elems[i];
    }
    // now go through and set all of the dstr-type keys
    for(size_t i = UINT_ELEMS; i < num_elems; i++){
        PROP_GO( dstr_new(&elems[i].d, 3), fail_elems);
    }

    // allocate hashmap
    hashmap_t h;
    PROP_GO( hashmap_init(&h), fail_elems);

    // insert everything
    for(unsigned int i = 0; i < UINT_ELEMS; i++){
        PROP_GO( hashmap_putu(&h, i, &elems[i].helem), fail_hm);
    }
    for(size_t i = UINT_ELEMS; i < num_elems; i++){
        char cmaj = (char)('a' + (i%(26*26) - i%26)/26);
        char cmin = (char)('a' + (i%26));
        PROP_GO( FMT(&elems[i].d, "%x%x", FC(cmaj), FC(cmin)), fail_hm);
        PROP_GO( hashmap_puts(&h, &elems[i].d, &elems[i].helem), fail_hm);
    }

    // dereference everything
    for(unsigned int i = 0; i < UINT_ELEMS; i++){
        void *val;
        PROP_GO( hashmap_getu(&h, i, &val, NULL), fail_hm);
        // make sure we got the right value
        hashable_t *out = val;
        if(out->n != i) ORIG_GO(E_VALUE, "dereferenced wrong value", fail_hm);
    }
    for(size_t i = UINT_ELEMS; i < num_elems; i++){
        void *val;
        PROP_GO( hashmap_gets(&h, &elems[i].d, &val, NULL), fail_hm);
        // make sure we got the right value
        hashable_t *out = val;
        if(out->n != i) ORIG_GO(E_VALUE, "dereferenced wrong value", fail_hm);
    }

    // iterate through everything
    size_t count = 0;
    for(hashmap_iter_t i = hashmap_first(&h); i.more; hashmap_next(&i)){
        if(++count > num_elems)  ORIG(E_VALUE, "iterated too many elements");
    }
    if(count < num_elems) ORIG(E_VALUE, "iterated too few elements");

fail_hm:
    hashmap_free(&h);
fail_elems:
    // free all of the dstr's
    for(size_t i = UINT_ELEMS; i < num_elems; i++){
        dstr_free(&elems[i].d);
    }
    free(elems);
    return error;
}

int main(int argc, char** argv){
    derr_t error;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_WARN);

    PROP_GO( test_hashmap(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    LOG_ERROR("FAIL\n");
    return 1;
}
