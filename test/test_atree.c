#include <stdlib.h>
#include <time.h>
#include <limits.h>

#include <common.h>
#include <logger.h>
#include <jsw_atree.h>

#include "test_utils.h"

#define NUM_INTS 1000


// struct to be packed into binary search tree, via jsw_anode_t hanger
typedef struct {
    int value;
    jsw_anode_t anode;
    unsigned char magic;
} binsrch_int_t;

static int cmp_binsrch_ints(const void *a, const void *b){
    int aval = ((binsrch_int_t*)a)->value;
    int bval = ((binsrch_int_t*)b)->value;
    if(aval == bval) return 0;
    return aval > bval ? 1 : -1;
}

static void release_binsrch_int(void *i){
    // no mallocs/frees in this test
    (void)i;
    return;
}

// static derr_t print_atree(const jsw_anode_t *node, size_t indent){
//     // skip nil nodes
//     if(!node->level) return E_OK;
//     // build indent
//     DSTR_STATIC(space, "                                                    ");
//     dstr_t indent_dstr = indent ? dstr_sub(&space, 0, indent) : (dstr_t){0};
//     // print left child
//     print_atree(node->link[0], indent + 1);
//     // print node
//     LOG_ERROR("%x%x,%x\n", FD(&indent_dstr),
//                            FI(((binsrch_int_t*)node->data)->value),
//                            FI(node->level));
//     // print right child
//     print_atree(node->link[1], indent + 1);
//     return E_OK;
// }

// call this for every node in the tree
static derr_t recursive_anode_assertions(const jsw_atree_t *tree,
                                         const jsw_anode_t *node){
    // skip tests on nil nodes
    if(node == tree->nil){
        return E_OK;
    }

    // no left-horizontal links
    jsw_anode_t *left = node->link[0];
    jsw_anode_t *right = node->link[1];
    if(left->level == node->level){
        ORIG(E_VALUE, "left-horizontal link detected");
    }

    // no double right-horizontal links
    if(right->level == node->level){
        jsw_anode_t *rightright = right->link[1];
        if(rightright != tree->nil && rightright->level == right->level){
            ORIG(E_VALUE, "double right-horizontal link detected" );
        }
    }

    // link levels are valid (ensures same number of pseudo-nodes for any path)
    int nlvl = node->level;
    int llvl = left->level;
    int rlvl = right->level;
    // left child nodes have level one-less-than node
    if(left != tree->nil && llvl != nlvl - 1){
        ORIG(E_VALUE, "invalid node leveling (leftwards)");
    }
    // right child nodes have level equal or one-less-than node
    if(right != tree->nil && (rlvl > nlvl || rlvl < nlvl - 1)){
        ORIG(E_VALUE, "invalid node leveling (rightwards)");
    }
    // anything with a null child is the bottom pseudonode (level of 1)
    if((left == tree->nil || right == tree->nil) && nlvl != 1){
        ORIG(E_VALUE, "invalid node leveling (bottom)");
    }

    // links are well-ordered
    int nval = ((binsrch_int_t*)node->data)->value;
    if(left != tree->nil){
        int lval = ((binsrch_int_t*)left->data)->value;
        if(lval > nval){
            LOG_ERROR("lmagic %x nmagic %x\n",
                      FU(((binsrch_int_t*)left->data)->magic),
                      FU(((binsrch_int_t*)node->data)->magic));
            LOG_ERROR("lval %x nval %x\n", FI(lval), FI(nval));
            ORIG(E_VALUE, "invalid node ordering (leftwards)");
        }
    }
    if(right != tree->nil){
        int rval = ((binsrch_int_t*)right->data)->value;
        if(nval > rval){
            ORIG(E_VALUE, "invalid node ordering (rightwards)");
        }
    }

    // apply test recursively
    PROP( recursive_anode_assertions(tree, left) );
    PROP( recursive_anode_assertions(tree, right) );

    return E_OK;
}

static derr_t atree_assertions(const jsw_atree_t *tree){
    PROP( recursive_anode_assertions(tree, tree->root) );
    return E_OK;
}

static derr_t do_test_atree(binsrch_int_t *ints, size_t num_ints){
    derr_t error;
    // the andersson tree
    jsw_atree_t tree;
    PROP( jsw_ainit(&tree, cmp_binsrch_ints, release_binsrch_int) );
    // insert each element
    for(size_t i = 0; i < num_ints; i++){
        jsw_ainsert(&tree, &ints[i].anode);
        // make sure all the properties of the atree are met
        PROP_GO( atree_assertions(&tree), cu_tree);
    }
    // print tree
    //print_atree(tree.root, 0);
    // delete each element
    for(size_t i = 0; i < num_ints; i++){
        jsw_aerase(&tree, &ints[i]);
        // make sure all the properties of the atree are met
        PROP_GO( atree_assertions(&tree), cu_tree);
    }
    // tree should be empty
    if(tree.size != 0){
        ORIG_GO(E_VALUE, "tree should be empty", cu_tree);
    }

cu_tree:
    // free the tree
    jsw_adelete(&tree);
    return error;
}

static derr_t test_atree(void){
    // some values to be inserted and stuff
    binsrch_int_t ints[NUM_INTS];
    // prep all the binsrch_int_t's self-pointers
    for(size_t i = 0; i < NUM_INTS; i++){
        ints[i].anode.data = &ints[i];
        ints[i].magic = (unsigned char)0xAA;
    }
    // ascending values
    LOG_ERROR("testing with ascending values\n");
    {
        for(int i = 0; i < NUM_INTS; i++){
            ints[i].value = i;
        }
        PROP( do_test_atree(ints, NUM_INTS) );
    }
    // descending values
    LOG_ERROR("testing with descending values\n");
    {
        for(int i = 0; i < NUM_INTS; i++){
            ints[i].value = NUM_INTS - i;
        }
        PROP( do_test_atree(ints, NUM_INTS) );
    }
    // identical values
    LOG_ERROR("testing with identical values\n");
    {
        for(int i = 0; i < NUM_INTS; i++){
            ints[i].value = 0;
        }
        PROP( do_test_atree(ints, NUM_INTS) );
    }
    // random values
    LOG_ERROR("testing with random values\n");
    {
        for(int i = 0; i < NUM_INTS; i++){
            ints[i].value = rand();
        }
        PROP( do_test_atree(ints, NUM_INTS) );
    }
    return E_OK;
}

int main(int argc, char** argv){
    derr_t error;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_DEBUG);

    // random number seed
    unsigned int seed = (unsigned int)(time(NULL) % UINT_MAX);
    srand(seed);
    LOG_ERROR("using seed: %x\n", FU(seed));

    PROP_GO( test_atree(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    LOG_ERROR("FAIL\n");
    return 1;
}
