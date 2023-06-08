#include <stdlib.h>
#include <time.h>
#include <limits.h>

#include <libdstr/libdstr.h>

#include "test_utils.h"

#define NUM_INTS 1000


// struct to be packed into the heap for testing
typedef struct {
    int value;
    hnode_t hnode;
    unsigned char magic;
} int_val_t;
DEF_CONTAINER_OF(int_val_t, hnode, hnode_t)

static const void *get_int_val(const hnode_t *hnode){
    int_val_t *b = CONTAINER_OF(hnode, int_val_t, hnode);
    return &b->value;
}

static void _print_heap(const hnode_t *hnode, size_t indent){
    // skip nil nodes
    if(!hnode) return;
    // build indent
    DSTR_STATIC(space, "                                                    ");
    dstr_t indent_dstr = dstr_sub2(space, 0, indent);
    // print left child
    _print_heap(hnode->children[0], indent + 1);
    // print node
    LOG_DEBUG(
        "%x%xB%x\n",
        FD(indent_dstr),
        FI(CONTAINER_OF(hnode, int_val_t, hnode)->value),
        FI(hnode->balance)
    );
    // print right child
    _print_heap(hnode->children[1], indent + 1);
    return;
}

static void print_heap(heap_t *h){
    _print_heap(heap_peek(h), 0);
}

// call this for every node in the tree
static derr_t node_assertions(
    const heap_t *h,
    const hnode_t *parent,
    const hnode_t *hnode,
    int *count_out,
    bool expect_balanced
){
    derr_t e = E_OK;

    *count_out = 0;

    // skip tests on nil nodes
    if(!hnode){
        return e;
    }

    EXPECT_P(&e, "parent", hnode->parent, parent);

    const hnode_t *l = hnode->children[0];
    const hnode_t *r = hnode->children[1];

    // every node is better than its children
    const int *nval = h->get(hnode);
    if(l){
        const int *lval = h->get(l);
        if(h->want_max){
            EXPECT_I_GE(&e, "hnode", *nval, *lval);
        }else{
            EXPECT_I_LE(&e, "hnode", *nval, *lval);
        }
    }
    if(r){
        const int *rval = h->get(r);
        if(h->want_max){
            EXPECT_I_GE(&e, "hnode", *nval, *rval);
        }else{
            EXPECT_I_LE(&e, "hnode", *nval, *rval);
        }
    }

    // descend into child nodes
    int lcount, rcount;
    PROP(&e, node_assertions(h, hnode, l, &lcount, expect_balanced) );
    PROP(&e, node_assertions(h, hnode, r, &rcount, expect_balanced) );

    // balance calculation must always be correct
    EXPECT_I(&e, "balance", hnode->balance, rcount - lcount);

    // we may or may not expect the tree to actually be balanced right now
    if(expect_balanced){
        EXPECT_I_LE(&e, "ABS(hnode->balance)", ABS(hnode->balance), 1);
    }

    *count_out = 1 + lcount + rcount;

    return e;
}

static derr_t heap_assertions(const heap_t *h, bool expect_balanced){
    derr_t e = E_OK;
    int ignore;
    PROP(&e,
        node_assertions(
            h,
            &h->root_parent,
            h->root_parent.children[0],
            &ignore,
            expect_balanced
        )
    );
    return e;
}

static derr_t do_test_heap(int_val_t *ints, size_t num_ints){
    derr_t e = E_OK;

    // test once with want_max, and once without
    for(size_t i = 0; i < 2; i++){
        bool want_max = !!i;
        heap_t h;
        heap_prep(&h, jsw_cmp_int, get_int_val, want_max);

        // insert the first 2/3 of elements
        for(size_t i = 0; i < num_ints * 2 / 3; i++){
            heap_put(&h, &ints[i].hnode);
            // heap is always balanced with pure insertions
            PROP(&e, heap_assertions(&h, true) );
        }

        print_heap(&h);

        // delete the first third of nodes directly
        for(size_t i = 0; i < num_ints / 3; i++){
            hnode_remove(&ints[i].hnode);
            // always safe to double-remove
            hnode_remove(&ints[i].hnode);
            print_heap(&h);
            // heap will not likely be balanced
            PROP(&e, heap_assertions(&h, false) );
        }

        // add the final third of ints
        for(size_t i = num_ints * 2 / 3; i < num_ints; i++){
            heap_put(&h, &ints[i].hnode);
            // heap may not be balanced yet
            PROP(&e, heap_assertions(&h, false) );
        }

        // back at highwater capacity, should be balanced again
        PROP(&e, heap_assertions(&h, true) );

        // print heap
        print_heap(&h);

        // pop the last two thirds elements
        int prev = want_max ? INT_MAX : INT_MIN;
        for(size_t i = num_ints / 3; i < num_ints; i++){
            hnode_t *hnode = heap_pop(&h);
            EXPECT_NOT_NULL(&e, "heap_pop()", hnode);
            int val = CONTAINER_OF(hnode, int_val_t, hnode)->value;
            if(want_max){
                EXPECT_I_LE(&e, "heap_pop().value", val, prev);
            }else{
                EXPECT_I_GE(&e, "heap_pop().value", val, prev);
            }
            // heap will not likely be balanced
            PROP(&e, heap_assertions(&h, false) );
        }

        // tree should be empty
        hnode_t *hnode = heap_pop(&h);
        EXPECT_NULL(&e, "heap_pop()", hnode);
    }

    return e;
}

static derr_t test_heap(void){
    derr_t e = E_OK;
    // some values to be inserted and stuff
    int_val_t ints[NUM_INTS];
    // prep all the int_val_t's self-pointers
    for(size_t i = 0; i < NUM_INTS; i++){
        ints[i].magic = (unsigned char)0xAA;
    }
    // ascending values
    LOG_DEBUG("testing with ascending values\n");
    {
        for(int i = 0; i < NUM_INTS; i++){
            ints[i].value = i;
        }
        PROP(&e, do_test_heap(ints, NUM_INTS) );
    }
    // descending values
    LOG_DEBUG("testing with descending values\n");
    {
        for(int i = 0; i < NUM_INTS; i++){
            ints[i].value = NUM_INTS - i;
        }
        PROP(&e, do_test_heap(ints, NUM_INTS) );
    }
    // identical values
    LOG_DEBUG("testing with identical values\n");
    {
        for(int i = 0; i < NUM_INTS; i++){
            ints[i].value = 0;
        }
        PROP(&e, do_test_heap(ints, NUM_INTS) );
    }
    // random values
    LOG_DEBUG("testing with random values\n");
    {
        for(int i = 0; i < NUM_INTS; i++){
            ints[i].value = rand();
        }
        PROP(&e, do_test_heap(ints, NUM_INTS) );
    }
    return e;
}

static derr_t test_zeroized(void){
    derr_t e = E_OK;

    heap_t h = {0};

    EXPECT_NULL(&e, "heap_pop()", heap_pop(&h));

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    // random number seed
    unsigned int seed = (unsigned int)(time(NULL) % UINT_MAX);
    srand(seed);
    LOG_ERROR("using seed: %x\n", FU(seed));

    PROP_GO(&e, test_heap(), test_fail);
    // PROP_GO(&e, test_indicies(), test_fail);
    // PROP_GO(&e, test_apop(), test_fail);
    // PROP_GO(&e, test_atrav(), test_fail);
    PROP_GO(&e, test_zeroized(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
