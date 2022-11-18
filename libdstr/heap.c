#include "libdstr/libdstr.h"

void heap_prep(heap_t *h, cmp_f cmp, heap_get_f get, bool want_max){
    *h = (heap_t){ .cmp = cmp, .get = get, .want_max = want_max };
}

void heap_put(heap_t *h, hnode_t *hnode){
    const void *nval = h->get(hnode);
    hnode_t *parent = &h->root_parent;
    hnode_t **ptr = &h->root_parent.children[0];
    while(*ptr){
        hnode_t *p = *ptr;
        const void *pval = h->get(p);
        // is hnode better than p?
        if((h->cmp(nval, pval) > 0) == h->want_max){
            // yes, replace p
            *ptr = hnode;
            hnode->parent = p->parent;
            hnode->children[0] = p->children[0];
            hnode->children[1] = p->children[1];
            hnode->balance = p->balance;
            if(hnode->children[0]) hnode->children[0]->parent = hnode;
            if(hnode->children[1]) hnode->children[1]->parent = hnode;
            // continue the algorithm but swap h and p
            hnode = p;
            p = *ptr;
            nval = pval;
        }
        // pick a dir to keep the tree balanced
        int dir = p->balance < 0;
        p->balance += 2*dir - 1;
        ptr = &p->children[dir];
        parent = p;
    }
    *hnode = (hnode_t){ .parent = parent };
    *ptr = hnode;
    return;
}

hnode_t *heap_peek(heap_t *h){
    return h->root_parent.children[0];
}

void hnode_remove(hnode_t *hnode){
    hnode_t *parent = hnode->parent;
    // is the node even in a tree?
    if(!parent) return;
    // find root_parent (and attached heap_t) rebalancing as we go up
    hnode_t *child = hnode;
    while(parent->parent){
        int dir = parent->children[1] == child;
        parent->balance -= 2*dir - 1;
        child = parent;
        parent = parent->parent;
    }
    heap_t *h = CONTAINER_OF(parent, heap_t, root_parent);

    /* setup our loop, which operates on parent, l and r:

            parent
               \
        (hnode being removed)
               / \
              l   r
    */

    parent = hnode->parent;
    int parent_dir = parent->children[1] == hnode;
    int old_balance = hnode->balance;
    hnode_t *children[2] = { hnode->children[0], hnode->children[1] };
    *hnode = (hnode_t){0};
    #define l (children[0])
    #define r (children[1])
    while(l && r){
        // both children are present, promote one
        int dir = (h->cmp(h->get(l), h->get(r)) < 0) == h->want_max;
        hnode_t *best = children[dir];
        hnode_t *worst = children[!dir];
        // best->children will be used as children next loop
        children[0] = best->children[0];
        children[1] = best->children[1];
        // shuffle pointers
        parent->children[parent_dir] = best;
        best->parent = parent;
        best->children[!dir] = worst;
        worst->parent = best;
        // rebalance
        int new_balance = old_balance - (2*dir - 1);
        old_balance = best->balance;
        best->balance = new_balance;
        // set up the next loop
        parent = best;
        parent_dir = dir;
    }
    if(l || r){
        // ended with a single child; just promote it directly
        int dir = !l;
        hnode_t *best = children[dir];
        parent->children[parent_dir] = best;
        best->parent = parent;
    }else{
        // ended at a leaf with no children
        parent->children[parent_dir] = NULL;
    }
    #undef l
    #undef r
}

hnode_t *heap_pop(heap_t *h){
    hnode_t *out = heap_peek(h);
    if(!out) return NULL;
    hnode_remove(out);
    return out;
}
