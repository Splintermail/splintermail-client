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

// do the format once for all log outputs, into a static buffer if possible
static derr_type_t pre_log_fmt(
    const char* format,
    const fmt_i **args,
    size_t nargs,
    dstr_t* stack,
    dstr_t* heap,
    dstr_t* out,
    bool *done
){
    if(*done) return E_NONE;

    derr_type_t etype;

    // try and expand into stack_dstr
    etype = _fmt_quiet(WD(stack), format, args, nargs);
    if(etype == E_FIXEDSIZE) goto use_heap;
    if(etype) return etype;
    // it worked, return stack_dstr as *out
    *out = *stack;
    *done = true;
    return E_NONE;

use_heap:
    // we will need to allocate the heap_dstr to be bigger than stack_dstr
    etype = dstr_new_quiet(heap, stack->size * 2);
    if(etype) return etype;
    etype = _fmt_quiet(WD(heap), format, args, nargs);
    if(etype) goto fail_heap;
    *out = *heap;
    *done = true;
    return E_NONE;

fail_heap:
    dstr_free(heap);
    return etype;
}

// this ALWAYS return 0, for use in the CATCH macro
// don't use any error-handling macros because they would recurse infinitely
int pvt_do_log(
    log_level_t level, const char* fstr, const fmt_i **args, size_t nargs
){
    DSTR_VAR(stack, 1024);
    dstr_t heap = {0};
    dstr_t buf = {0};
    // format lazily
    bool done = false;

    derr_type_t etype;

    // just print the whole format string to all of our fplist
    for(size_t i = 0; i < fplist_len; i++){
        // only print if this output is registered to see this level
        if(level >= fplevels[i]){
            etype = pre_log_fmt(fstr, args, nargs, &stack, &heap, &buf, &done);
            if(etype) return 0;
            fwrite(buf.data, 1, buf.len, fplist[i]);
        }
    }
    // repeat with fnlist
    for(size_t i = 0; i < fnlist_len; i++){
        if(level >= fnlevels[i]){
            etype = pre_log_fmt(fstr, args, nargs, &stack, &heap, &buf, &done);
            if(etype) return 0;
            // open the file
            FILE* f = compat_fopen(fnlist[i], "a");
            // there's no good way to report errors
            if(!f) continue;
            fwrite(buf.data, 1, buf.len, f);
            fclose(f);
        }
    }
    // fallback to stderr/LOG_LVL_WARN if no outputs are set
    if(!outputs_set && level >= LOG_LVL_WARN){
        etype = pre_log_fmt(fstr, args, nargs, &stack, &heap, &buf, &done);
        if(etype) return 0;
        fwrite(buf.data, 1, buf.len, stderr);
    }
    if(_auto_log_flush){
        log_flush();
    }
    dstr_free(&heap);
    return 0;
}

void pvt_do_log_fatal(const char* fstr, const fmt_i **args, size_t nargs){
    pvt_do_log(LOG_LVL_FATAL, fstr, args, nargs);
    log_flush();
    abort();
}

void DUMP(derr_t e){
    if(is_error(e)){
        LOG_ERROR("error trace (%x):\n", FD(error_to_dstr(e.type)));
        if(e.msg.len > 0){
            LOG_ERROR("%x", FD((e).msg));
        }
    }
}

// TRACE() and friends are best-effort append-to-derr_t.msg functions
derr_type_t pvt_trace_quiet(
    derr_t *e, const char* fstr, const fmt_i **args, size_t nargs
){
    // if e is yet-unallocated, allocate it first
    if(e->msg.data == NULL){
        derr_type_t etype = dstr_new_quiet(&e->msg, 1024);
        if(etype) return etype;
    }
    // attempt to append to the trace
    derr_type_t etype = _fmt_quiet(WD(&e->msg), fstr, args, nargs);
    return etype;
}

// ORIG() and friends set the derr_t.type and append to derr_t.msg
void pvt_orig(
    derr_t *e,
    derr_type_t code,
    const char *fstr,
    const fmt_i **args,
    size_t nargs,
    const char *file,
    const char *func,
    int line
){
    e->type = code;
    pvt_trace_quiet(e, fstr, args, nargs);
    TRACE(e,
        "originating %x from file %x: %x(), line %x\n",
        FD(error_to_dstr(code)), FS(file), FS(func), FI(line)
    );
}

/* MERGE and friends don't do control flow because they are only used when
   gathering errors from multiple threads.

   The only difference between MERGE_CMD and MERGE_VAR is that MERGE_VAR takes
   a pointer to a derr_t instead of a derr_t value, so that it set the variable
   to E_OK. */

void pvt_merge_cmd(
    derr_t *e,
    derr_t cmd,
    const char* message,
    const char* file,
    const char* func,
    int line
){
    // prefer the old type
    if(e->type == E_NONE){
        e->type = cmd.type;
    }
    // if e has no message, just use the new one
    if(e->msg.data == NULL){
        e->msg = cmd.msg;
        if(e->msg.data != NULL){
            // if the new one is non-NULL, extend it with some context
            TRACE(e, "merging %x from %x at file %x: %x(), line %x\n",
                FD(error_to_dstr(cmd.type)), FS(message), FS(file),
                FS(func), FI(line));
        }

    }else{
        // otherwise, embed the new message into the old one
        if(is_error(cmd) || cmd.msg.data != NULL){
            // embed the new error trace with a message
            TRACE(e, "merging %x from %x at file %x: %x(), line %x\n%x",
                FD(error_to_dstr(cmd.type)), FS(message), FS(file),
                FS(func), FI(line), FD(cmd.msg));
        }
        // done with the old msg
        dstr_free(&cmd.msg);
    }
}

void pvt_merge_var(
    derr_t *e,
    derr_t *var,
    const char* message,
    const char* file,
    const char* func,
    int line
){
    pvt_merge_cmd(e, *var, message, file, func, line);
    // pvt_merge_cmd will dstr_free() the msg but we need to erase the pointer
    *var = E_OK;
}

// append code's trace to e and return true if code is an error.
bool pvt_multiprop_var(
    derr_t *out,
    const char *file,
    const char *func,
    int line,
    derr_t **errs,
    size_t nerrs
){
    size_t i = 0;
    // search for the first error (which may be out)
    bool found_err = is_error(*out);
    for(; i < nerrs && !found_err; i++){
        derr_t *e = errs[i];
        if(!is_error(*e)){
            // drop the trace of anything that isn't an error
            // (this is different than pvt_prop_var)
            dstr_free(&e->msg);
            continue;
        }
        found_err = true;
        pvt_prop_var(out, e, file, func, line);
    }
    // clear secondary errors
    for(; i < nerrs; i++){
        DROP_VAR(errs[i]);
    }
    return found_err;
}
