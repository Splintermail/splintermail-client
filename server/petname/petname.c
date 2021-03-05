#include <stdint.h>

#include "petname.h"

#include "animals.h"
#include "adjectives.h"

static derr_t number_under(uint64_t end, uint64_t *out){
    derr_t e = E_OK;

    // integer division gives us the right answer:
    // divisor = 11 / 10 = 1
    // divisor = 19 / 10 = 1
    // divisor = 20 / 10 = 2
    // divisor = 29 / 10 = 2
    // for the last case:
    // for rand=29; 29 / 2 = 14, retry
    // for rand=21; 21 / 2 = 10, retry
    // for rand=20; 20 / 2 = 10, retry
    // for rand=19; 19 / 2 = 9, good
    // for rand=18; 18 / 2 = 9, good
    // for rand=17; 17 / 2 = 8, good
    uint64_t divisor = UINT64_MAX / end;

    for(size_t limit = 0; limit < 100000; limit++){
        DSTR_VAR(buf, sizeof(*out));
        PROP(&e, random_bytes(&buf, buf.size) );

        uint64_t temp = *((uint64_t*)buf.data) / divisor;

        if(temp < end){
            *out = temp;
            return e;
        }
    }

    TRACE(&e, "failed to find random number less than %x\n", FU(end));
    ORIG(&e, E_INTERNAL, "10000 tries without success");
}

derr_t petname_email(dstr_t *out){
    derr_t e = E_OK;

    size_t nanimals = sizeof(animals)/sizeof(*animals);
    size_t nadjectives = sizeof(adjectives)/sizeof(*adjectives);
    size_t nnumbers = 1000;

    uint64_t anm;
    uint64_t adj;
    uint64_t n;
    PROP(&e, number_under(nanimals, &anm) );
    PROP(&e, number_under(nadjectives, &adj) );
    PROP(&e, number_under(nnumbers, &n) );

    PROP(&e,
        FMT(out, "%x%x%x@splintermail.com", FS(adjectives[adj]), FS(animals[anm]), FU(n))
    );

    return e;
}
