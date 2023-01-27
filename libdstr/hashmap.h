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
    hash_elem_t sentinel; // end of every bucket points here
} hashmap_t;
DEF_CONTAINER_OF(hashmap_t, sentinel, hash_elem_t)

typedef struct {
    hashmap_t *hashmap;
    size_t bucket_idx;
    hash_elem_t *current;
    // track the next value so caller can hash_elem_remove current safely
    hash_elem_t *next;
} hashmap_trav_t;

derr_t hashmap_init(hashmap_t *h);
/* throws: E_NOMEM */
void hashmap_free(hashmap_t *h);

/* putters and setters do not return E_NOMEM error; if table needs to be
   expanded but we are out of memory, "retry-expand-on-next-insert" is fine. */

// setters return the replaced element, if any
// these abort if elem is already in a dictionary
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

// delete an element directly (idempotent)
void hash_elem_remove(hash_elem_t *elem);

/* iterator; it is safe to call hash_elem_remove() on the returned element if
   you wish, but it is not safe to insert or remove anything else */
hash_elem_t *hashmap_iter(hashmap_trav_t *trav, hashmap_t *h);
hash_elem_t *hashmap_next(hashmap_trav_t *trav);

// "pop"ing iterator, also remove objects from the hashmap
hash_elem_t *hashmap_pop_iter(hashmap_trav_t *trav, hashmap_t *h);
hash_elem_t *hashmap_pop_next(hashmap_trav_t *trav);

// hashmap-friendly wrapper types
typedef struct {
    dstr_t key;
    dstr_t val;
    hash_elem_t elem;
} map_str_str_t;
DEF_CONTAINER_OF(map_str_str_t, elem, hash_elem_t)
DEF_STEAL_PTR(map_str_str_t)

derr_t map_str_str_new(
    const dstr_t key, const dstr_t val, map_str_str_t **out
);
void map_str_str_free(map_str_str_t **old);
