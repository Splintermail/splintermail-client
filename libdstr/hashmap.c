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

/* code below here came from Pete Warden, though he cites other people.  It has
   been slightly modified. */
/* https://petewarden.typepad.com/searchbrowser/2010/01/c-hashmap.html */

/* The implementation here was originally done by Gary S. Brown.  I have
   borrowed the tables directly, and made some minor changes to the
   crc32-function (including changing the interface). //ylo */

  /* ============================================================= */
  /*  COPYRIGHT (C) 1986 Gary S. Brown.  You may use this program, or       */
  /*  code or tables extracted from it, as desired without restriction.     */
  /*                                                                        */
  /*  First, the polynomial itself and its table of feedback terms.  The    */
  /*  polynomial is                                                         */
  /*  X^32+X^26+X^23+X^22+X^16+X^12+X^11+X^10+X^8+X^7+X^5+X^4+X^2+X^1+X^0   */
  /*                                                                        */
  /*  Note that we take it "backwards" and put the highest-order term in    */
  /*  the lowest-order bit.  The X^32 term is "implied"; the LSB is the     */
  /*  X^31 term, etc.  The X^0 term (usually shown as "+1") results in      */
  /*  the MSB being 1.                                                      */
  /*                                                                        */
  /*  Note that the usual hardware shift register implementation, which     */
  /*  is what we're using (we're merely optimizing it by doing eight-bit    */
  /*  chunks at a time) shifts bits into the lowest-order term.  In our     */
  /*  implementation, that means shifting towards the right.  Why do we     */
  /*  do it this way?  Because the calculated CRC must be transmitted in    */
  /*  order from highest-order term to lowest-order term.  UARTs transmit   */
  /*  characters in order from LSB to MSB.  By storing the CRC this way,    */
  /*  we hand it to the UART in the order low-byte to high-byte; the UART   */
  /*  sends each low-bit to hight-bit; and the result is transmission bit   */
  /*  by bit from highest- to lowest-order term without requiring any bit   */
  /*  shuffling on our part.  Reception works similarly.                    */
  /*                                                                        */
  /*  The feedback terms table consists of 256, 32-bit entries.  Notes:     */
  /*                                                                        */
  /*      The table can be generated at runtime if desired; code to do so   */
  /*      is shown later.  It might not be obvious, but the feedback        */
  /*      terms simply represent the results of eight shift/xor opera-      */
  /*      tions for all combinations of data and CRC register values.       */
  /*                                                                        */
  /*      The values must be right-shifted by eight bits by the "updcrc"    */
  /*      logic; the shift must be unsigned (bring in zeroes).  On some     */
  /*      hardware you could probably optimize the shift in assembler by    */
  /*      using byte-swap instructions.                                     */
  /*      polynomial $edb88320                                              */
  /*                                                                        */
  /*  --------------------------------------------------------------------  */

static unsigned int crc32_tab[] = {
      0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419,
      0x706af48f, 0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4,
      0xe0d5e91e, 0x97d2d988, 0x09b64c2b, 0x7eb17cbd, 0xe7b82d07,
      0x90bf1d91, 0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de,
      0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7, 0x136c9856,
      0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
      0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4,
      0xa2677172, 0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,
      0x35b5a8fa, 0x42b2986c, 0xdbbbc9d6, 0xacbcf940, 0x32d86ce3,
      0x45df5c75, 0xdcd60dcf, 0xabd13d59, 0x26d930ac, 0x51de003a,
      0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423, 0xcfba9599,
      0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
      0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190,
      0x01db7106, 0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f,
      0x9fbfe4a5, 0xe8b8d433, 0x7807c9a2, 0x0f00f934, 0x9609a88e,
      0xe10e9818, 0x7f6a0dbb, 0x086d3d2d, 0x91646c97, 0xe6635c01,
      0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e, 0x6c0695ed,
      0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
      0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3,
      0xfbd44c65, 0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2,
      0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a,
      0x346ed9fc, 0xad678846, 0xda60b8d0, 0x44042d73, 0x33031de5,
      0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa, 0xbe0b1010,
      0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
      0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17,
      0x2eb40d81, 0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6,
      0x03b6e20c, 0x74b1d29a, 0xead54739, 0x9dd277af, 0x04db2615,
      0x73dc1683, 0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8,
      0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1, 0xf00f9344,
      0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
      0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a,
      0x67dd4acc, 0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5,
      0xd6d6a3e8, 0xa1d1937e, 0x38d8c2c4, 0x4fdff252, 0xd1bb67f1,
      0xa6bc5767, 0x3fb506dd, 0x48b2364b, 0xd80d2bda, 0xaf0a1b4c,
      0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55, 0x316e8eef,
      0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
      0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe,
      0xb2bd0b28, 0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31,
      0x2cd99e8b, 0x5bdeae1d, 0x9b64c2b0, 0xec63f226, 0x756aa39c,
      0x026d930a, 0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713,
      0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38, 0x92d28e9b,
      0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
      0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1,
      0x18b74777, 0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c,
      0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45, 0xa00ae278,
      0xd70dd2ee, 0x4e048354, 0x3903b3c2, 0xa7672661, 0xd06016f7,
      0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc, 0x40df0b66,
      0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
      0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605,
      0xcdd70693, 0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8,
      0x5d681b02, 0x2a6f2b94, 0xb40bbe37, 0xc30c8ea1, 0x5a05df1b,
      0x2d02ef8d
   };

/* Return a 32-bit CRC of the contents of the buffer. */

static unsigned int crc32(const dstr_t *skey){
    // treat chars as unsigned char
    unsigned char * s = (unsigned char*)skey->data;
    unsigned int crc32val = 0;
    for(size_t i = 0;  i < skey->len;  i ++){
        crc32val = crc32_tab[(crc32val ^ s[i]) & 0xff] ^ (crc32val >> 8);
    }
    return crc32val;
}

/* end of code from Pete Warden */

static unsigned int hash_str(const dstr_t *skey){
    unsigned int crc = crc32(skey);
    return hash_uint(crc);
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
