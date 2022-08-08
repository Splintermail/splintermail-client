#include "parse.c"

#include <stdlib.h>

static derr_t test_labels_iter(void){
    derr_t e = E_OK;

    #define ASSERT(code) \
        if(!(code)) ORIG(&e, E_VALUE, "assertion failed: " #code)

    char buf[] =
        "\x01" "a" "\x02" "bb" "\x03" "ccc" "\x00" // a.bb.ccc
        "\x01" "z" "\xC0\x02" // z.bb.ccc
    ;
    size_t len = sizeof(buf)-1;

    // validate with parse_name
    size_t used = 0;
    used = parse_name(buf, len, used);
    ASSERT(used == 10);

    used = parse_name(buf, len, used);
    ASSERT(used == 14);

    // iter with labels_iter
    used = 0;
    labels_t labels;
    lstr_t *lstr = labels_iter(&labels, buf, used);
    ASSERT(lstr);
    ASSERT(lstr_eq(*lstr, LSTR("a")));
    lstr = labels_next(&labels);
    ASSERT(lstr);
    ASSERT(lstr_eq(*lstr, LSTR("bb")));
    lstr = labels_next(&labels);
    ASSERT(lstr);
    ASSERT(lstr_eq(*lstr, LSTR("ccc")));
    lstr = labels_next(&labels);
    ASSERT(!lstr);

    used += labels.used;
    lstr = labels_iter(&labels, buf, used);
    ASSERT(lstr);
    ASSERT(lstr_eq(*lstr, LSTR("z")));
    lstr = labels_next(&labels);
    ASSERT(lstr);
    ASSERT(lstr_eq(*lstr, LSTR("bb")));
    lstr = labels_next(&labels);
    ASSERT(lstr);
    ASSERT(lstr_eq(*lstr, LSTR("ccc")));
    lstr = labels_next(&labels);
    ASSERT(!lstr);

    ASSERT(labels.used == 14);

    #undef ASSERT

    return e;
}

static derr_t test_labels_read(void){
    derr_t e = E_OK;

    lstr_t *heapname = NULL;

    #define ASSERT(code) \
        if(!(code)) ORIG_GO(&e, E_VALUE, "assertion failed: " #code, cu)

    char buf[] =
        "\x01" "a" "\x02" "bb" "\x03" "ccc" "\x00" // a.bb.ccc (odd)
        "\x01" "z" "\xC0\x00" // z.a.bb.ccc (even)
    ;
    size_t len = sizeof(buf)-1;

    // validate with parse_name
    size_t used = 0;
    used = parse_name(buf, len, used);
    ASSERT(used == 10);

    used = parse_name(buf, len, used);
    ASSERT(used == 14);

    // use heap memory for asan checks
    size_t heapcap = 2;
    heapname = dmalloc(&e, heapcap * sizeof(*heapname));
    CHECK(&e);

    lstr_t name[10];
    size_t cap = sizeof(name) / sizeof(*name);

    size_t n;

    // check length limiting
    n = labels_read(buf, 0, heapname, heapcap);
    ASSERT(n == heapcap + 1);

    // test reading
    n = labels_read(buf, 0, name, cap);
    ASSERT(n == 3);
    ASSERT(lstr_eq(name[0], LSTR("a")));
    ASSERT(lstr_eq(name[1], LSTR("bb")));
    ASSERT(lstr_eq(name[2], LSTR("ccc")));

    // test reversing (odd)
    labels_reverse(name, n);
    ASSERT(lstr_eq(name[0], LSTR("ccc")));
    ASSERT(lstr_eq(name[1], LSTR("bb")));
    ASSERT(lstr_eq(name[2], LSTR("a")));

    n = labels_read(buf, 10, name, cap);
    ASSERT(n == 4);
    ASSERT(lstr_eq(name[0], LSTR("z")));
    ASSERT(lstr_eq(name[1], LSTR("a")));
    ASSERT(lstr_eq(name[2], LSTR("bb")));
    ASSERT(lstr_eq(name[3], LSTR("ccc")));

    // test reversing (even)
    labels_reverse(name, n);
    ASSERT(lstr_eq(name[0], LSTR("ccc")));
    ASSERT(lstr_eq(name[1], LSTR("bb")));
    ASSERT(lstr_eq(name[2], LSTR("a")));
    ASSERT(lstr_eq(name[3], LSTR("z")));

    #undef ASSERT

cu:
    if(heapname) free(heapname);
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
    RUN(test_labels_read());

    printf("%s\n", retval ? "FAIL" : "PASS");

    return retval;
}
