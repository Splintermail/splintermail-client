#include <stdio.h>
#include <stdlib.h>

#include <libdstr/libdstr.h>

// deletes whatever is found in the first argument
int main(int argc, char** argv) {
    derr_t e = E_OK;

    // log to stdout
    logger_add_fileptr(LOG_LVL_DEBUG, stdout);

    // also log to a log file in %TEMP%
    bool ok;
    dstr_t temp = dgetenv(DSTR_LIT("TEMP"), &ok);
    DSTR_VAR(logfile, 256);
    if(!ok){
        LOG_ERROR("$TEMP not set\n");
    }else{
        IF_PROP(&e, FMT(&logfile, "%x\\splintermail-rm_rf.log", FD(temp)) ){
            TRACE(&e, "unable to configure to logfile in %TEMP%\n");
            DUMP(e);
            DROP_VAR(&e);
        }else{
            logger_add_filename(LOG_LVL_DEBUG, logfile.data);
        }
    }

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
    IF_PROP(&e, rm_rf(argv[1]) ){
        TRACE(&e, "file removal failed\n");
        // never exit 1 at runtime!  That blocks the uninstalling!
        DUMP(e);
        DROP_VAR(&e);
        return 0;
    }
    return 0;
}
