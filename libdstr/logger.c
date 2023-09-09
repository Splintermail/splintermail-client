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

/* PROP() and friends merge two error contexts.  Typically, the first error
   context is the main `derr_t e` for the function, and the second is the
   return value of some command, but also it may be the secondary error context
   e2 in the `else` statement after a CATCH clause which does not trigger. */

// append code's trace to e and return true if code is an error.
bool pvt_prop(
    derr_t *e, derr_t code, const char *file, const char *func, int line
){
    // always keep the new trace
    if(code.msg.data != NULL){
        // handle empty errors or duplicate parameters
        if(e->msg.data == NULL || e->msg.data == code.msg.data){
            // just use the new trace as is
            e->msg = code.msg;
        }else if(e->msg.data != NULL){
            // apend the new trace to the existing one
            // best effort; ignore error
            dstr_append_quiet(&e->msg, &code.msg);
            // done with the new msg
            dstr_free(&code.msg);
        }
    }

    // prefer the new type (this is different from MERGE)
    if(code.type != E_NONE){
        e->type = code.type;
    }

    if(!is_error(code)) return false;

    TRACE(e, "propagating %x from file %x: %x(), line %x\n",
        FD(error_to_dstr(e->type)), FS(file), FS(func), FI(line));
    return true;
}

bool pvt_prop_var(
    derr_t *e, derr_t *e2, const char *file, const char *func, int line
){
    bool ret = pvt_prop(e, *e2, file, func, line);
    if(e != e2) *e2 = E_OK;
    return ret;
}

/* CHECK handles an error that we have ignored until now, useful when
   transitioning from chunks of code which use the builder api error handling
   strategy to chunks of code which use normal error handling */
bool pvt_check(derr_t *e, const char *file, const char *func, int line){
    if(!is_error(*e)) return false;

    TRACE(e, "Handling %x in file %x: %x(), line %x\n",
        FD(error_to_dstr(e->type)), FS(file), FS(func), FI(line));
    return true;
}

/* CATCH is almost always for checking the secondary error context, e2, to
   do some analysis before dropping it or merging it into the main error
   context `e` with RETHROW or PROP.

   The reason you can't do CATCHing on a single error stack is that if you
   decide to drop an error you don't know the "scope" of what you want to drop;
   you want to erase the errors for this thing you tried but not any other
   error information attached to the trace.  By having a separate derr_t you
   have a clearly defined scope for the CATCH to make decisions about.  Then
   you can choose to DROP the error, or merge it into the main error stack with
   a PROP or a RETHROW. */

bool trace_catch(derr_t *e, const char *file, const char *func, int line){
    TRACE(e, "catching %x in file %x: %x(), line %x\n",
            FD(error_to_dstr(e->type)), FS(file), FS(func), FI(line));
    return true;
}

/* RETHROW works like ORIG except it gives context to generic low-level errors
   in situations where we know what they mean.  Example:

    dstr_toi(){
        // the main error context
        derr_t e = E_OK;

        ...

        // the secondary error context
        derr_t e2 = dstr_copy(... , ...) // returns E_FIXEDSIZE

        // right now we know E_FIXEDSIZE means a bad parameter,
        // but higher up code might not know
        CATCH(&e2, E_FIXEDSIZE){
            // merge the secondary context into the main context as E_PARAM
            RETHROW(&e, &e2, E_PARAM);
        }else{
            // Any uncaught errors in e2 should be merged into `e` normally
            PROP(e, e2);
        }
        ...
    }

    Now it is easier to handle errors from dstr_toi() in higher-level code
    because there is only one possible error it can throw.
    */
void pvt_rethrow(
    derr_t *e,
    derr_t *e2,
    derr_type_t newtype,
    const char *file,
    const char *func,
    int line
){
    derr_type_t oldtype = e2->type;
    // we always use the specified type
    e->type = newtype;
    if(e->msg.data == e2->msg.data){
        // same error object, just append to it at the end
    }else if(e->msg.data == NULL){
        // if there was no previous trace, just use the new trace
        e->msg = e2->msg;
        // done with the old error, but don't free the trace we are reusing
        *e2 = E_OK;
    }else{
        // otherwise, combine the traces
        /* TODO: you should clearly delineate the new trace from the old trace,
                 so that it is clear exactly what is being rethrown */
        dstr_append_quiet(&e->msg, &e2->msg);
        // done with the old error
        DROP_VAR(e2);
    }
    TRACE(e, "rethrowing %x as %x in file %x: %x(), line %x\n",
        FD(error_to_dstr(oldtype)), FD(error_to_dstr(newtype)),
        FS(file), FS(func), FI(line));
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
