typedef enum {
    LOG_LVL_DEBUG,
    LOG_LVL_INFO,
    LOG_LVL_WARN,
    LOG_LVL_ERROR,
    LOG_LVL_FATAL,
} log_level_t;

derr_t logger_add_fileptr(log_level_t level, FILE* f);
derr_t logger_add_filename(log_level_t level, const char* f);
void logger_clear_outputs(void);
void log_flush(void);
void auto_log_flush(bool val);
// this ALWAYS return 0, for use in the CATCH macro
int pvt_do_log(
    log_level_t level, const char* fstr, const fmt_i** args, size_t nargs
);
SM_NORETURN(
    void pvt_do_log_fatal(const char* fstr, const fmt_i** args, size_t nargs)
);
#define LOG_AS(log_level, fstr, ...) \
    pvt_do_log(log_level, fstr, \
             &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
             sizeof((const fmt_i*[]){NULL, __VA_ARGS__}) / sizeof(fmt_i*) - 1)
#define LOG_ERROR(fstr, ...) \
    pvt_do_log(LOG_LVL_ERROR, fstr, \
             &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
             sizeof((const fmt_i*[]){NULL, __VA_ARGS__}) / sizeof(fmt_i*) - 1)
#define LOG_WARN(fstr, ...) \
    pvt_do_log(LOG_LVL_WARN, fstr, \
             &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
             sizeof((const fmt_i*[]){NULL, __VA_ARGS__}) / sizeof(fmt_i*) - 1)
#define LOG_INFO(fstr, ...) \
    pvt_do_log(LOG_LVL_INFO, fstr, \
             &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
             sizeof((const fmt_i*[]){NULL, __VA_ARGS__}) / sizeof(fmt_i*) - 1)
#define LOG_DEBUG(fstr, ...) \
    pvt_do_log(LOG_LVL_DEBUG, fstr, \
             &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
             sizeof((const fmt_i*[]){NULL, __VA_ARGS__}) / sizeof(fmt_i*) - 1)
#define LOG_FATAL(fstr, ...) \
    pvt_do_log_fatal( \
        "FATAL ERROR in file %x: %x(), line %x: " fstr, \
        (const fmt_i*[]){ \
            FS(FILE_BASENAME), \
            FS(__func__), \
            FU(__LINE__), \
            __VA_ARGS__ \
        }, \
        sizeof((const fmt_i*[]){NULL, __VA_ARGS__}) / sizeof(fmt_i*) + 2 \
    )

#define FILE_LOC \
    FILE_BASENAME, __func__, __LINE__

static inline bool is_error(derr_t e){
    return e.type != E_NONE;
}

// for dropping an error directly from a command
static inline void DROP_CMD(derr_t e){
    dstr_free(&e.msg);
}

// for dropping and cleaning an error stored on the stack
static inline void DROP_VAR(derr_t *error){
    error->type = E_NONE;
    dstr_free(&error->msg);
}

void DUMP(derr_t e);

// TRACE() and friends are best-effort append-to-derr_t.msg functions
derr_type_t pvt_trace_quiet(
    derr_t *e, const char* fstr, const fmt_i **args, size_t nargs
);

#define TRACE(e, fstr, ...) \
    pvt_trace_quiet((e), \
        fstr, \
        &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
        sizeof((const fmt_i*[]){NULL, __VA_ARGS__}) / sizeof(fmt_i*) - 1 \
    )


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
);

#define TRACE_ORIG(e, code, fstr, ...) \
    pvt_orig( \
        (e), \
        (code), \
        "ERROR: " fstr "\n", \
        &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
        sizeof((const fmt_i*[]){NULL, __VA_ARGS__}) / sizeof(fmt_i*) - 1, \
        FILE_LOC \
    ) \

#define ORIG(e, code, fstr, ...) do { \
    pvt_orig( \
        (e), \
        (code), \
        "ERROR: " fstr "\n", \
        &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
        sizeof((const fmt_i*[]){NULL, __VA_ARGS__}) / sizeof(fmt_i*) - 1, \
        FILE_LOC \
    ); \
    return *(e); \
} while(0)

/* it's weird that label is between a format string and its args, but it's
   backwards-compatible and doesn't require any macro magic */
