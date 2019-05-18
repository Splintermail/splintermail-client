#include "common.h"
#include "logger.h"

#define LIST_LEN_MAX 16

// fixed-size static buffer of FILE*'s that we are logging to
static FILE* fplist[LIST_LEN_MAX];
static const char* fnlist[LIST_LEN_MAX];
static log_level_t fplevels[LIST_LEN_MAX];
static log_level_t fnlevels[LIST_LEN_MAX];
static size_t fplist_len = 0;
static size_t fnlist_len = 0;

derr_t logger_add_fileptr(log_level_t level, FILE* f){
    derr_t e = E_OK;
    if(fplist_len == sizeof(fplist)/sizeof(*fplist)){
        ORIG(e, E_FIXEDSIZE, "can't log to any more FILE*'s")
    }

    fplist[fplist_len] = f;
    fplevels[fplist_len] = level;

    fplist_len++;

    return E_OK;
}

derr_t logger_add_filename(log_level_t level, const char* f){
    derr_t e = E_OK;
    if(fnlist_len == sizeof(fnlist)/sizeof(*fnlist)){
        ORIG(e, E_FIXEDSIZE, "can't log to any more file names")
    }

    fnlist[fnlist_len] = f;
    fnlevels[fnlist_len] = level;

    fnlist_len++;

    return E_OK;
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
            FILE* f = fopen(fnlist[i], "a");
            // there's no good way to report errors
            if(!f) continue;
            pvt_ffmt_quiet(f, NULL, format, args, nargs);
            fclose(f);
        }
    }
    return 0;
}

// writing to the trace is usually best-effort and the error will be ignored.
derr_type_t pvt_trace_quiet(
        derr_t *e, const char* fstr, const fmt_t *args, size_t nargs){
    derr_type_t type;
    // if e is yet-unallocated, allocate it first
    if(e->msg.data == NULL){
        type = dstr_new_quiet(&e->msg, 1024);
        if(type) return type;
    }
    // attempt to append to the trace
    return pvt_fmt_quiet(&e->msg, fstr, args, nargs);
}
