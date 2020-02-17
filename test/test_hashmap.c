#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <libdstr/common.h>
#include <libdstr/logger.h>
#include <libdstr/hashmap.h>

#include "test_utils.h"

#define UINT_ELEMS 1000
#define DSTR_ELEMS (26*26)

typedef struct {
    unsigned int n;
    dstr_t d;
    hash_elem_t he;
} hashable_t;
DEF_CONTAINER_OF(hashable_t, he, hash_elem_t);

static derr_t test_hashmap(void){
    derr_t e = E_OK;

    // all of the elements
    size_t num_elems = UINT_ELEMS + DSTR_ELEMS;
    hashable_t *elems = malloc(num_elems * sizeof(*elems));
    if(!elems) ORIG(&e, E_NOMEM, "not enough memory");

    // init all of the elements
    for(unsigned int i = 0; i < num_elems; i++){
        elems[i].n = i;
        elems[i].d = (dstr_t){0};
    }
    // now go through and set all of the dstr-type keys
    for(size_t i = UINT_ELEMS; i < num_elems; i++){
        PROP_GO(&e, dstr_new(&elems[i].d, 3), fail_elems);
    }

    // allocate hashmap
    hashmap_t h;
    PROP_GO(&e, hashmap_init(&h), fail_elems);

    // insert everything
    for(unsigned int i = 0; i < UINT_ELEMS; i++){
        hash_elem_t *old = hashmap_setu(&h, i, &elems[i].he);
        if(old) ORIG_GO(&e, E_VALUE, "insert was unique!", fail_h);
    }
    for(size_t i = UINT_ELEMS; i < num_elems; i++){
        char cmaj = (char)('a' + (i%(26*26) - i%26)/26);
        char cmin = (char)('a' + (i%26));
        PROP_GO(&e, FMT(&elems[i].d, "%x%x", FC(cmaj), FC(cmin)), fail_h);
        hash_elem_t *old = hashmap_sets(&h, &elems[i].d, &elems[i].he);
        if(old) ORIG_GO(&e, E_VALUE, "insert was unique!", fail_h);
    }

    // dereference everything
    for(unsigned int i = 0; i < UINT_ELEMS; i++){
        hash_elem_t *out = hashmap_getu(&h, i);
        if(!out) ORIG_GO(&e, E_VALUE, "missing value!", fail_h);
        // make sure we got the right value
        hashable_t *val = CONTAINER_OF(out, hashable_t, he);
        if(val->n != i) ORIG_GO(&e, E_VALUE, "dereferenced wrong value", fail_h);
    }
    for(size_t i = UINT_ELEMS; i < num_elems; i++){
        hash_elem_t *out = hashmap_gets(&h, &elems[i].d);
        if(!out) ORIG_GO(&e, E_VALUE, "missing value!", fail_h);
        // make sure we got the right value
        hashable_t *val = CONTAINER_OF(out, hashable_t, he);
        if(val->n != i) ORIG_GO(&e, E_VALUE, "dereferenced wrong value", fail_h);
    }

    // iterate through everything
    hashmap_iter_t i;
    size_t count = 0;
    for(i = hashmap_first(&h); i.current; hashmap_next(&i)){
        if(++count > num_elems)
            ORIG_GO(&e, E_VALUE, "iterated too many elements", fail_h);
    }
    if(count < num_elems)
        ORIG_GO(&e, E_VALUE, "iterated too few elements", fail_h);

    // again, but popping
    count = 0;
    for(i = hashmap_pop_first(&h); i.current; hashmap_pop_next(&i)){
        if(++count > num_elems)
            ORIG_GO(&e, E_VALUE, "iterated too many elements", fail_h);
    }
    if(count < num_elems)
        ORIG_GO(&e, E_VALUE, "iterated too few elements", fail_h);
    if(h.num_elems != 0)
        ORIG_GO(&e, E_VALUE, "hashmap should be empty", fail_h);

fail_h:
    hashmap_free(&h);
fail_elems:
    // free all of the dstr's
    for(size_t i = UINT_ELEMS; i < num_elems; i++){
        dstr_free(&elems[i].d);
    }
    free(elems);
    return e;
}

static derr_t test_empty_iter(void){
    derr_t e = E_OK;
    hashmap_t h;
    PROP(&e, hashmap_init(&h) );
    for(hashmap_iter_t i = hashmap_first(&h); i.current; hashmap_next(&i)){
        ORIG_GO(&e, E_VALUE, "iterated too many elements", fail_h);
    }
fail_h:
    hashmap_free(&h);
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_WARN);

    PROP_GO(&e, test_hashmap(), test_fail);
    PROP_GO(&e, test_empty_iter(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