#define ORIG_GO(e, code, fstr, label, ...) do { \
    pvt_orig( \
        (e), \
        (code), \
        "ERROR: " fstr "\n", \
        &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
        sizeof((const fmt_i*[]){NULL, __VA_ARGS__}) / sizeof(fmt_i*) - 1, \
        FILE_LOC \
    ); \
    goto label; \
} while(0)


/* PROP() and friends merge two error contexts.  Typically, the first error
   context is the main `derr_t e` for the function, and the second is the
   return value of some command, but also it may be the secondary error context
   e2 in the `else` statement after a CATCH clause which does not trigger. */

// append code's trace to e and return true if code is an error.
static inline bool pvt_prop(derr_t *e, derr_t code,
        const char *file, const char *func, int line){
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

// An if statement with useful side-effects
#define IF_PROP(e, code) \
    if(pvt_prop(e, code, FILE_LOC))

// command can also be just a raw error code.
#define PROP(e, code) \
     do { if(pvt_prop(e, code, FILE_LOC)) { return *(e); } } while(0)

#define PROP_GO(e, code, _label) \
     do { if(pvt_prop(e, code, FILE_LOC)) { goto _label; } } while(0)

#define TRACE_PROP(e, code) \
    do { IF_PROP(e, code){} } while(0)


// PROP_VAR and friends clean up the variable they merge from
static inline bool pvt_prop_var(derr_t *e, derr_t *e2,
        const char *file, const char *func, int line){
    bool ret = pvt_prop(e, *e2, file, func, line);
    if(e != e2) *e2 = E_OK;
    return ret;
}

#define IF_PROP_VAR(e, e2) \
    if(pvt_prop_var(e, e2, FILE_LOC))

#define PROP_VAR(e, e2) \
    do { if(pvt_prop_var(e, e2, FILE_LOC)){ return *(e); } } while(0)

#define PROP_VAR_GO(e, e2, _label) \
    do { if(pvt_prop_var(e, e2, FILE_LOC)){ goto _label; } } while(0)

#define TRACE_PROP_VAR(e, e2) \
    do { IF_PROP_VAR(e, e2){} } while(0)


/* CHECK handles an error that we have ignored until now, useful when
   transitioning from chunks of code which use the builder api error handling
   strategy to chunks of code which use normal error handling */
static inline bool pvt_check(derr_t *e,
        const char *file, const char *func, int line){
    if(!is_error(*e)) return false;

    TRACE(e, "Handling %x in file %x: %x(), line %x\n",
        FD(error_to_dstr(e->type)), FS(file), FS(func), FI(line));
    return true;
}

#define CHECK(e) \
    do { if(pvt_check(e, FILE_LOC)){ return *(e); } } while(0)

#define CHECK_GO(e, label) \
    do { if(pvt_check(e, FILE_LOC)){ goto label; } } while(0)

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
static inline bool pvt_catch(derr_t *e, derr_type_t error_mask,
        const char *file, const char *func, int line){
    // if(e->type & error_mask){
    //     TRACE(e, "catching %x in file %x: %x(), line %x\n",
    //             FD(error_to_dstr(e->type)), FS(file), FS(func), FI(line));
    //     return true;
    // }
    if(error_mask->matches(error_mask, e->type)){
        TRACE(e, "catching %x in file %x: %x(), line %x\n",
                FD(error_to_dstr(e->type)), FS(file), FS(func), FI(line));
        return true;
    }
    return false;
}
#define CATCH(e, ...) if(pvt_catch(&(e), ERROR_GROUP(__VA_ARGS__), FILE_LOC))
// TODO: fix all the CATCH instances
#define CATCH2(e, ...) if(pvt_catch((e), ERROR_GROUP(__VA_ARGS__), FILE_LOC))


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
        CATCH(e2, E_FIXEDSIZE){
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
static inline void pvt_rethrow(derr_t *e, derr_t *e2, derr_type_t newtype,
        const char *file, const char *func, int line){
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

#define RETHROW(e, _new, _newtype) do {\
    pvt_rethrow((e), (_new), (_newtype), FILE_LOC); \
    return *(e); \
} while(0)

#define RETHROW_GO(e, _new, _newtype, label) do {\
    pvt_rethrow((e), (_new), (_newtype), FILE_LOC); \
    goto label; \
} while(0)

#define TRACE_RETHROW(e, _new, _newtype) \
    pvt_rethrow((e), (_new), (_newtype), FILE_LOC) \

/* NOFAIL will catch errors we have specifically prevented and turn them into
   E_INTERNAL errors */
static inline bool pvt_nofail(derr_t *e, derr_type_t mask, derr_t cmd,
        const char* file, const char* func, int line){
    // does the error from the command match the mask?
    if(pvt_catch(&cmd, mask, file, func, line)){
        // set error to E_INTERNAL
        pvt_rethrow(e, &cmd, E_INTERNAL, file, func, line);
        // take error actions
        return true;
    }
    // otherwise, use normal PROP semantics
    return pvt_prop(e, cmd, file, func, line);
}

#define NOFAIL(e, error_mask, cmd) do { \
    if(pvt_nofail((e), (error_mask), (cmd), FILE_LOC)){ return *(e); } \
} while(0)

