struct hnode_t;
typedef struct hnode_t hnode_t;

struct hnode_t {
    hnode_t *parent;
    hnode_t *children[2];
    // balance<0: left-heavy, balance=0: balanced, balance>0: right-heavy
    int balance;
};

// reuse cmp_f from jsw_anode_t
// heap_get_f will fetch a key for the object that owns the node
typedef const void *(*heap_get_f) ( const hnode_t* );

typedef struct {
    hnode_t root_parent;
    cmp_f cmp;
    heap_get_f get;
    bool want_max;
} heap_t;
DEF_CONTAINER_OF(heap_t, root_parent, hnode_t)

void heap_prep(heap_t *h, cmp_f cmp, heap_get_f get, bool want_max);

// store a node in the heap
void heap_put(heap_t *h, hnode_t *n);

// return the min/max node in the heap but leave it in place
hnode_t *heap_peek(heap_t *h);

// idempotently remove a node from wherever it is in the heap
void hnode_remove(hnode_t *n);

// pop the min/max node in the heap (combines heap_peek() and hnode_remove())
hnode_t *heap_pop(heap_t *h);
