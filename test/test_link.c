#include <libdstr/common.h>
#include <libdstr/logger.h>
#include <libdstr/link.h>

#include "test_utils.h"

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

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_WARN);

    PROP_GO(&e, test_link(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
