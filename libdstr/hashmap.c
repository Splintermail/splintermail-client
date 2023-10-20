#include <stdlib.h>

#include "libdstr.h"

#define INIT_NUM_BUCKETS 32 /* power of 2 */
#define MAX_LOAD_FACTOR 1


/* code below came from Elliot Bach, though he cites other people.  It has been
   slightly modified. */
/* https://web.archive.org/web/20160329102146/http://elliottback.com/wp/hashmap-implementation-in-c/ */

static unsigned int hash_uint(unsigned int ukey){
    /* Robert Jenkins' 32 bit Mix Function */
    ukey += (ukey << 12);
    ukey ^= (ukey >> 22);
    ukey += (ukey << 4);
    ukey ^= (ukey >> 9);
    ukey += (ukey << 10);
    ukey ^= (ukey >> 2);
    ukey += (ukey << 7);
    ukey ^= (ukey >> 12);

    /* Knuth's Multiplicative Method */
    ukey = (unsigned int)(((ukey >> 3) * 2654435761) & 0xFFFFFFFF);

    return ukey;
}

/* end of code from Elliot Bach */

static unsigned int hash_str(const dstr_t *skey){
    // the xor form of the djb2 hash, found at www.cse.yorku.ca/~oz/hash.html
    unsigned char *udata = (unsigned char*)skey->data;
    unsigned int x = 0;
    for(size_t i = 0; i < skey->len; i++){
        x = (x * 33) ^ udata[i];
    }
    return x;
}

derr_t hashmap_init(hashmap_t *h){
    derr_t e = E_OK;
    size_t new_size = 32;
    // always allocate in powers of 2
    while( new_size < INIT_NUM_BUCKETS * sizeof(*(h->buckets)) ){
        new_size *= 2;
    }
    *h = (hashmap_t){
        .buckets = malloc(new_size),
        .num_buckets = INIT_NUM_BUCKETS,
        // the mask is an all-ones binary number, one less than num_buckets
        .mask = (unsigned int)(INIT_NUM_BUCKETS - 1),
    };
    if(!h->buckets){
        ORIG(&e, E_NOMEM, "unable to allocate hashmap");
    }
    // all buckets start pointing to our sentinel
    for(size_t i = 0; i < INIT_NUM_BUCKETS; i++){
        h->buckets[i] = &h->sentinel;
    }
    return e;
}

void hashmap_free(hashmap_t *h){
    if(h->buckets) free(h->buckets);
    *h = (hashmap_t){0};
}

static void check_rehash(hashmap_t *h){
    // check if we meet conditions for rehash
    if(h->num_elems / h->num_buckets < MAX_LOAD_FACTOR) return;
    // double the number of buckets
    size_t new_num_buckets = h->num_buckets * 2;
    // double the size of the array
    size_t new_size = 32;
    while( new_size < new_num_buckets * sizeof(*(h->buckets)) ){
        new_size *= 2;
    }
    // reallocate buckets
    hash_elem_t **new_buckets = realloc(h->buckets, new_size);

    // if realloc failed, just try again next time around
    if(!new_buckets) return;

    // grab some old values and update to new ones
    size_t half_buckets = h->num_buckets;
    h->num_buckets = new_num_buckets;
    unsigned int old_mask = h->mask;
    h->mask = (unsigned int)((h->num_buckets - 1) & 0xFFFFFFFF);
    h->buckets = new_buckets;

    /* because we always have power-of-two num_buckets, every bucket in the old
       **buckets will now be split between two buckets: (that index) and
       (that index + half_buckets).  Therefore we can safely walk down the
       first half of **buckets and consider only one bucket at a time, without
       worrying about affecting any of the other old buckets before they are
       considered. */
    // get a bitmask that is just the uppermost bit
    unsigned int upper_bit = h->mask ^ old_mask;
    for(size_t i = 0; i < half_buckets; i++){
        // grab the old linked list
        hash_elem_t *old = h->buckets[i];
        // sort the old list into hi and lo
        hash_elem_t *hi = &h->sentinel;
        hash_elem_t *lo = &h->sentinel;
        while(old != &h->sentinel){
            hash_elem_t *next = old->next;
            if(old->hash & upper_bit){
                old->next = hi;
                hi = old;
            }else{
                old->next = lo;
                lo = old;
            }
            old = next;
        }
        h->buckets[i] = lo;
        h->buckets[i + half_buckets] = hi;
    }
}

