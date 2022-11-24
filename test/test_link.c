#include <libdstr/libdstr.h>

#include "test_utils.h"

#define EXPECT_LIST(e, name, got, exp, nexp, XVAL, XEXP) do { \
    derr_t *_e = (e); \
    link_t *__got = (got); \
    size_t _i = 0; \
    for(link_t *_link = __got->next; _link != __got; _link = _link->next){ \
        if(_i == nexp){ \
            ORIG(_e, \
                E_VALUE, name " too long, expected only %x items", FU(nexp) \
            ); \
        } \
        XEXP(_e, "item of " name, (XVAL(_link)), exp[_i++]); \
    } \
    EXPECT_U(_e, "list length", _i, nexp); \
} while (0)

typedef struct {
    size_t val;
    link_t link;
} val_t;
DEF_CONTAINER_OF(val_t, link, link_t) \

static derr_t test_link(void){
    derr_t e = E_OK;
    // prepare some values
    val_t vals[10];
    for(size_t i = 0; i < sizeof(vals)/sizeof(*vals); i++){
        vals[i].val = i;
        link_init(&vals[i].link);
    }
    // append all the values to a list
    link_t list;
    link_init(&list);
    for(size_t i = 0; i < sizeof(vals)/sizeof(*vals); i++){
        link_list_append(&list, &vals[i].link);
    }

    // make sure they are all the right values
    size_t i = 0;
    val_t *v;
    LINK_FOR_EACH(v, &list, val_t, link){
        if(v->val != i){
            ORIG(&e, E_VALUE, "wrong value");
        }
        i++;
    }
    if(i != 10){
        ORIG(&e, E_VALUE, "list too short");
    }

    // make sure FOR_EACH_SAFE is safe against link_remove()
    val_t *tmp;
    LINK_FOR_EACH_SAFE(v, tmp, &list, val_t, link){
        link_remove(&v->link);
    }

    bool empty = true;
    LINK_FOR_EACH(v, &list, val_t, link){
        empty = false;
        TRACE(&e, "value found: %x\n", FU(v->val));
    }
    if(!empty){
        ORIG(&e, E_VALUE, "list not empty!");
    }

    return e;
}

static derr_t test_zeroized(void){
    derr_t e = E_OK;

    link_t link = {0};

    if(!link_list_isempty(&link)){
        ORIG(&e, E_VALUE, "zeroized list should be empty");
    }

    // check on common operations while other objects are freed
    if(link_list_pop_first(&link) != NULL){
        ORIG(&e, E_VALUE, "zeroized list should return NULL from pop_first()");
    }
    if(link_list_pop_last(&link) != NULL){
        ORIG(&e, E_VALUE, "zeroized list should return NULL from pop_last()");
    }

    link_remove(&link);

    // link still zeroized
    EXPECT_NULL(&e, "link.next", link.next);

    // append initializes head and item
    link_t item = {0};
    link_list_append(&link, &item);
    link_remove(&item);
    EXPECT_P(&e, "link.next", link.next, &link);
    EXPECT_P(&e, "item.next", item.next, &item);

    // prepend also initializes head and item
    link = (link_t){0};
    item = (link_t){0};
    link_list_prepend(&link, &item);
    link_remove(&item);
    EXPECT_P(&e, "link.next", link.next, &link);
    EXPECT_P(&e, "item.next", item.next, &item);

    return e;
}

