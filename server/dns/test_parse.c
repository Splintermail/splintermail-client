#include "parse.c"

static derr_t test_labels_iter(void){
    derr_t e = E_OK;

    #define ASSERT(code) \
        if(!(code)) ORIG(&e, E_VALUE, "assertion failed: " #code)

    char buf[] =
        "\x01" "a" "\x02" "bb" "\x03" "ccc" "\x00" // a.bb.ccc
        "\x01" "z" "\xC2" // z.bb.ccc
    ;
    size_t len = sizeof(buf)-1;

    // validate with parse_name
    size_t used = 0;
    used = parse_name(buf, len, used);
    ASSERT(used == 10);

    used = parse_name(buf, len, used);
    ASSERT(used == 13);

    // iter with labels_iter
    used = 0;
    labels_t labels;
    lstr_t *lstr = labels_iter(&labels, buf, used);
    ASSERT(lstr);
    ASSERT(strncmp(lstr->str, "a", lstr->len) == 0);
    lstr = labels_next(&labels);
    ASSERT(lstr);
    ASSERT(strncmp(lstr->str, "bb", lstr->len) == 0);
    lstr = labels_next(&labels);
    ASSERT(lstr);
    ASSERT(strncmp(lstr->str, "ccc", lstr->len) == 0);
    lstr = labels_next(&labels);
    ASSERT(!lstr);

    used += labels.used;
    lstr = labels_iter(&labels, buf, used);
    ASSERT(lstr);
    ASSERT(strncmp(lstr->str, "z", lstr->len) == 0);
    lstr = labels_next(&labels);
    ASSERT(lstr);
    ASSERT(strncmp(lstr->str, "bb", lstr->len) == 0);
    lstr = labels_next(&labels);
    ASSERT(lstr);
    ASSERT(strncmp(lstr->str, "ccc", lstr->len) == 0);
    lstr = labels_next(&labels);
    ASSERT(!lstr);

    ASSERT(labels.used == 13);

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

    RUN(test_labels_iter());

    printf("%s\n", retval ? "FAIL" : "PASS");

    return retval;
}
