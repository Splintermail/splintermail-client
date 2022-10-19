#include <stdlib.h>

#include "libdstr.h"

#define LIST_LEN_MAX 16

// fixed-size static buffer of FILE*'s that we are logging to
static FILE* fplist[LIST_LEN_MAX];
static const char* fnlist[LIST_LEN_MAX];
static log_level_t fplevels[LIST_LEN_MAX];
static log_level_t fnlevels[LIST_LEN_MAX];
static size_t fplist_len = 0;
static size_t fnlist_len = 0;
static bool _auto_log_flush = false;
static bool outputs_set = false;

derr_t logger_add_fileptr(log_level_t level, FILE* f){
    derr_t e = E_OK;
    if(fplist_len == sizeof(fplist)/sizeof(*fplist)){
        ORIG(&e, E_FIXEDSIZE, "can't log to any more FILE*'s");
    }

    fplist[fplist_len] = f;
    fplevels[fplist_len] = level;

    fplist_len++;

    outputs_set = true;

    return e;
}

derr_t logger_add_filename(log_level_t level, const char* f){
    derr_t e = E_OK;
    if(fnlist_len == sizeof(fnlist)/sizeof(*fnlist)){
        ORIG(&e, E_FIXEDSIZE, "can't log to any more file names");
    }

    fnlist[fnlist_len] = f;
    fnlevels[fnlist_len] = level;

    fnlist_len++;

    outputs_set = true;

    return e;
}

void logger_clear_outputs(void){
    fplist_len = 0;
    fnlist_len = 0;
}

void log_flush(void){
    // only need to flush fplist, not fnlist
    for(size_t i = 0; i < fplist_len; i++){
        fflush(fplist[i]);
    }
}

void auto_log_flush(bool val){
    _auto_log_flush = val;
}

// don't use any error-handling macros because they would recurse infinitely
int pvt_do_log(log_level_t level, const char* format,
               const fmt_t* args, size_t nargs){
    // just print the whole format string to all of our fplist
    for(size_t i = 0; i < fplist_len; i++){
        // only print if this output is registered to see this level
        if(level >= fplevels[i]){
            pvt_ffmt_quiet(fplist[i], NULL, format, args, nargs);
        }
    }
    // repeat with fnlist
    for(size_t i = 0; i < fnlist_len; i++){
        if(level >= fnlevels[i]){
            // open the file
            FILE* f = compat_fopen(fnlist[i], "a");
            // there's no good way to report errors
            if(!f) continue;
            pvt_ffmt_quiet(f, NULL, format, args, nargs);
            fclose(f);
        }
    }
    // fallback to stderr/LOG_LVL_WARN if no outputs are set
    if(!outputs_set && level >= LOG_LVL_WARN){
        pvt_ffmt_quiet(stderr, NULL, format, args, nargs);
    }
    if(_auto_log_flush){
        log_flush();
    }
    if(level == LOG_LVL_FATAL){
        log_flush();
        abort();
    }
    return 0;
}
