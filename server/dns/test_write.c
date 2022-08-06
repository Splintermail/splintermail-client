#include "write.c"

#define ASSERT(code) if(!(code)) ORIG(&e, E_VALUE, "assertion failed: " #code)

static derr_t test_predicted_lengths(void){
    derr_t e = E_OK;

    char buf[1024];
    size_t cap = sizeof(buf);

    lstr_t labels[] = {LSTR("hostmaster"), LSTR("splintermail"), LSTR("com")};
    size_t nlabels = sizeof(labels) / sizeof(*labels);
    ASSERT(namelen(labels, nlabels) == put_name(labels, nlabels, buf, 0));

    ASSERT(write_soa(buf, 0, 0) == write_soa(buf, cap, 0));

    ASSERT(write_edns(buf, 0, 0) == BARE_EDNS_SIZE);
    ASSERT(write_edns(buf, cap, 0) == BARE_EDNS_SIZE);

    return e;
}

int main(void){
    int retval = 0;

    logger_add_fileptr(LOG_LVL_INFO, stdout);

    #define RUN(code) do { \
        derr_t e = code; \
        if(is_error(e)){ \
            DUMP(e); \
            DROP_VAR(&e); \
            retval = 1; \
        } \
    } while(0)

    RUN(test_predicted_lengths());

    printf("%s\n", retval ? "FAIL" : "PASS");

    return retval;
}
