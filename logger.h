#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include "common.h"

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

static inline void drop(derr_t *error){
    error->type = E_NONE;
    dstr_free(&error->msg);
}

#define DROP(e) drop(&(e))

#define DUMP(e) LOG_ERROR("error trace:\n%x", FD(&(e).msg))

// writing to the trace is usually best-effort and the error will be ignored.
derr_type_t pvt_trace_quiet(
        derr_t *e, const char* fstr, const fmt_t *args, size_t nargs);

#define TRACE(e, fstr, ...) \
    pvt_trace_quiet(&(e), fstr, (const fmt_t[]){FI(1), __VA_ARGS__}, \
            sizeof((const fmt_t[]){FI(1), __VA_ARGS__}) / sizeof(fmt_t))

// just like TRACE but works on a pointer to an error
#define TRACE2(e, fstr, ...) \
    pvt_trace_quiet((e), fstr, (const fmt_t[]){FI(1), __VA_ARGS__}, \
            sizeof((const fmt_t[]){FI(1), __VA_ARGS__}) / sizeof(fmt_t))

// err_ptr can't be null, due to the behavior of macros
#define TRACE_ORIG(e, code, message){ \
    (e).type = (code); \
    TRACE((e), \
        "ERROR: %x\n" \
        "originating %x from file %x: %x(), line %x\n", \
        FS(message), FD(error_to_dstr(code)), FS(FILE_BASENAME), \
        FS(__func__), FI(__LINE__) \
    ); \
}

#define TRACE_PROP(e) \
        TRACE((e), \
            "propagating %x from file %x: %x(), line %x\n",\
            FD(error_to_dstr((e).type)), FS(FILE_BASENAME), \
            FS(__func__), FI(__LINE__) \
        )

#define ORIG(e, _code, _message) { \
    TRACE_ORIG((e), (_code), (_message)); \
    return (e); \
}

// command can also be just a raw error code.
#define PROP(e, _command) { \
    /* can't DROP e here because sometimes e and _command are the same */ \
    (e) = (_command); \
    if((e).type){ \
        TRACE_PROP(e); \
        return (e); \
    } \
}

#define TRACE_CATCH(e) \
    TRACE((e), \
        "catching %x in file %x: %x(), line %x\n", \
        FD(error_to_dstr((e).type)), FS(FILE_BASENAME), \
        FS(__func__), FI(__LINE__))

// to use the below macros, declare "derr_t error" beforehand

#define CATCH(e, _error_mask) \
    if(((e).type & (_error_mask)) && (TRACE_CATCH(e) || true))

#define ORIG_GO(e, _code, _message, _label) { \
    TRACE_ORIG((e), (_code), (_message)); \
    goto _label; \
}

#define PROP_GO(e, _command, _label) {\
    /* can't DROP e here because sometimes e and _command are the same */ \
    (e) = (_command); \
    if((e).type){ \
        TRACE_PROP(e); \
        goto _label; \
    } \
}

/* RETHROW works like ORIG except it gives context to generic low-level errors
   in situations where we know what they mean.  Example:

    dstr_toi(){
        dstr_t error = dstr_copy(... , ...) // returns E_FIXEDSIZE
        // right now we know E_FIXEDSIZE means a bad parameter,
        // but higher up code might not know
        CATCH(error, E_FIXEDSIZE){
            RETHROW(E_PARAM);
        }
        ...
    }

    Now it is easier to handle errors from dstr_toi() in higher-level code
    because there is only one possible error it can throw.
    */
#define TRACE_RETHROW(e, _newtype) \
    TRACE((e), \
        "rethrowing %x as %x in file %x: %x(), line %x\n", \
        FD(error_to_dstr((e).type)), FD(error_to_dstr(_newtype)), \
        FS(FILE_BASENAME), FS(__func__), FI(__LINE__))

#define RETHROW(e, _newtype) {\
    TRACE_RETHROW((e), (_newtype)); \
    (e).type = _newtype; \
    return (e); \
}

