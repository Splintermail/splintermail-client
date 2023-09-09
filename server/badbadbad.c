#include <string.h>

#include "libdstr/libdstr.h"
#include "badbadbad_alert.h"

int main(int argc, char** argv){
    derr_t e = E_OK;

    DROP_CMD( logger_add_fileptr(LOG_LVL_DEBUG, stderr) );

    char* summary_str;
    if(argc > 1){
        summary_str = argv[1];
    }else{
        summary_str = "summary missing from call to badbadbad executable";
    }

    dstr_t summary;
    DSTR_WRAP(summary, summary_str, strlen(summary_str), true);

    // some working memory
    dstr_t body = (dstr_t){0};
    PROP_GO(&e, dstr_new(&body, 4096), send);

    // try to read from stdin
    size_t amnt_read;
    do{
        PROP_GO(&e, dstr_read(0, &body, 2048, &amnt_read), send);
    }while(amnt_read > 0);

send:
    CATCH_ANY(&e){
        DUMP(e);
        DROP_VAR(&e);
    }
    badbadbad_alert(summary, body);

    // this is always safe because we zeroed the dstr before allocating it
    dstr_free(&body);
    return 0;
}
