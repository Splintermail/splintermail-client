typedef enum {
    LOG_LVL_DEBUG,
    LOG_LVL_INFO,
    LOG_LVL_WARN,
    LOG_LVL_ERROR,
} log_level_t;

derr_t logger_add_fileptr(log_level_t level, FILE* f);
derr_t logger_add_filename(log_level_t level, const char* f);
void logger_clear_outputs(void);
void log_flush(void);
// this ALWAYS return 0, for use in the CATCH macro
int pvt_do_log(log_level_t level, const char* format,
             const fmt_t* args, size_t nargs);
#define LOG_AS(log_level, fstr, ...) \
    pvt_do_log(log_level, fstr, \
             (const fmt_t[]){FI(1), __VA_ARGS__}, \
             sizeof((const fmt_t[]){FI(1), __VA_ARGS__}) / sizeof(fmt_t))
#define LOG_ERROR(fstr, ...) \
    pvt_do_log(LOG_LVL_ERROR, fstr, \
             (const fmt_t[]){FI(1), __VA_ARGS__}, \
             sizeof((const fmt_t[]){FI(1), __VA_ARGS__}) / sizeof(fmt_t))
#define LOG_WARN(fstr, ...) \
    pvt_do_log(LOG_LVL_WARN, fstr, \
             (const fmt_t[]){FI(1), __VA_ARGS__}, \
             sizeof((const fmt_t[]){FI(1), __VA_ARGS__}) / sizeof(fmt_t))
#define LOG_INFO(fstr, ...) \
    pvt_do_log(LOG_LVL_INFO, fstr, \
             (const fmt_t[]){FI(1), __VA_ARGS__}, \
             sizeof((const fmt_t[]){FI(1), __VA_ARGS__}) / sizeof(fmt_t))
#define LOG_DEBUG(fstr, ...) \
    pvt_do_log(LOG_LVL_DEBUG, fstr, \
             (const fmt_t[]){FI(1), __VA_ARGS__}, \
             sizeof((const fmt_t[]){FI(1), __VA_ARGS__}) / sizeof(fmt_t))

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

static inline void DUMP(derr_t e){
    if(is_error(e)){
        LOG_ERROR("error trace (%x):\n", FD(error_to_dstr(e.type)));
        if(e.msg.len > 0){
            LOG_ERROR("%x", FD(&(e).msg));
        }
    }
}


// TRACE() and friends are best-effort append-to-derr_t.msg functions
static inline derr_type_t pvt_trace_quiet(
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

#define TRACE(e, fstr, ...) \
    pvt_trace_quiet((e), fstr, (const fmt_t[]){FI(1), __VA_ARGS__}, \
            sizeof((const fmt_t[]){FI(1), __VA_ARGS__}) / sizeof(fmt_t))


// ORIG() and friends set the derr_t.type and append to derr_t.msg

#define TRACE_ORIG(e, code, message){ \
    (e)->type = (code); \
    TRACE((e), \
        "ERROR: %x\n" \
        "originating %x from file %x: %x(), line %x\n", \
        FS(message), FD(error_to_dstr(code)), FS(FILE_BASENAME), \
        FS(__func__), FI(__LINE__) \
    ); \
}

#define ORIG(e, _code, _message) { \
    TRACE_ORIG((e), (_code), (_message)); \
    return *(e); \
}

#define ORIG_GO(e, _code, _message, _label) { \
    TRACE_ORIG((e), (_code), (_message)); \
    goto _label; \
}


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

    // always keep the new type (this is different from MERGE)
    e->type = code.type;

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
    if(pvt_prop(e, code, FILE_LOC)){ return *(e); }

#define PROP_GO(e, code, _label) \
    if(pvt_prop(e, code, FILE_LOC)){ goto _label; }

#define TRACE_PROP(e) \
    do { IF_PROP(e, *e){} } while(0)


// PROP_VAR and friends clean up the variable they merge from
static inline bool pvt_prop_var(derr_t *e, derr_t *e2,
        const char *file, const char *func, int line){
    bool ret = pvt_prop(e, *e2, file, func, line);
    *e2 = E_OK;
    return ret;
}

#define IF_PROP_VAR(e, e2) \
    if(pvt_prop_var(e, e2, FILE_LOC))

#define PROP_VAR(e, e2) \
    if(pvt_prop_var(e, e2, FILE_LOC)){ return *(e); }

#define PROP_VAR_GO(e, e2, _label) \
    if(pvt_prop_var(e, e2, FILE_LOC)){ goto _label; }


/* CHECK handles an error that we have ignored until now, useful when
   transitioning from chunks of code which use the bison error handling
   strategy to chunks of code which use normal error handling */
static inline bool pvt_check(derr_t *e,
        const char *file, const char *func, int line){
    if(!is_error(*e)) return false;

    TRACE(e, "Handling %x in file %x: %x(), line %x\n",
        FD(error_to_dstr(e->type)), FS(file), FS(func), FI(line));
    return true;
}