#define RETHROW_GO(e, _newtype, label) {\
    TRACE_RETHROW((e), (_newtype)); \
    (e).type = (_newtype); \
    goto label; \
}

/* MERGE doesn't do control flow because it is often used when gathering errors
   from multiple threads */
#define MERGE(e, _new, message) { \
    /* _new might be a function call or a variable, so only use it once */ \
    derr_t temp = (_new); \
    /* if e has no error type, just use the new one */ \
    if((e).type == E_NONE){ \
        (e).type = temp.type; \
    } \
    /* find the right way to merge the traces */ \
    if((e).msg.len == 0 ){ \
        if(temp.msg.len > 0){ \
            /* Just append the new trace to the old trace */ \
            TRACE((e), "%x", FD(&temp.msg)); \
        } \
    }else{ \
        /* we have an existing error trace */ \
        if(temp.msg.len > 0){ \
            /* embed the new error trace with a message */ \
            TRACE((e), "merging %x from %x at file %x: %x(), line %x\n%x", \
                FD(error_to_dstr(temp.type)), FS(message), FS(FILE_BASENAME), \
                FS(__func__), FI(__LINE__), FD(&temp.msg)); \
        } \
    } \
    /* done with temp */ \
    DROP(temp); \
}

// MERGE, then set _new to E_OK. `_new` should be a variable name.
#define MERGE_VAR(e, _new, message) { \
    MERGE((e), (_new), message) \
    (_new) = E_OK; \
}

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
    TRACE2(orig, "splitting %x at file %x: %x(), line %x\n",
        FD(error_to_dstr(orig->type)), FS(file), FS(func), FI(line));
    // copy type and zero msg
    derr_t out = {.type = orig->type, .msg = (dstr_t){0}};
    // duplicate message
    dstr_append_quiet(&out.msg, &orig->msg);
    return out;
}
#define SPLIT(e) pvt_split(&(e), FILE_BASENAME, __func__, __LINE__)

/* if you pass an error laterally, you need to reset it without freeing the
   message. It could be done manually but if I have any good ideas later for
   what to do here I want these instances to be marked identically. */
#define PASSED(e) (e) = E_OK

/*
Problems I have with this API:

I can't figure out how to cleanly support these two cases:
    // err2 should be set to E_OK:
    MERGE(err1, err2, "note about err2");

    // do_something()'s return should be ignored
    MERGE(err1, do_something(), "some note");

Or these two:
    // e should be set to E_OK
    DROP(e);

    // do_something()'s return is freed then ignored
    DROP( do_something() );

Also, it's not immediately obvious from the API that there's no support for
TRACE() outside of error handling situations.
    // supported
    if(bad == true){
        TRACE(e, "bad is true\n");
        ORIG(e, E_VALUE);
    }

    // supported
    e = do_something();
    CATCH(E_VALUE){
        TRACE(e, "tried something that failed in way it shouldn't have\n");
        RETRHOW(e, E_INTERNAL);
    }else PROP(e, e);

    // suported, but bad
    e = do_something();
    if(e.type){ // <-- redundant error handling is necessary
        TRACE(e, "tried something that failed");
        PROP(e, e);
    }

    // not supported
    TRACE(e, "trying something which might fail");
    PROP(e, do_something() ); // <-- This is a memory leak.

    // also not supported
    e = do_something(); // doens't fail this time
    TRACE(e, "trying something which might fail");
    PROP(e, e);
    // ...
    PROP(e, later_do_something_else() ); // <-- This is a memory leak.

But there should be better support for TRACE, so that, for example, a failed
connection can report a trace of all the connections it tried to make, but the
failed attempts should not be logged under normal circumstances.

This change would require perhaps a special E_TRACE type (which I don't like)
or some sort of IS_HOT(e) test which checks for type or msg and returns true.
That could be used by MERGE.  Also PROP would have to do something similar.

*/

#endif // LOGGER_H
