#include <stdint.h>

#include "libcrypto/libcrypto.h"

#include "petname.h"

#include "animals.h"
#include "adjectives.h"

derr_t petname_email(dstr_t *out){
    derr_t e = E_OK;

    size_t nanimals = sizeof(animals)/sizeof(*animals);
    size_t nadjectives = sizeof(adjectives)/sizeof(*adjectives);
    size_t nnumbers = 1000;

    uint64_t anm;
    uint64_t adj;
    uint64_t n;
    PROP(&e, random_uint64_under(nanimals, &anm) );
    PROP(&e, random_uint64_under(nadjectives, &adj) );
    PROP(&e, random_uint64_under(nnumbers, &n) );

    PROP(&e,
        FMT(out, "%x%x%x@splintermail.com", FS(adjectives[adj]), FS(animals[anm]), FU(n))
    );

    return e;
}
