#include <common.h>
#include <logger.h>

#include "test_utils.h"

static derr_t test_logger(void){


    return E_OK;
}


int main(void){
    derr_t error;
    PROP_GO( test_logger(), test_fail);
    printf("PASS\n");
    return 0;

test_fail:
    return 1;
}
