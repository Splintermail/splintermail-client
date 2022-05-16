#ifndef TEST_UTILS_H
#define TEST_UTILS_H

#include <string.h>

#include <libdstr/libdstr.h>

// if a test ever needs extra CLI options a VA_ARG could be added to this macro
// if you need to get the test_files_path, that arg should be type const char**
#define PARSE_TEST_OPTIONS(argc, argv, test_files_path, default_lvl) do { \
    log_level_t lvl = default_lvl; \
    const char** tfp = test_files_path; \
    /* get logger level */ \
    opt_spec_t o_logger = {'l', "logger", true, OPT_RETURN_INIT}; \
    opt_spec_t* spec[] = {&o_logger}; \
    size_t speclen = sizeof(spec) / sizeof(*spec); \
    int newargc; \
    /* decide to print usage or continue */ \
    bool args_valid = true; \
    /* parse options */ \
    derr_t e2 = opt_parse(argc, argv, spec, speclen, &newargc); \
    CATCH(e2, E_ANY){ \
        args_valid = false; \
        DROP_VAR(&e2); \
        goto print_usage; \
    } \
    /* set log level */ \
    DSTR_STATIC(dstr_debug, "debug"); \
    DSTR_STATIC(dstr_warn, "warn"); \
    DSTR_STATIC(dstr_info, "info"); \
    DSTR_STATIC(dstr_error, "error"); \
    if(o_logger.found){ \
        if(dstr_cmp(&dstr_debug, &o_logger.val) == 0){ \
            lvl = LOG_LVL_DEBUG; \
        }else if(dstr_cmp(&dstr_info, &o_logger.val) == 0){ \
            lvl = LOG_LVL_INFO; \
        }else if(dstr_cmp(&dstr_warn, &o_logger.val) == 0){ \
            lvl = LOG_LVL_WARN; \
        }else if(dstr_cmp(&dstr_error, &o_logger.val) == 0){ \
            lvl = LOG_LVL_ERROR; \
        }else{ \
            args_valid = false; \
        } \
    } \
    if(tfp != NULL){ \
        if(newargc < 2){ \
            args_valid = false; \
        }else{ \
            *tfp = argv[1]; \
        } \
    } \
print_usage: \
    if(args_valid == false){ \
        char* display = tfp != NULL ? "TEST_FILES ": ""; \
        printf("usage `%s %s [(-l|--logger) LOG_LEVEL]`\n" \
               "\n" \
               "where LOG_LEVEL is one of:\n" \
               "    debug\n" \
               "    info\n" \
               "    warn\n" \
               "    error\n", argv[0], display); \
        return 3; \
    } \
    /* add logger */ \
    logger_add_fileptr(lvl, stdout); \
} while(0)

derr_t file_cmp(
    const char* fa, const char* fb, bool normalize_line_ends, int* result
);
derr_t file_cmp_dstr(
    const char* fa, const dstr_t* b, bool normalize_line_ends, int* result
);

// makedir_temp creates a uniquely named temporary directory in PWD
derr_t mkdir_temp(const char *prefix, dstr_t *path);

// these are really only used in tests, so right now they are not in common.h:
#define LIST_STATIC_VAR(type, name, num_items) \
    static type name ## _buffer[num_items]; \
    static LIST(type) name = {name ## _buffer, \
                              sizeof(name ## _buffer), \
                              0, \
                              true}
#define DSTR_STATIC_VAR(name, size) \
    static char name ## _buffer[size]; \
    static dstr_t name = { name ## _buffer, size, 0, 1 }

// EXPECT helpers

#define EXPECT_U(e, name, got, exp) do { \
    uintmax_t _got = (got); \
    uintmax_t _exp = (exp); \
    if(_got != (uintmax_t)_exp){ \
        TRACE(e, \
            "expected %x == %x but got %x\n", FS(name), FU(_exp),  FU(_got) \
        ); \
        ORIG(e, E_VALUE, "wrong value"); \
    } \
} while(0)

#define EXPECT_U_GO(e, name, got, exp, label) do { \
    uintmax_t _got = (got); \
    uintmax_t _exp = (exp); \
    if(_got != (uintmax_t)_exp){ \
        TRACE(e, \
            "expected %x == %x but got %x\n", FS(name), FU(_exp),  FU(_got) \
        ); \
        ORIG_GO(e, E_VALUE, "wrong value", label); \
    } \
} while(0)

#define EXPECT_I(e, name, got, exp) do { \
    intmax_t _got = (got); \
    intmax_t _exp = (exp); \
    if(_got != _exp){ \
        TRACE(e, \
            "expected %x == %x but got %x\n", FS(name), FI(_exp),  FI(_got) \
        ); \
        ORIG(e, E_VALUE, "wrong value"); \
    } \
} while(0)

