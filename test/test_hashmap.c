#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>

#include <libdstr/libdstr.h>

#include "test_utils.h"

#define UINT_ELEMS 1000
#define DSTR_ELEMS (26*26)

typedef struct {
    unsigned int n;
    dstr_t d;
    char buf[3];
    hash_elem_t he;
} hashable_t;
DEF_CONTAINER_OF(hashable_t, he, hash_elem_t)

static derr_t test_hashmap(void){
    derr_t e = E_OK;

    // all of the elements
    size_t num_elems = UINT_ELEMS + DSTR_ELEMS;
    hashable_t **elems = malloc(num_elems * sizeof(*elems));
    if(!elems) ORIG(&e, E_NOMEM, "not enough memory");
    for(size_t i = 0; i < num_elems; i++) elems[i] = NULL;

    // init all of the elements
    for(unsigned int i = 0; i < num_elems; i++){
        elems[i] = DMALLOC_STRUCT_PTR(&e, elems[i]);
        CHECK_GO(&e, fail_elems);
        elems[i]->n = i;
        DSTR_WRAP_ARRAY(elems[i]->d, elems[i]->buf);
    }

    // allocate hashmap
    hashmap_t h;
    PROP_GO(&e, hashmap_init(&h), fail_elems);

    // insert everything
    for(unsigned int i = 0; i < UINT_ELEMS; i++){
        hash_elem_t *old = hashmap_setu(&h, i, &elems[i]->he);
        if(old) ORIG_GO(&e, E_VALUE, "insert was unique!", fail_h);
    }
    for(size_t i = UINT_ELEMS; i < num_elems; i++){
        char cmaj = (char)('a' + (i%(26*26) - i%26)/26);
        char cmin = (char)('a' + (i%26));
        PROP_GO(&e, FMT(&elems[i]->d, "%x%x", FC(cmaj), FC(cmin)), fail_h);
        hash_elem_t *old = hashmap_sets(&h, &elems[i]->d, &elems[i]->he);
        EXPECT_NULL_GO(&e, "old", old, fail_h);
    }

    // dereference everything
    for(unsigned int i = 0; i < UINT_ELEMS; i++){
        hash_elem_t *out = hashmap_getu(&h, i);
        EXPECT_NOT_NULL_GO(&e, "out", out, fail_h);
        // make sure we got the right value
        hashable_t *val = CONTAINER_OF(out, hashable_t, he);
        EXPECT_U_GO(&e, "val->n", val->n, i, fail_h);
    }
    for(size_t i = UINT_ELEMS; i < num_elems; i++){
        hash_elem_t *out = hashmap_gets(&h, &elems[i]->d);
        if(!out) ORIG_GO(&e, E_VALUE, "missing value!", fail_h);
        // make sure we got the right value
        hashable_t *val = CONTAINER_OF(out, hashable_t, he);
        EXPECT_U_GO(&e, "val->n", val->n, i, fail_h);
    }

    // iterate through everything
    size_t count = 0;
    hashmap_trav_t trav;
    hash_elem_t *elem = hashmap_iter(&trav, &h);
    for(size_t i = 0; elem; i++, elem = hashmap_next(&trav)){
        if(++count > num_elems){
            ORIG_GO(&e, E_VALUE, "iterated too many elements", fail_h);
        }
        // remove half of the elements
        if(i%2) continue;
        hash_elem_remove(elem);
        hashable_t *val = CONTAINER_OF(elem, hashable_t, he);
        elems[val->n] = NULL;
        // actually free the memory to test for use-after-free
        free(val);
    }
    if(count < num_elems){
        ORIG_GO(&e, E_VALUE, "iterated too few elements", fail_h);
    }

    // extra next is ok
    EXPECT_NULL_GO(&e, "extra next", hashmap_next(&trav), fail_h);

    // again, but popping everything
    count = 0;
    elem = hashmap_pop_iter(&trav, &h);
    for( ; elem; elem = hashmap_pop_next(&trav)){
        if(++count > num_elems){
            ORIG_GO(&e, E_VALUE, "iterated too many elements", fail_h);
        }
        // hash_elem_remove should be a noop
        hash_elem_remove(elem);
        hashable_t *val = CONTAINER_OF(elem, hashable_t, he);
        elems[val->n] = NULL;
        // actually free the memory to test for use-after-free
        free(val);
    }
    if(count < num_elems/2){
        ORIG_GO(&e, E_VALUE, "iterated too few elements", fail_h);
    }
    if(h.num_elems != 0){
        ORIG_GO(&e, E_VALUE, "hashmap should be empty", fail_h);
    }
    // extra pops are ok
    EXPECT_NULL_GO(&e, "extra pop", hashmap_pop_next(&trav), fail_h);

fail_h:
    hashmap_free(&h);
fail_elems:
    for(size_t i = 0; i < num_elems; i++){
        if(elems[i]) free(elems[i]);
    }
    free(elems);
    return e;
}

