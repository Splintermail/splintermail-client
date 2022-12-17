#include <server/dns/libdns.h>

#include <stdlib.h>
#include <string.h>

derr_t rrl_init(rrl_t *rrl, size_t nbuckets){
    derr_t e = E_OK;

    *rrl = (rrl_t){
        .buckets = malloc(nbuckets),
        .nbuckets = nbuckets,
    };
    if(!rrl->buckets){
        ORIG(&e, E_NOMEM, "nomem");
    }
    memset(rrl->buckets, 0, nbuckets);

    return e;
}

void rrl_free(rrl_t *rrl){
    if(rrl->buckets) free(rrl->buckets);
    *rrl = (rrl_t){0};
}

// copied from hashmap.c, which has a proper attribution
// TODO: investigate other public domain hashing functions:
// (public domain) http://burtleburtle.net/bob/hash/doobs.html
// (LGPL) http://www.azillionmonkeys.com/qed/hash.html
static uint32_t hash_uint(uint32_t ukey){
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
    ukey = (uint32_t)(((ukey >> 3) * 2654435761) & 0xFFFFFFFF);

    return ukey;
}

static uint32_t hash_addr(const struct sockaddr *sa){
    int family = sa->sa_family;
    if(family == AF_INET){
        const struct sockaddr_in *sin = (const struct sockaddr_in*)sa;
        return hash_uint(sin->sin_addr.s_addr);
    }else if(family == AF_INET6){
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6*)sa;
        uint32_t *keys = (uint32_t*)&sin6->sin6_addr.s6_addr;
        return hash_uint(keys[0]) ^ hash_uint(keys[1]);
    }
    LOG_FATAL( "hash_addr with invalid address family %x\n", FI(family));
    return 0;
}

// returns true if this recipient is under the limit
bool rrl_check(rrl_t *rrl, const struct sockaddr *sa, xtime_t now){
    if(!rrl->nbuckets) return true;
    size_t hash = hash_addr(sa) % rrl->nbuckets;
    uint8_t window = (now / SECOND) & 0x1f;
    uint8_t bucket = rrl->buckets[hash];
    uint8_t stamp = (bucket >> 3) & 0x1f;
    if(stamp != window){
        // reset bucket
        rrl->buckets[hash] = (window << 3);
        return true;
    }
    uint8_t count = bucket & 0x7;
    if(count == 7){
        // limit reached
        return false;
    }
    // increase count
    rrl->buckets[hash] = (window << 3) | (count + 1);
    return true;
}