#define EXPECT_I_GO(e, name, got, exp, label) do { \
    intmax_t _got = (got); \
    intmax_t _exp = (exp); \
    if(_got != _exp){ \
        TRACE(e, \
            "expected %x == %x but got %x\n", FS(name), FI(_exp),  FI(_got) \
        ); \
        ORIG_GO(e, E_VALUE, "wrong value", label); \
    } \
} while(0)

#define EXPECT_B(e, name, got, exp) do { \
    bool _got = (got); \
    bool _exp = (exp); \
    if(_got != _exp){ \
        TRACE(e, \
            "expected %x == %x but got %x\n", FS(name), FB(_exp),  FB(_got) \
        ); \
        ORIG(e, E_VALUE, "wrong value"); \
    } \
} while(0)

#define EXPECT_B_GO(e, name, got, exp, label) do { \
    bool _got = (got); \
    bool _exp = (exp); \
    if(_got != _exp){ \
        TRACE(e, \
            "expected %x == %x but got %x\n", FS(name), FB(_exp),  FB(_got) \
        ); \
        ORIG_GO(e, E_VALUE, "wrong value", label); \
    } \
} while(0)

// single line output for short strings
#define EXPECT_D(e, name, got, exp) do { \
    dstr_t *_got = (got); \
    dstr_t *_exp = (exp); \
    if(dstr_cmp(_got, _exp) != 0){ \
        TRACE(e, \
            "expected %x == %x but got %x\n", \
            FS(name), FD_DBG(_exp), FD_DBG(_got) \
        ); \
        ORIG(e, E_VALUE, "wrong value"); \
    } \
} while (0)

#define EXPECT_D_GO(e, name, got, exp, label) do { \
    dstr_t *_got = (got); \
    dstr_t *_exp = (exp); \
    if(dstr_cmp(_got, _exp) != 0){ \
        TRACE(e, \
            "expected %x == %x but got %x\n", \
            FS(name), FD_DBG(_exp), FD_DBG(_got) \
        ); \
        ORIG_GO(e, E_VALUE, "wrong value", label); \
    } \
} while (0)

// triple line output for mid-length strings
#define EXPECT_D3(e, name, got, exp) do { \
    dstr_t *_got = (got); \
    dstr_t *_exp = (exp); \
    if(dstr_cmp(_got, _exp) != 0){ \
        TRACE(e, \
            "for value '%x'\n" \
            "expected: %x\n" \
            "but got:  %x\n", \
            FS(name), FD_DBG(_exp), FD_DBG(_got) \
        ); \
        ORIG(e, E_VALUE, "wrong value"); \
    } \
} while (0)

#define EXPECT_D3_GO(e, name, got, exp, label) do { \
    dstr_t *_got = (got); \
    dstr_t *_exp = (exp); \
    if(dstr_cmp(_got, _exp) != 0){ \
        TRACE(e, \
            "for value '%x'\n" \
            "expected: %x\n" \
            "but got:  %x\n", \
            FS(name), FD_DBG(_exp), FD_DBG(_got) \
        ); \
        ORIG_GO(e, E_VALUE, "wrong value", label); \
    } \
} while (0)

// multiline string support
#define EXPECT_DM(e, name, got, exp) do { \
    dstr_t *_got = (got); \
    dstr_t *_exp = (exp); \
    if(dstr_cmp(_got, _exp) != 0){ \
        char *_line_end = "\n";  \
        if(dstr_endswith(_got, &DSTR_LIT("\n"))) \
            if(dstr_endswith(_exp, &DSTR_LIT("\n"))) \
                _line_end = ""; \
        TRACE(e, \
            "-- for value '%x', expected:\n" \
            "%x%x" \
            "-- but got:\n" \
            "%x%x", \
            FS(name), FD(_exp), FS(_line_end), FD(_got), FS(_line_end) \
        ); \
        ORIG(e, E_VALUE, "wrong value"); \
    } \
} while (0)

#define EXPECT_DM_GO(e, name, got, exp, label) do { \
    dstr_t *_got = (got); \
    dstr_t *_exp = (exp); \
    if(dstr_cmp(_got, _exp) != 0){ \
        char *_line_end = "\n";  \
        if(dstr_endswith(_got, &DSTR_LIT("\n"))) \
            if(dstr_endswith(_exp, &DSTR_LIT("\n"))) \
                _line_end = ""; \
        TRACE(e, \
            "-- for value '%x', expected:\n" \
            "%x%x" \
            "-- but got:\n" \
            "%x%x", \
            FS(name), FD(_exp), FS(_line_end), FD(_got), FS(_line_end) \
        ); \
        ORIG_GO(e, E_VALUE, "wrong value", label); \
    } \
} while (0)

#endif // TEST_UTILS_H
