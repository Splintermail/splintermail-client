#include <stdlib.h>
#include <time.h>
#include <limits.h>

#include <libdstr/libdstr.h>

#include "test_utils.h"

#define NUM_INTS 1000


// struct to be packed into binary search tree, via jsw_anode_t hanger
typedef struct {
    int value;
    jsw_anode_t anode;
    unsigned char magic;
} binsrch_int_t;
DEF_CONTAINER_OF(binsrch_int_t, anode, jsw_anode_t)

static const void *get_binsrch(const jsw_anode_t *node){
    binsrch_int_t *b = CONTAINER_OF(node, binsrch_int_t, anode);
    return &b->value;
}

static derr_t print_atree(const jsw_anode_t *node, size_t indent){
    derr_t e = E_OK;
    // skip nil nodes
    if(!node->level) return e;
    // build indent
    DSTR_STATIC(space, "                                                    ");
    dstr_t indent_dstr = dstr_sub2(space, 0, indent);
    // print left child
    print_atree(node->link[0], indent + 1);
    // print node
    LOG_DEBUG("%x%xL%x(%x)\n", FD(&indent_dstr),
            FI(CONTAINER_OF(node, binsrch_int_t, anode)->value),
            FI(node->level),
            FU(node->count));
    // print right child
    print_atree(node->link[1], indent + 1);
    return e;
}

// call this for every node in the tree
static derr_t recursive_anode_assertions(const jsw_atree_t *tree,
                                         const jsw_anode_t *node){
    derr_t e = E_OK;
    // skip tests on nil nodes
    if(node == &tree->nil){
        return e;
    }

    // no left-horizontal links
    jsw_anode_t *left = node->link[0];
    jsw_anode_t *right = node->link[1];
    if(left->level == node->level){
        ORIG(&e, E_VALUE, "left-horizontal link detected");
    }

    // no double right-horizontal links
    if(right->level == node->level){
        jsw_anode_t *rightright = right->link[1];
        if(rightright != &tree->nil && rightright->level == right->level){
            ORIG(&e, E_VALUE, "double right-horizontal link detected" );
        }
    }

    // count is the sum of the counts of the children
    if(node->count != left->count + right->count + 1){
        print_atree(node, 0);
        ORIG(&e, E_VALUE, "invalid count detected" );
    }

    // link levels are valid (ensures same number of pseudo-nodes for any path)
    int nlvl = node->level;
    int llvl = left->level;
    int rlvl = right->level;
    // left child nodes have level one-less-than node
    if(left != &tree->nil && llvl != nlvl - 1){
        ORIG(&e, E_VALUE, "invalid node leveling (leftwards)");
    }
    // right child nodes have level equal or one-less-than node
    if(right != &tree->nil && (rlvl > nlvl || rlvl < nlvl - 1)){
        ORIG(&e, E_VALUE, "invalid node leveling (rightwards)");
    }
    // anything with a null child is the bottom pseudonode (level of 1)
    if((left == &tree->nil || right == &tree->nil) && nlvl != 1){
        ORIG(&e, E_VALUE, "invalid node leveling (bottom)");
    }
    // no double horizontal links
    if(nlvl == right->link[1]->level){
        ORIG(&e, E_VALUE, "double horizontal right links");
    }

    // links are well-ordered
    int nval = CONTAINER_OF(node, binsrch_int_t, anode)->value;
    if(left != &tree->nil){
        int lval = CONTAINER_OF(left, binsrch_int_t, anode)->value;
        if(lval > nval){
            TRACE(&e, "lmagic %x nmagic %x\n",
                    FU(CONTAINER_OF(left, binsrch_int_t, anode)->magic),
                    FU(CONTAINER_OF(node, binsrch_int_t, anode)->magic));
            TRACE(&e, "lval %x nval %x\n", FI(lval), FI(nval));
            ORIG(&e, E_VALUE, "invalid node ordering (leftwards)");
        }
    }
    if(right != &tree->nil){
        int rval = CONTAINER_OF(right, binsrch_int_t, anode)->value;
        if(nval > rval){
            ORIG(&e, E_VALUE, "invalid node ordering (rightwards)");
        }
    }

    // apply test recursively
    PROP(&e, recursive_anode_assertions(tree, left) );
    PROP(&e, recursive_anode_assertions(tree, right) );

    return e;
}

static derr_t atree_assertions(const jsw_atree_t *tree){
    derr_t e = E_OK;
    PROP(&e, recursive_anode_assertions(tree, tree->root) );
    return e;
}

static derr_t do_test_atree(binsrch_int_t *ints, size_t num_ints){
    derr_t e = E_OK;
    // the andersson tree
    jsw_atree_t tree;
    jsw_ainit(&tree, jsw_cmp_int, get_binsrch);
    // insert each element
    for(size_t i = 0; i < num_ints; i++){
        jsw_ainsert(&tree, &ints[i].anode);
        // make sure all the properties of the atree are met
        PROP(&e, atree_assertions(&tree) );
    }
    // print tree
    print_atree(tree.root, 0);
    // delete each element
    for(size_t i = 0; i < num_ints; i++){
        jsw_aerase(&tree, &ints[i]);
        // make sure all the properties of the atree are met
        PROP(&e, atree_assertions(&tree) );
    }
    // tree should be empty
    if(tree.size != 0){
        ORIG(&e, E_VALUE, "tree should be empty" );
    }

    return e;
}

