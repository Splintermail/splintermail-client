#include <string.h>

#include "common.h"
#include "logger.h"
#include "badbadbad_alert.h"

int main(int argc, char** argv){
    char* summary_str;
    if(argc > 1){
        summary_str = argv[1];
    }else{
        summary_str = "summary missing from call to badbadbad executable";
    }

    dstr_t summary;
    DSTR_WRAP(summary, summary_str, strlen(summary_str), true);

    derr_t error;

    // some working memory
    dstr_t body;
    memset(&body, 0, sizeof(body));
    PROP_GO( dstr_new(&body, 4096), send);

    // try to read from stdin
    size_t amnt_read;
    do{
        PROP_GO( dstr_read(0, &body, 2048, &amnt_read), send);
    }while(amnt_read > 0);

send:
    badbadbad_alert(&summary, &body);

    // this is always safe because we zeroed the dstr before allocating it
    dstr_free(&body);
    return 0;
}