static derr_t test_listwise_append(void){
    derr_t e = E_OK;

    // prepare some values
    val_t vals[10];
    size_t nvals = sizeof(vals)/sizeof(*vals);
    for(size_t i = 0; i < nvals; i++){
        vals[i].val = i;
        link_init(&vals[i].link);
    }

    // half of links in one list, half in the other
    link_t l1;
    link_t l2;
    link_init(&l1);
    link_init(&l2);
    for(size_t i = 0; i < nvals; i++){
        link_list_append(i < nvals/2 ? &l1 : &l2, &vals[i].link);
    }

    link_list_append_list(&l1, &l2);

    // donor list empty
    EXPECT_B(&e, "isempty(l2)", link_list_isempty(&l2), true);

    size_t *exp = (size_t[]){0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    for(size_t i = 0; i < nvals; i++){
        link_t *link = link_list_pop_first(&l1);
        EXPECT_NOT_NULL(&e, "link", link);
        val_t *val = CONTAINER_OF(link, val_t, link);
        EXPECT_U(&e, "val", val->val, exp[i]);
    }

    // reset
    for(size_t i = 0; i < nvals; i++){
        link_remove(&vals[i].link);
    }

    // again, but prepend
    for(size_t i = 0; i < nvals; i++){
        link_list_append(i < nvals/2 ? &l1 : &l2, &vals[i].link);
    }

    link_list_prepend_list(&l1, &l2);

    // donor list empty
    EXPECT_B(&e, "isempty(l2)", link_list_isempty(&l2), true);

    exp = (size_t[]){5, 6, 7, 8, 9, 0, 1, 2, 3, 4};
    for(size_t i = 0; i < nvals; i++){
        link_t *link = link_list_pop_first(&l1);
        EXPECT_NOT_NULL(&e, "link", link);
        val_t *val = CONTAINER_OF(link, val_t, link);
        EXPECT_U(&e, "val", val->val, exp[i]);
    }

    return e;
}

static derr_t test_pop_n(void){
    derr_t e = E_OK;

    // prepare some values
    val_t vals[10];
    size_t nvals = sizeof(vals)/sizeof(*vals);
    for(size_t i = 0; i < nvals; i++){
        vals[i].val = i;
        link_init(&vals[i].link);
    }

    // half of links in one list, half in the other
    link_t l1;
    link_t l2;
    link_init(&l1);
    link_init(&l2);
    for(size_t i = 0; i < nvals; i++){
        link_list_append(i < nvals/2 ? &l1 : &l2, &vals[i].link);
    }

    link_t *r1a;
    link_t *r1b;
    link_t *r1c;
    link_t *r2a;
    link_t *r2b;
    link_t *r2c;

    // pop first, two from l1, one from l2
    bool ok = link_list_pop_first_n(
        LINK_IO(&l1, &r1a), LINK_IO(&l1, &r1b), LINK_IO(&l2, &r2a)
    );
    EXPECT_B(&e, "ok", ok, true);
    EXPECT_NOT_NULL(&e, "r1a", r1a);
    EXPECT_NOT_NULL(&e, "r1b", r1b);
    EXPECT_NOT_NULL(&e, "r2a", r2a);
    EXPECT_U(&e, "r1a.value", CONTAINER_OF(r1a, val_t, link)->val, 0);
    EXPECT_U(&e, "r1b.value", CONTAINER_OF(r1b, val_t, link)->val, 1);
    EXPECT_U(&e, "r2a.value", CONTAINER_OF(r2a, val_t, link)->val, 5);

    // pop last, two from l2, one from l1
    ok = link_list_pop_last_n(
        LINK_IO(&l1, &r1a), LINK_IO(&l2, &r2a), LINK_IO(&l2, &r2b)
    );
    EXPECT_B(&e, "ok", ok, true);
    EXPECT_NOT_NULL(&e, "r1a", r1a);
    EXPECT_NOT_NULL(&e, "r2a", r2a);
    EXPECT_NOT_NULL(&e, "r2b", r2b);
    EXPECT_U(&e, "r1a.value", CONTAINER_OF(r1a, val_t, link)->val, 4);
    EXPECT_U(&e, "r2a.value", CONTAINER_OF(r2a, val_t, link)->val, 9);
    EXPECT_U(&e, "r2b.value", CONTAINER_OF(r2b, val_t, link)->val, 8);

    // pop first too many, make sure they're put back in the right order
    ok = link_list_pop_first_n(
        LINK_IO(&l2, &r2a),
        LINK_IO(&l1, &r1a),
        LINK_IO(&l1, &r1b),
        LINK_IO(&l1, &r1c)
    );
    EXPECT_B(&e, "ok", ok, false);
    EXPECT_NULL(&e, "r2a", r2a);
    EXPECT_NULL(&e, "r1a", r1a);
    EXPECT_NULL(&e, "r1b", r1b);
    EXPECT_NULL(&e, "r1c", r1c);
    #define XVAL(l) CONTAINER_OF((l), val_t, link)->val
    {
        size_t exp[] = {2, 3};
        size_t nexp = sizeof(exp)/sizeof(*exp);
        EXPECT_LIST(&e, "l1", &l1, exp, nexp, XVAL, EXPECT_U);
    }
    {
        size_t exp[] = {6, 7};
        size_t nexp = sizeof(exp)/sizeof(*exp);
        EXPECT_LIST(&e, "l2", &l2, exp, nexp, XVAL, EXPECT_U);
    }

    // pop last too many, make sure they're put back in the right order
    // also ensure extra output r2d gets zeroized
    link_t *r2d = &l2;
    ok = link_list_pop_last_n(
        LINK_IO(&l1, &r1a),
        LINK_IO(&l2, &r2a),
        LINK_IO(&l2, &r2b),
        LINK_IO(&l2, &r2c),
        LINK_IO(&l2, &r2d),
    );
    EXPECT_B(&e, "ok", ok, false);
    EXPECT_NULL(&e, "r1a", r1a);
    EXPECT_NULL(&e, "r2a", r2a);
    EXPECT_NULL(&e, "r2b", r2b);
    EXPECT_NULL(&e, "r2c", r2c);
    EXPECT_NULL(&e, "r2d", r2d);
    {
        size_t exp[] = {2, 3};
        size_t nexp = sizeof(exp)/sizeof(*exp);
        EXPECT_LIST(&e, "l1", &l1, exp, nexp, XVAL, EXPECT_U);
    }
    {
        size_t exp[] = {6, 7};
        size_t nexp = sizeof(exp)/sizeof(*exp);
        EXPECT_LIST(&e, "l2", &l2, exp, nexp, XVAL, EXPECT_U);
    }
    #undef XVAL

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_WARN);

    PROP_GO(&e, test_link(), test_fail);
    PROP_GO(&e, test_zeroized(), test_fail);
    PROP_GO(&e, test_listwise_append(), test_fail);
    PROP_GO(&e, test_pop_n(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