static derr_t test_atree(void){
    derr_t e = E_OK;
    // some values to be inserted and stuff
    binsrch_int_t ints[NUM_INTS];
    // prep all the binsrch_int_t's self-pointers
    for(size_t i = 0; i < NUM_INTS; i++){
        ints[i].magic = (unsigned char)0xAA;
    }
    // ascending values
    LOG_DEBUG("testing with ascending values\n");
    {
        for(int i = 0; i < NUM_INTS; i++){
            ints[i].value = i;
        }
        PROP(&e, do_test_atree(ints, NUM_INTS) );
    }
    // descending values
    LOG_DEBUG("testing with descending values\n");
    {
        for(int i = 0; i < NUM_INTS; i++){
            ints[i].value = NUM_INTS - i;
        }
        PROP(&e, do_test_atree(ints, NUM_INTS) );
    }
    // identical values
    LOG_DEBUG("testing with identical values\n");
    {
        for(int i = 0; i < NUM_INTS; i++){
            ints[i].value = 0;
        }
        PROP(&e, do_test_atree(ints, NUM_INTS) );
    }
    // random values
    LOG_DEBUG("testing with random values\n");
    {
        for(int i = 0; i < NUM_INTS; i++){
            ints[i].value = rand();
        }
        PROP(&e, do_test_atree(ints, NUM_INTS) );
    }
    return e;
}

static derr_t test_indicies(void){
    derr_t e = E_OK;
    // elements
    binsrch_int_t ints[NUM_INTS];
    for(int i = 0; i < NUM_INTS; i++){
        ints[i].value = i;
    }
    // the andersson tree
    jsw_atree_t tree;
    jsw_ainit(&tree, jsw_cmp_int, get_binsrch);
    // insert each element
    for(size_t i = 0; i < NUM_INTS; i++){
        jsw_ainsert(&tree, &ints[i].anode);
    }
    // check each index after deref-by-value
    for(size_t i = 0; i < NUM_INTS; i++){
        size_t idx_out;
        jsw_anode_t *result = jsw_afind(&tree, &ints[i], &idx_out);
        if(result == NULL)
            ORIG_GO(&e, E_VALUE, "failed to deref-by-value", done);
        if(idx_out != i){
            TRACE(&e, "expected %x got %x\n", FU(i), FU(idx_out));
            ORIG_GO(&e, E_VALUE, "deref-by-value returned wrong index", done);
        }
    }
    // check each value after deref-by-index
    for(size_t i = 0; i < NUM_INTS; i++){
        jsw_anode_t *result = jsw_aindex(&tree, i);
        if(result == NULL)
            ORIG_GO(&e, E_VALUE, "failed to deref-by-index", done);
        binsrch_int_t *bsi = CONTAINER_OF(result, binsrch_int_t, anode);
        if(bsi->value != (int)i){
            TRACE(&e, "expected %x got %x\n", FU(i), FI(bsi->value));
            ORIG_GO(&e, E_VALUE, "deref-by-index returned wrong value", done);
        }
    }

done:
    // print for debugging
    print_atree(tree.root, 0);
    return e;
}

// code mostly copied from do_test_atree
static derr_t test_apop(void){
    derr_t e = E_OK;
    // some values to be inserted and stuff
    binsrch_int_t ints[NUM_INTS];
    // prep all the binsrch_int_t's
    for(int i = 0; i < NUM_INTS; i++){
        ints[i].magic = (unsigned char)0xAA;
        ints[i].value = i;
    }
    // the andersson tree
    jsw_atree_t tree;
    jsw_ainit(&tree, jsw_cmp_int, get_binsrch);
    // insert each element
    for(size_t i = 0; i < NUM_INTS; i++){
        jsw_ainsert(&tree, &ints[i].anode);
        // make sure all the properties of the atree are met
        PROP(&e, atree_assertions(&tree) );
    }
    // print tree
    print_atree(tree.root, 0);
    // pop each element
    jsw_anode_t *node = jsw_apop(&tree);
    size_t count = 0;
    for(; node != NULL; node = jsw_apop(&tree)){
        // make sure all the properties of the atree are met
        PROP(&e, atree_assertions(&tree) );
        count++;
    }
    // count should match NUM_INTS
    if(count != NUM_INTS){
        TRACE(&e, "popped %x elements instead of %x\n", FU(count),
                FU(NUM_INTS));
        ORIG(&e, E_VALUE, "popped wrong number of elements");
    }
    // tree should be empty
    if(tree.size != 0){
        ORIG(&e, E_VALUE, "tree should be empty" );
    }

    return e;
}

/* Traverse a tree, popping a bunch of elements. At each point, step forwards
   and backwards to make sure that we don't get weird effects from popping
   while we are in the middle of a traversal. */