#define NOFAIL_GO(e, error_mask, cmd, label) do { \
    if(pvt_nofail((e), (error_mask), (cmd), FILE_LOC)){ goto label; } \
} while(0)

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
);
#define MERGE_CMD(e, cmd, message) \
    pvt_merge_cmd((e), (cmd), (message), FILE_LOC)

void pvt_merge_var(
    derr_t *e,
    derr_t *var,
    const char* message,
    const char* file,
    const char* func,
    int line
);
#define MERGE_VAR(e, e2, message) \
    pvt_merge_var((e), (e2), (message), FILE_LOC)

/* if you pass an error laterally, you need to reset it without freeing the
   message. It could be done manually but if I have any good ideas later for
   what to do here I want these instances to be marked identically. */
#define PASSED(e) do { \
    (e) = E_OK; \
} while(0)

/* MULTIPROP_VAR() and friends automate the process of "keep the first error,
   drop secondary errors", for an arbitrary number of variables.  If the base
   derr_t (the first one) is an error, all the __VAR_ARGS__ derr_t's are simply
   dropped, with no effect on the trace.  Otherwise, if any of the __VAR_ARGS__
   derr_t's are found to be errors, the first one is PROP_VAR'd and the
   remaining ones are silently dropped.

   This behavior is specifically focused on simplifying the error handling
   during callbacks in the advance state pattern; the primary state most likely
   has an output error which may already be set.  If it is, the callback error
   should be dropped.  If it's not, the callback error should be captured.
   Then after resolving which error to keep, action needs to be taken based on
   the final error state.  Example:

    static void thing_done_cb(void *data, derr_t err){
        main_obj_t *main_obj = data;

        MULTIPROP_VAR_GO(&main_obj->err, done, &err);

        // take success-specific actions here

    done:
        schedule(main_obj);
    }
*/

// append code's trace to e and return true if code is an error.
bool pvt_multiprop_var(
    derr_t *out,
    const char *file,
    const char *func,
    int line,
    derr_t **errs,
    size_t nerrs
);

#define IF_MULTIPROP_VAR(e, ...) if( \
    pvt_multiprop_var((e), \
        FILE_LOC, \
        (derr_t*[]){__VA_ARGS__}, \
        sizeof((derr_t*[]){__VA_ARGS__}) / sizeof(derr_t*) \
    ) \
)

#define TRACE_MULTIPROP_VAR(e, ...) do { \
    pvt_multiprop_var((e), \
        FILE_LOC, \
        (derr_t*[]){__VA_ARGS__}, \
        sizeof((derr_t*[]){__VA_ARGS__}) / sizeof(derr_t*) \
    ); \
} while(0)

#define MULTIPROP_VAR(e, ...) do { \
    if( \
        pvt_multiprop_var((e), \
            FILE_LOC, \
            (derr_t*[]){__VA_ARGS__}, \
            sizeof((derr_t*[]){__VA_ARGS__}) / sizeof(derr_t*) \
        ) \
    ) return *(e); \
} while(0)

#define MULTIPROP_VAR_GO(e, _label, ...) do { \
    if( \
        pvt_multiprop_var((e), \
            FILE_LOC, \
            (derr_t*[]){__VA_ARGS__}, \
            sizeof((derr_t*[]){__VA_ARGS__}) / sizeof(derr_t*) \
        ) \
    ) goto _label; \
} while(0)
