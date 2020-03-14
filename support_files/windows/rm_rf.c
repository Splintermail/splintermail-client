#include <stdio.h>
#include <stdlib.h>

#include <libdstr/libdstr.h>

// deletes whatever is found in the first argument
int main(int argc, char** argv) {
    derr_t e = E_OK;
    // log to stdout
    logger_add_fileptr(LOG_LVL_DEBUG, stdout);
    // // also log to a log file
    // logger_add_filename(LOG_LVL_DEBUG, "C:/testlog");

    LOG_DEBUG("rm_rf called like this:\n");
    for(int i = 0; i < argc; i++){
        LOG_DEBUG("%x\n", FS(argv[i]));
    }
    LOG_DEBUG("\n");

    // check number of arguments
    if(argc != 2){
        LOG_ERROR("wrong number of arguments!\n");
        LOG_ERROR("usage: %x TARGET\n", FS(argv[0]));
        return 255;
    }

    // do the deletion
    PROP_GO(e, rm_rf(argv[1]), fail);

    return 0;

fail:
    DUMP(e);
    DROP(e);
    return 1;
}
