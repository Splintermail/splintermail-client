typedef union {
    const dstr_t *dstr;
    unsigned int uint;
} hash_key_t;

typedef struct hash_elem_t {
    hash_key_t key;
    struct hash_elem_t *next;
    unsigned int hash;
    bool key_is_str;
} hash_elem_t;

typedef struct {
    size_t num_buckets; // always a power of 2
    size_t num_elems; // for tracking avg load
    hash_elem_t **buckets;
    unsigned int mask; // use bitmask as our mod function
} hashmap_t;

typedef struct {
    // internal state only
    hashmap_t *hashmap;
    size_t bucket_idx;
    // for application to read (read-only):
    hash_elem_t *current;
} hashmap_iter_t;

derr_t hashmap_init(hashmap_t *h);
/* throws: E_NOMEM */
void hashmap_free(hashmap_t *h);

/* putters and setters do not return E_NOMEM error; if table needs to be
   expanded but we are out of memory, "retry-expand-on-next-insert" is fine. */

// setters return the replaced element, if any
/* WATCH OUT! make sure that the dstr_t *key points to somewhere permanent */
hash_elem_t *hashmap_sets(hashmap_t *h, const dstr_t *key, hash_elem_t *elem);
hash_elem_t *hashmap_setu(hashmap_t *h, unsigned int key, hash_elem_t *elem);

// setters which throw errors instead of replacing existing elements.
derr_t hashmap_sets_unique(hashmap_t *h, const dstr_t *key, hash_elem_t *elem);
derr_t hashmap_setu_unique(hashmap_t *h, unsigned int key, hash_elem_t *elem);
/* throws: E_PARAM */

// getters return the found element, if any
hash_elem_t *hashmap_gets(hashmap_t *h, const dstr_t *key);
hash_elem_t *hashmap_getu(hashmap_t *h, unsigned int key);

// deleters return the deleted element, if any
hash_elem_t *hashmap_dels(hashmap_t *h, const dstr_t *key);
hash_elem_t *hashmap_delu(hashmap_t *h, unsigned int key);

// delete an element directly (elem must be in h)
void hashmap_del_elem(hashmap_t *h, hash_elem_t *elem);

// iterators
hashmap_iter_t hashmap_first(hashmap_t *h);
void hashmap_next(hashmap_iter_t *i);
// example:
//    for(hashmap_iter_t i = hashmap_first(&h); i.current; hashmap_next(&i)){
//        do_something(i.current);
//    }

// "pop"ing iterators, also remove objects from the hashmap
hashmap_iter_t hashmap_pop_first(hashmap_t *h);
void hashmap_pop_next(hashmap_iter_t *i);
