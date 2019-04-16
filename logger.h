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

// needs to be an inline function to handle NULL pointers for err_ptr
static inline void LOG_ORIG(derr_t *err_ptr, derr_t code, const char *message){
    LOG_ERROR("ERROR: %x\noriginating %x from file %x: %x(), line %x\n",
              FS(message), FD(error_to_dstr(code)), FS(FILE_BASENAME),
              FS(__func__), FI(__LINE__) );
    if(err_ptr) *err_ptr = code;
}

#define LOG_PROP(_error) \
        LOG_ERROR("propagating %x from file %x: %x(), line %x\n",\
                  FD(error_to_dstr(_error)), FS(FILE_BASENAME), \
                  FS(__func__), FI(__LINE__) )

#define ORIG(_code, _message) { \
    LOG_ORIG(NULL, _code, _message); \
    return _code; \
}

// command can also be just a raw error code.
#define PROP(_command) { \
    derr_t _derr = _command; \
    if(_derr){ \
        LOG_PROP(_derr); \
        return _derr; \
    } \
}

#define LOG_CATCH(_error) \
    LOG_ERROR("catching %x in file %x: %x(), line %x\n", \
              FD(error_to_dstr(_error)), FS(FILE_BASENAME), \
              FS(__func__), FI(__LINE__))

// to use the below macros, declare "derr_t error" beforehand

// since LOG_ERROR always returns 0 this works nicely, even with else statements
#define CATCH(_error_mask) \
    if((error & (_error_mask)) && (LOG_CATCH(error)==0))

#define ORIG_GO(_code, _message, _label) { \
    LOG_ORIG(&error, _code, _message); \
    goto _label; \
}

#define PROP_GO(_command, _label) {\
    error = _command; \
    if(error){ \
        LOG_PROP(error); \
        goto _label; \
    } \
}

/* RETHROW works like ORIG except it gives context to generic low-level errors
   in situations where we know what they mean.  Example:

    dstr_toi(){
        dstr_t error = dstr_copy(... , ...) // returns E_FIXEDSIZE
        // right now we know E_FIXEDSIZE means a bad parameter,
        // but higher up code might not know
        CATCH(E_FIXEDSIZE){
            RETHROW(E_PARAM);
        }
        ...
    }

    Now it is easier to handle errors from dstr_toi() in higher-level code
    because there is only one possible error it can throw.
    */
#define LOG_RETHROW(_newerror) \
    LOG_ERROR("rethrowing %x as %x in file %x: %x(), line %x\n", \
              FD(error_to_dstr(error)), FD(error_to_dstr(_newerror)), \
              FS(FILE_BASENAME), FS(__func__), FI(__LINE__))

#define RETHROW(_newerror) {\
    LOG_RETHROW(_newerror); \
    return _newerror; \
}

#define RETHROW_GO(_newerror, label) {\
    LOG_RETHROW(_newerror); \
    error = _newerror; \
    goto label; \
}

#endif // LOGGER_H
