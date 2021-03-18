#include "libcrypto.h"

/* compare two strings in constant time, where the timing does not leak length
   information about the secret string */
bool dstr_eq_consttime(const dstr_t *provided, const dstr_t *secret){
    // check lengths first
    bool valid = provided->len == secret->len;

    // when lengths differ, compare provided against itself
    const dstr_t *other = valid ? secret : provided;

    for(size_t i = 0; i < provided->len; i++){
        valid &= provided->data[i] == other->data[i];
    }

    return valid;
}
