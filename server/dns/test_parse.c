#include "server/dns/libdns.h"

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
    ASSERT(lstr_ieq(*lstr, LSTR("a")));
    lstr = labels_next(&labels);
    ASSERT(lstr);
    ASSERT(lstr_ieq(*lstr, LSTR("bB")));
    lstr = labels_next(&labels);
    ASSERT(lstr);
    ASSERT(lstr_ieq(*lstr, LSTR("cCc")));
    lstr = labels_next(&labels);
    ASSERT(!lstr);

    used += labels.used;
    lstr = labels_iter(&labels, buf, used);
    ASSERT(lstr);
    ASSERT(lstr_ieq(*lstr, LSTR("z")));
    lstr = labels_next(&labels);
    ASSERT(lstr);
    ASSERT(lstr_ieq(*lstr, LSTR("bb")));
    lstr = labels_next(&labels);
    ASSERT(lstr);
    ASSERT(lstr_ieq(*lstr, LSTR("ccc")));
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
        "\x01" "a" "\x02" "bb" "\x03" "ccc" "\x00" // a.bb.ccc
        "\x01" "z" "\xC0\x00" // z.a.bb.ccc
        "\x01" "x" "\xC0\x0a" // x.z.a.bb.ccc
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

    lstr_t name[4];
    size_t cap = sizeof(name) / sizeof(*name);

    size_t n;

    // check length limiting
    n = labels_read(buf, 0, heapname, heapcap);
    ASSERT(n == 3);
    n = labels_read(buf, 10, heapname, heapcap);
    ASSERT(n == 4);
    n = labels_read(buf, 14, heapname, heapcap);
    ASSERT(n == 5);

    n = labels_read_reverse(buf, 0, heapname, heapcap);
    ASSERT(n == 3);
    n = labels_read_reverse(buf, 10, heapname, heapcap);
    ASSERT(n == 4);
    n = labels_read_reverse(buf, 14, heapname, heapcap);
    ASSERT(n == 5);

    // test reading under capacity
    n = labels_read(buf, 0, name, cap);
    ASSERT(n == 3);
    ASSERT(lstr_ieq(name[0], LSTR("a")));
    ASSERT(lstr_ieq(name[1], LSTR("bb")));
    ASSERT(lstr_ieq(name[2], LSTR("ccc")));

    n = labels_read_reverse(buf, 0, name, cap);
    ASSERT(n == 3);
    ASSERT(lstr_ieq(name[0], LSTR("ccc")));
    ASSERT(lstr_ieq(name[1], LSTR("bb")));
    ASSERT(lstr_ieq(name[2], LSTR("a")));

    // test reading at-capacity
    n = labels_read(buf, 10, name, cap);
    ASSERT(n == 4);
    ASSERT(lstr_ieq(name[0], LSTR("z")));
    ASSERT(lstr_ieq(name[1], LSTR("a")));
    ASSERT(lstr_ieq(name[2], LSTR("bb")));
    ASSERT(lstr_ieq(name[3], LSTR("ccc")));

    n = labels_read_reverse(buf, 10, name, cap);
    ASSERT(n == 4);
    ASSERT(lstr_ieq(name[0], LSTR("ccc")));
    ASSERT(lstr_ieq(name[1], LSTR("bb")));
    ASSERT(lstr_ieq(name[2], LSTR("a")));
    ASSERT(lstr_ieq(name[3], LSTR("z")));

    // test reading over capacity
    n = labels_read(buf, 14, name, cap);
    ASSERT(n == 5);
    // capture first n
    ASSERT(lstr_ieq(name[0], LSTR("x")));
    ASSERT(lstr_ieq(name[1], LSTR("z")));
    ASSERT(lstr_ieq(name[2], LSTR("a")));
    ASSERT(lstr_ieq(name[3], LSTR("bb")));

    n = labels_read_reverse(buf, 14, name, cap);
    // capture last n
    ASSERT(n == 5);
    ASSERT(lstr_ieq(name[0], LSTR("ccc")));
    ASSERT(lstr_ieq(name[1], LSTR("bb")));
    ASSERT(lstr_ieq(name[2], LSTR("a")));
    ASSERT(lstr_ieq(name[3], LSTR("z")));

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
