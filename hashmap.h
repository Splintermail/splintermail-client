#ifndef HASHMAP_H
#define HASHMAP_H

#include "common.h"

typedef union {
    const dstr_t *dstr;
    unsigned int uint;
} hash_key_t;

typedef struct hash_elem_t {
    hash_key_t key;
    void *data;
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
    hashmap_t *hashmap;
    size_t bucket_idx;
    hash_elem_t *current;
} hashmap_iter_t;

derr_t hashmap_init(hashmap_t *h);
void hashmap_free(hashmap_t *h);

/* putters and setters do not return E_NOMEM error; if table needs to be
   expanded but we are out of memory, "retry-expand-on-next-insert" is fine. */

// putters, raise error if key already in hashmap
derr_t hashmap_puts(hashmap_t *h, const dstr_t *key, hash_elem_t *elem);
derr_t hashmap_putu(hashmap_t *h, unsigned int key, hash_elem_t *elem);

// setters, (NULL **old OR NULL *found) means "*old needs no cleanup"
void hashmap_sets(hashmap_t *h, const dstr_t *key, hash_elem_t *elem,
                    void **old, bool *found);
void hashmap_setu(hashmap_t *h, unsigned int key, hash_elem_t *elem,
                    void **old, bool *found);

// getters, if *found is NULL, that means "raise error if key not found"
// otherwise, throws no errors
// **data is allowed to be NULL, regardless
derr_t hashmap_gets(hashmap_t *h, const dstr_t *key, void **data, bool *found);
derr_t hashmap_getu(hashmap_t *h, unsigned int key, void **data, bool *found);

// deleters are idempotent. **old and *found can be NULL without side effects.
void hashmap_dels(hashmap_t *h, const dstr_t *key, void **old, bool *found);
void hashmap_delu(hashmap_t *h, unsigned int key, void **old, bool *found);

// iterators
void *hashmap_first(hashmap_t *h, hashmap_iter_t *i);
void *hashmap_next(hashmap_iter_t *i);
// example:
// for(void *data = hashmap_first(h, i); data = hashmap_next(i); data != NULL)

#endif // HASHMAP_H