static derr_t test_atrav(void){
    derr_t e = E_OK;
    // some values to be inserted and stuff
    binsrch_int_t ints[NUM_INTS];
    // prep all the binsrch_int_t's
    for(int i = 0; i < NUM_INTS; i++){
        ints[i].magic = (unsigned char)0xAA;
        ints[i].value = i;
    }
    // the andersson tree
    jsw_atree_t tree;
    jsw_ainit(&tree, jsw_cmp_int, get_binsrch);
    // insert each element
    for(size_t i = 0; i < NUM_INTS; i++){
        jsw_ainsert(&tree, &ints[i].anode);
    }
    // walk the tree, popping first, last, and odd elements
    size_t i = 0;
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, &tree);
    while(true){
        binsrch_int_t *bsi = CONTAINER_OF(node, binsrch_int_t, anode);
        if(bsi->value != (int)i){
            TRACE(&e, "expected %x got %x\n", FU(i), FI(bsi->value));
            ORIG(&e, E_VALUE, "found wrong value while walking");
        }

        // check the result of stepping backwards then forwards
        jsw_atrav_t trav_copy = trav;
        jsw_anode_t *node2 = jsw_atprev(&trav);
        if(i < 3){
            if(node2 != NULL) ORIG(&e, E_VALUE, "expected NULL backstep");
            // oops, we broke trav, so restore it
            trav = trav_copy;
        }else{
            if(node2 == NULL) ORIG(&e, E_VALUE, "expected non-NULL backstep");

            int exp = (int)i - 1;
            if(exp % 2){
                exp--;
            }

            binsrch_int_t *bsi2 = CONTAINER_OF(node2, binsrch_int_t, anode);
            if(bsi2->value != exp){
                TRACE(&e, "expected %x got %x\n", FI(exp), FI(bsi2->value));
                ORIG(&e, E_VALUE, "found wrong value in backstep");
            }

            // step forwards again to put trav back where the want it
            node2 = jsw_atnext(&trav);

            // should be back where we started
            if(node2 != node){
                ORIG(&e, E_VALUE, "backstep reset failed");
            }
        }

        // now advance the node, popping first/last/odd elements
        if(i == 0 || i == NUM_INTS || i % 2 ){
            node = jsw_pop_atnext(&trav);
        }else{
            node = jsw_atnext(&trav);
        }
        i++;

        // check end-of-iteration conditions
        if(i == NUM_INTS){
            if(node != NULL){
                ORIG(&e, E_VALUE, "expected NULL atnext");
            }
            break;
        }

        // check the result of stepping forwards then backwards
        trav_copy = trav;
        node2 = jsw_atnext(&trav);
        if(i == NUM_INTS - 1){
            if(node2 != NULL){
                ORIG(&e, E_VALUE, "expected NULL forestep");
            }
            // oops, we broke trav, so restore it
            trav = trav_copy;
        }else{
            if(node2 == NULL) ORIG(&e, E_VALUE, "expected non-NULL forestep");

            int exp = (int)i + 1;

            binsrch_int_t *bsi2 = CONTAINER_OF(node2, binsrch_int_t, anode);
            if(bsi2->value != exp){
                TRACE(&e, "expected %x got %x\n", FI(exp), FI(bsi2->value));
                ORIG(&e, E_VALUE, "found wrong value in forestep");
            }

            // step backwards again to put trav back where the want it
            node2 = jsw_atprev(&trav);

            // should be back where we started
            if(node2 != node){
                ORIG(&e, E_VALUE, "forestep reset failed");
            }
        }
    }

    // tree should be reduced in size
    size_t exp_size = NUM_INTS;
    // expect no odds
    exp_size -= NUM_INTS/2;
    // expect no zero element
    exp_size -= (NUM_INTS > 0);
    // expect no final element
    exp_size -= (NUM_INTS % 2);
    if(tree.size != exp_size){
        TRACE(&e, "expected %x got %x\n", FU(tree.size), FU(exp_size));
        ORIG(&e, E_VALUE, "tree should be empty" );
    }

    return e;
}

/* a zeriozed object should be safe to iterate through, since that is a common
   operation in freeing other objects */
static derr_t test_iter_zeroized(void){
    derr_t e = E_OK;

    jsw_atree_t tree = {0};

    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, &tree);
    if(node != NULL){
        ORIG(&e, E_VALUE, "zeroized tree should return NULL when iterated" );
    }

    node = jsw_atlast(&trav, &tree);
    if(node != NULL){
        ORIG(&e, E_VALUE, "zeroized tree should return NULL when iterated" );
    }

    node = jsw_apop(&tree);
    if(node != NULL){
        ORIG(&e, E_VALUE, "zeroized tree should return NULL for jsw_apop" );
    }

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

    PROP_GO(&e, test_atree(), test_fail);
    PROP_GO(&e, test_indicies(), test_fail);
    PROP_GO(&e, test_apop(), test_fail);
    PROP_GO(&e, test_atrav(), test_fail);
    PROP_GO(&e, test_iter_zeroized(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