// returns the hash value
static unsigned int hashmap_elem_init(hash_elem_t *elem, const dstr_t *skey,
        unsigned int ukey, bool key_is_str){
    if(elem->next != NULL){
        LOG_FATAL("hashmap re-insertion detected\n");
    }
    elem->key_is_str = key_is_str;
    if(key_is_str){
        elem->key.dstr = skey;
        elem->hash = hash_str(skey);
    }else{
        elem->key.uint = ukey;
        elem->hash = hash_uint(ukey);
    }
    return elem->hash;
}

static bool hash_elem_match(hash_elem_t *a, hash_elem_t *b){
    if(a->key_is_str){
        // string-type key
        return b->key_is_str && (dstr_eq(*a->key.dstr, *b->key.dstr));
    }
    // numeric key
    return !b->key_is_str && (a->key.uint == b->key.uint);
}

// This will raise E_PARAM if and only if (**old == NULL && key is a duplicate)
static derr_t hashmap_set(hashmap_t *h, const dstr_t *skey,
        unsigned int ukey, bool key_is_str, hash_elem_t *elem,
        hash_elem_t **old){
    derr_t e = E_OK;
    hashmap_elem_init(elem, skey, ukey, key_is_str);
    // get bucket index
    size_t idx = elem->hash & h->mask;
    // get end of bucket, checking for identical key as we go
    hash_elem_t **eptr;
    if(old) *old = NULL;
    // check if we need to replace an element with this value
    for(eptr = &h->buckets[idx]; *eptr != &h->sentinel; eptr = &(*eptr)->next){
        if(!hash_elem_match(elem, *eptr)) continue;
        if(old == NULL){
            ORIG(&e, E_PARAM, "refusing to insert duplicate value");
        }
        *old = *eptr;
        elem->next = (*old)->next;
        (*old)->next = NULL;
        *eptr = elem;
        return e;
    }

    // if no replacement was made, append value to end of linked list
    elem->next = *eptr;
    *eptr = elem;
    h->num_elems++;
    check_rehash(h);

    return e;
}

hash_elem_t *hashmap_sets(hashmap_t *h, const dstr_t *key, hash_elem_t *elem){
    hash_elem_t *old;
    DROP_CMD( hashmap_set(h, key, 0, true, elem, &old) );
    return old;
}

hash_elem_t *hashmap_setu(hashmap_t *h, unsigned int key, hash_elem_t *elem){
    hash_elem_t *old;
    DROP_CMD( hashmap_set(h, NULL, key, false, elem, &old) );
    return old;
}

derr_t hashmap_sets_unique(hashmap_t *h, const dstr_t *key, hash_elem_t *elem){
    derr_t e = E_OK;
    PROP(&e, hashmap_set(h, key, 0, true, elem, NULL) );
    return e;
}
derr_t hashmap_setu_unique(hashmap_t *h, unsigned int key, hash_elem_t *elem){
    derr_t e = E_OK;
    PROP(&e, hashmap_set(h, NULL, key, false, elem, NULL) );
    return e;
}

static hash_elem_t *hashmap_get(hashmap_t *h, const dstr_t *skey,
        unsigned int ukey, bool key_is_str){
    // build dummy element
    hash_elem_t elem = {0};
    hashmap_elem_init(&elem, skey, ukey, key_is_str);
    // get bucket index
    size_t idx = elem.hash & h->mask;
    // get end of bucket, checking for match
    hash_elem_t **eptr;
    for(eptr = &h->buckets[idx]; *eptr != &h->sentinel; eptr = &(*eptr)->next){
        if(hash_elem_match(&elem, *eptr)){
            // found match
            return *eptr;
        }
    }
    // no match found
    return NULL;
}

hash_elem_t *hashmap_gets(hashmap_t *h, const dstr_t *key){
    return hashmap_get(h, key, 0, true);
}

hash_elem_t *hashmap_getu(hashmap_t *h, unsigned int key){
    return hashmap_get(h, NULL, key, false);
}

static hash_elem_t *hashmap_del(hashmap_t *h, const dstr_t *skey,
        unsigned int ukey, bool key_is_str){
    // build dummy element
    hash_elem_t elem = {0};
    hashmap_elem_init(&elem, skey, ukey, key_is_str);
    // get bucket index
    size_t idx = elem.hash & h->mask;
    // get end of bucket, checking for match
    hash_elem_t **eptr;
    for(eptr = &h->buckets[idx]; *eptr != &h->sentinel; eptr = &(*eptr)->next){
        if(hash_elem_match(&elem, *eptr)){
            // found match
            hash_elem_t *deleted = *eptr;
            *eptr = deleted->next;
            deleted->next = NULL;
            // decrement element count
            if(!h->num_elems--){
                LOG_FATAL("num_elems underflow in hashmap_del\n");
            }
            return deleted;
        }
    }
    return NULL;
}

