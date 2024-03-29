#include <stdlib.h>
#include <time.h>
#include <limits.h>

#include <libdstr/libdstr.h>
#include <libimap/libimap.h>

#include "test_utils.h"

#define EXP_VS_GOT(exp, got) do { \
    if(exp != got){ \
        TRACE(&e, "expected: %x\n" \
                 "but got:  %x\n", FU(exp), FU(got)); \
        ORIG_GO(&e, E_VALUE, "test fail", cleanup); \
    } \
} while(0)

static derr_t test_seq_set_iter(void){
    derr_t e = E_OK;

    ie_seq_set_trav_t trav;
    ie_seq_set_t *seq_set = NULL;
    unsigned int min = 5;
    unsigned int max = 100;
    unsigned int x;

    // get 0 for NULL set
    x = ie_seq_set_iter(&trav, seq_set, min, max);
    EXP_VS_GOT(0, x);

    // safe to call next
    x = ie_seq_set_next(&trav);
    EXP_VS_GOT(0, x);

    // in order, with a max
    seq_set = ie_seq_set_new(&e, 0, 10);
    // out of order, with a max
    seq_set = ie_seq_set_append(&e, seq_set, ie_seq_set_new(&e, 20, 0));
    // in order, no max
    seq_set = ie_seq_set_append(&e, seq_set, ie_seq_set_new(&e, 55, 66));
    // out of order, no max
    seq_set = ie_seq_set_append(&e, seq_set, ie_seq_set_new(&e, 11, 10));
    // overlaps with minimum
    seq_set = ie_seq_set_append(&e, seq_set, ie_seq_set_new(&e, 1, 10));
    // overlaps with maximum
    seq_set = ie_seq_set_append(&e, seq_set, ie_seq_set_new(&e, 50, 1000));
    // overlaps with both ranges
    seq_set = ie_seq_set_append(&e, seq_set, ie_seq_set_new(&e, 1, 1000));
    // single value
    seq_set = ie_seq_set_append(&e, seq_set, ie_seq_set_new(&e, 31,31));
    CHECK(&e);

    // very first call
    x = ie_seq_set_iter(&trav, seq_set, min, max);
    EXP_VS_GOT(10, x);
    for(unsigned int i = 11; i <= 100; i++){
        x = ie_seq_set_next(&trav);
        EXP_VS_GOT(i, x);
    }
    for(unsigned int i = 20; i <= 100; i++){
        x = ie_seq_set_next(&trav);
        EXP_VS_GOT(i, x);
    }
    for(unsigned int i = 55; i <= 66; i++){
        x = ie_seq_set_next(&trav);
        EXP_VS_GOT(i, x);
    }
    for(unsigned int i = 10; i <= 11; i++){
        x = ie_seq_set_next(&trav);
        EXP_VS_GOT(i, x);
    }
    for(unsigned int i = 5; i <= 10; i++){
        x = ie_seq_set_next(&trav);
        EXP_VS_GOT(i, x);
    }
    for(unsigned int i = 50; i <= 100; i++){
        x = ie_seq_set_next(&trav);
        EXP_VS_GOT(i, x);
    }
    for(unsigned int i = 5; i <= 100; i++){
        x = ie_seq_set_next(&trav);
        EXP_VS_GOT(i, x);
    }
    x = ie_seq_set_next(&trav);
    EXP_VS_GOT(31, x);

    // no more values
    x = ie_seq_set_next(&trav);
    EXP_VS_GOT(0, x);

    // safe to call again
    x = ie_seq_set_next(&trav);
    EXP_VS_GOT(0, x);


cleanup:
    ie_seq_set_free(seq_set);

    return e;
}

static derr_t test_seq_set_iter_nobounds(void){
    derr_t e = E_OK;

    ie_seq_set_trav_t trav;
    ie_seq_set_t *seq_set = NULL;
    unsigned int min = 0;
    unsigned int max = 0;
    unsigned int x;

    // get 0 for NULL set
    x = ie_seq_set_iter(&trav, seq_set, min, max);
    EXP_VS_GOT(0, x);

    // safe to call next
    x = ie_seq_set_next(&trav);
    EXP_VS_GOT(0, x);

    // in order, with a max
    seq_set = ie_seq_set_new(&e, 5, 10);
    // out of order, with a max
    seq_set = ie_seq_set_append(&e, seq_set, ie_seq_set_new(&e, 20, 30));
    CHECK(&e);

    // very first call
    x = ie_seq_set_iter(&trav, seq_set, min, max);
    EXP_VS_GOT(5, x);
    for(unsigned int i = 6; i <= 10; i++){
        x = ie_seq_set_next(&trav);
        EXP_VS_GOT(i, x);
    }
    for(unsigned int i = 20; i <= 30; i++){
        x = ie_seq_set_next(&trav);
        EXP_VS_GOT(i, x);
    }

    // no more values
    x = ie_seq_set_next(&trav);
    EXP_VS_GOT(0, x);

    // safe to call again
    x = ie_seq_set_next(&trav);
    EXP_VS_GOT(0, x);


cleanup:
    ie_seq_set_free(seq_set);

    return e;
}


int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_seq_set_iter(), test_fail);
    PROP_GO(&e, test_seq_set_iter_nobounds(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