#define CHECK(e) \
    if(pvt_check(e, FILE_LOC)){ return *(e); }

#define CHECK_GO(e, label) \
    if(pvt_check(e, FILE_LOC)){ goto label; }

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
    if(e->msg.data == NULL){
        // if there was no previous trace, just use the new trace
        e->msg = e2->msg;
        // done with the old error, but don't free the trace we are reusing
        *e2 = E_OK;
    }else if(e->msg.data != e2->msg.data){
        // otherwise, combine the traces
        /* TODO: you should clearly dilineate the new trace from the old trace,
                 so that it is clear exactly what is being rethrown */
        dstr_append_quiet(&e->msg, &e2->msg);
        // done with the old error
        DROP_VAR(e2);
    }
    TRACE(e, "rethrowing %x as %x in file %x: %x(), line %x\n",
        FD(error_to_dstr(oldtype)), FD(error_to_dstr(newtype)),
        FS(file), FS(func), FI(line));
}

#define RETHROW(e, _new, _newtype) {\
    pvt_rethrow((e), (_new), (_newtype), FILE_LOC); \
    return *(e); \
}

#define RETHROW_GO(e, _new, _newtype, label) {\
    pvt_rethrow((e), (_new), (_newtype), FILE_LOC); \
    goto label; \
}

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

#define NOFAIL(e, error_mask, cmd) \
    if(pvt_nofail((e), (error_mask), (cmd), FILE_LOC)){ return *(e); }

#define NOFAIL_GO(e, error_mask, cmd, label) \
    if(pvt_nofail((e), (error_mask), (cmd), FILE_LOC)){ goto label; }

/* MERGE and friends don't do control flow because they are only used when
   gathering errors from multiple threads.

   The only difference between MERGE_CMD and MERGE_VAR is that MERGE_VAR takes
   a pointer to a derr_t instead of a derr_t value, so that it set the variable
   to E_OK. */

static inline void pvt_merge_cmd(derr_t *e, derr_t cmd, const char* message,
        const char* file, const char* func, int line){
    // if e has no error type, just use the new one
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
                FS(func), FI(line), FD(&cmd.msg));
        }
        // done with the old msg
        dstr_free(&cmd.msg);
    }
}
#define MERGE_CMD(e, cmd, message) \
    pvt_merge_cmd((e), (cmd), (message), FILE_LOC)


static inline void pvt_merge_var(derr_t *e, derr_t *var, const char* message,
        const char* file, const char* func, int line){
    pvt_merge_cmd(e, *var, message, file, func, line);
    // pvt_merge_cmd will dstr_free() the msg but we need to erase the pointer
    *var = E_OK;
}
#define MERGE_VAR(e, e2, message) \
    pvt_merge_var((e), (e2), (message), FILE_LOC)


/* SPLIT effectively duplicates an error object.  It is used in multi-threaded
   situations where a single error causes two different things to fail, such
   as a session and the entire loop. */
static inline derr_t pvt_split(derr_t *orig, const char *file,
        const char *func, int line){
    // be smart if orig is not even an error
    if(orig->type == E_NONE){
        return E_OK;
    }
    // do TRACE before duplication
    TRACE(orig, "splitting %x at file %x: %x(), line %x\n",
        FD(error_to_dstr(orig->type)), FS(file), FS(func), FI(line));
    // copy type and zero msg
    derr_t out = {.type = orig->type, .msg = (dstr_t){0}};
    // duplicate message (best effort)
    dstr_append_quiet(&out.msg, &orig->msg);
    return out;
}
#define SPLIT(e) pvt_split(&(e), FILE_LOC)

/* BROADCAST duplicates an error object, like SPLIT, except with a different
   implication.  BROADCAST implies a shared reasource failed and all of its
   accessors will receive the same error. */
static inline derr_t pvt_broadcast(derr_t *orig, const char *file,
        const char *func, int line){
    // be smart if orig is not even an error
    if(orig->type == E_NONE){
        return E_OK;
    }
    // do TRACE before duplication
    TRACE(orig, "broadcasting %x at file %x: %x(), line %x\n",
        FD(error_to_dstr(orig->type)), FS(file), FS(func), FI(line));
    // copy type and zero msg
    derr_t out = {.type = orig->type, .msg = (dstr_t){0}};
    // duplicate message (best effort)
    dstr_append_quiet(&out.msg, &orig->msg);
    return out;
}
#define BROADCAST(e) pvt_broadcast(&(e), FILE_LOC)

/* if you pass an error laterally, you need to reset it without freeing the
   message. It could be done manually but if I have any good ideas later for
   what to do here I want these instances to be marked identically. */
#define PASSED(e) (e) = E_OK