static derr_t test_empty_iter(void){
    derr_t e = E_OK;
    hashmap_trav_t trav;
    hash_elem_t *elem;
    // pre-init iterate
    hashmap_t h = {0};
    elem = hashmap_iter(&trav, &h);
    if(elem) ORIG(&e, E_VALUE, "iterated too many elements");

    // post-init iterate
    PROP(&e, hashmap_init(&h) );
    elem = hashmap_iter(&trav, &h);
    if(elem) ORIG_GO(&e, E_VALUE, "iterated too many elements", fail_h);

    // post-free iterate
    hashmap_free(&h);
    elem = hashmap_iter(&trav, &h);
    if(elem) ORIG(&e, E_VALUE, "iterated too many elements");

fail_h:
    // double-free check
    hashmap_free(&h);
    return e;
}

static derr_t test_hashmap_del_elem(void){
    derr_t e = E_OK;
    // all of the elements
    size_t num_elems = 10;
    hashable_t elems[10] = {0};

    // allocate hashmap
    hashmap_t h;
    PROP(&e, hashmap_init(&h) );

    // insert all of the elements
    for(unsigned int i = 0; i < num_elems; i++){
        elems[i].n = i;
        if(h.num_elems != i){
            ORIG_GO(&e, E_VALUE, "wrong num_elems", cu);
        }
        hash_elem_t *old = hashmap_setu(&h, i, &elems[i].he);
        if(old != NULL){
            ORIG_GO(&e, E_VALUE, "hashmap_setu() returned non-null", cu);
        }
    }
    if(h.num_elems != num_elems){
        ORIG_GO(&e, E_VALUE, "wrong num_elems", cu);
    }

    // is it safe to delete an element not in the hashmap?
    hash_elem_t not_present_elem = {0};
    hash_elem_remove(&not_present_elem);
    if(h.num_elems != num_elems){
        ORIG_GO(&e, E_VALUE, "wrong num_elems", cu);
    }

    // delete each element
    for(unsigned int i = 0; i < num_elems; i++){
        elems[i].n = i;
        if(h.num_elems != num_elems - i){
            ORIG_GO(&e, E_VALUE, "wrong num_elems", cu);
        }
        hash_elem_remove(&elems[i].he);
        // is it gone?
        hash_elem_t *old = hashmap_getu(&h, i);
        if(old != NULL){
            ORIG_GO(&e, E_VALUE, "hashmap_getu() returned non-null", cu);
        }
        // is remove idempotent?
        hash_elem_remove(&elems[i].he);
    }
    if(h.num_elems != 0){
        ORIG_GO(&e, E_VALUE, "wrong num_elems", cu);
    }

    // is it still safe to delete an element not in the hashmap?
    hash_elem_remove(&not_present_elem);
    if(h.num_elems != 0){
        ORIG_GO(&e, E_VALUE, "wrong num_elems", cu);
    }

cu:
    hashmap_free(&h);
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_WARN);

    PROP_GO(&e, test_hashmap(), test_fail);
    PROP_GO(&e, test_empty_iter(), test_fail);
    PROP_GO(&e, test_hashmap_del_elem(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