hash_elem_t *hashmap_dels(hashmap_t *h, const dstr_t *key){
    return hashmap_del(h, key, 0, true);
}

hash_elem_t *hashmap_delu(hashmap_t *h, unsigned int key){
    return hashmap_del(h, NULL, key, false);
}

void hash_elem_remove(hash_elem_t *elem){
    // detect noop
    if(!elem->next) return;
    // find hashmap
    hash_elem_t *sentinel = elem->next;
    while(sentinel->next) sentinel = sentinel->next;
    hashmap_t *h = CONTAINER_OF(sentinel, hashmap_t, sentinel);
    if(!h->num_elems--){
        LOG_FATAL("num_elems underflow in hash_elem_remove\n");
    }
    size_t idx = elem->hash & h->mask;
    // find what points to our elem
    hash_elem_t **fixme = &h->buckets[idx];
    for(; *fixme != &h->sentinel; fixme = &(*fixme)->next){
        if(*fixme != elem) continue;
        *fixme = elem->next;
        elem->next = NULL;
        return;
    }
    LOG_FATAL("hash_elem_remove did not find elem\n");
}

static size_t next_nonempty_bucket(hashmap_t *h, size_t i){
    hash_elem_t *sentinel = &h->sentinel;
    hash_elem_t **buckets = h->buckets;
    size_t nbuckets = h->num_buckets;
    for( ; i < nbuckets; i++){
        if(buckets[i] == sentinel) continue;
        return i;
    }
    return SIZE_MAX;
}

hash_elem_t *hashmap_iter(hashmap_trav_t *trav, hashmap_t *h){
    *trav = (hashmap_trav_t){ .hashmap = h, .current = &h->sentinel };
    return hashmap_next(trav);
}

hash_elem_t *hashmap_next(hashmap_trav_t *trav){
    hash_elem_t *sentinel = &trav->hashmap->sentinel;
    // take the next element of the list
    if(trav->current != sentinel){
        trav->current = trav->next;
        if(trav->current != sentinel){
            trav->next = trav->current->next;
            return trav->current;
        }
        // move to the next bucket
        trav->bucket_idx++;
    }
    // find a non-empty bucket
    trav->bucket_idx = next_nonempty_bucket(trav->hashmap, trav->bucket_idx);
    if(trav->bucket_idx == SIZE_MAX) return NULL;
    trav->current = trav->hashmap->buckets[trav->bucket_idx];
    trav->next = trav->current->next;
    return trav->current;
}

hash_elem_t *hashmap_pop_iter(hashmap_trav_t *trav, hashmap_t *h){
    *trav = (hashmap_trav_t){ .hashmap = h };
    return hashmap_pop_next(trav);
}

hash_elem_t *hashmap_pop_next(hashmap_trav_t *trav){
    trav->bucket_idx = next_nonempty_bucket(trav->hashmap, trav->bucket_idx);
    if(trav->bucket_idx == SIZE_MAX) return NULL;
    // remove the element from the bucket
    hash_elem_t *elem = trav->hashmap->buckets[trav->bucket_idx];
    trav->hashmap->buckets[trav->bucket_idx] = elem->next;
    elem->next = NULL;
    // decrement element counter
    if(!trav->hashmap->num_elems--){
        LOG_FATAL("num_elems underflow in hashmap_pop_next\n");
    }
    return elem;
}

derr_t map_str_str_new(
    const dstr_t key, const dstr_t val, map_str_str_t **out
){
    derr_t e = E_OK;
    *out = NULL;

    map_str_str_t *mss = DMALLOC_STRUCT_PTR(&e, mss);
    CHECK(&e);

    PROP_GO(&e, dstr_copy(&key, &mss->key), fail);
    PROP_GO(&e, dstr_copy(&val, &mss->val), fail);

    *out = mss;

    return e;

fail:
    map_str_str_free(&mss);
    return e;
}

void map_str_str_free(map_str_str_t **old){
    map_str_str_t *mss = *old;
    if(!mss) return;
    dstr_free(&mss->key);
    dstr_free(&mss->val);
    free(mss);
    *old = NULL;
}
