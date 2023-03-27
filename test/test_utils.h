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
    opt_spec_t o_logger = {'l', "logger", true}; \
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

// for multiline dstr, zoom in on the first difference
static inline void firstdiff(
    const dstr_t *a, const dstr_t *b, dstr_t *aout, dstr_t *bout, dstr_t *caret
){
    // find the index of the first difference
    // note, if we never find a difference it's probably due to lengths
    size_t i = 0;
    for(; i < a->len && i < b->len; i++){
        if(a->data[i] != b->data[i]) break;
    }
    size_t backtrack = MIN(i, 20);
    *aout = dstr_sub2(*a, i - backtrack, i - backtrack + 80);
    *bout = dstr_sub2(*b, i - backtrack, i - backtrack + 80);
    // measure the print length of the common string
    dstr_t common = dstr_sub2(*a, i-backtrack, i);
    DSTR_VAR(buf, 20*4);
    FMT_QUIET(&buf, "%x", FD_DBG(&common));
    DSTR_STATIC(
        spaces_caret,
        "          ""          ""          ""          "
        "          ""          ""          ""          " "^"
    );
    // keep as many spaces as the print length of common
    size_t start = spaces_caret.len - 1 - MIN(spaces_caret.len - 1, buf.len);
    *caret = dstr_sub2(spaces_caret, start, SIZE_MAX);
}

// mkdir_temp creates a uniquely named temporary directory in PWD
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

#define _EXPECT_NUM(type, f, e, name, got, op, exp) do { \
    type _got = (got); \
    type _exp = (exp); \
    if(!(_got op _exp)){ \
        ORIG(e, \
            E_VALUE, \
            "expected %x " #op " %x but got %x", \
            FS(name), f(_exp), f(_got) \
        ); \
    } \
} while(0)

#define _EXPECT_NUM_GO(type, f, e, name, got, op, exp, label) do { \
    type _got = (got); \
    type _exp = (exp); \
    if(!(_got op _exp)){ \
        ORIG_GO(e, \
            E_VALUE, \
            "expected %x " #op " %x but got %x", \
            label, \
            FS(name), f(_exp), f(_got) \
        ); \
    } \
} while(0)

#define _EXPECT_U(e, n, g, o, x) \
    _EXPECT_NUM(uintmax_t, FU, e, n, g, o, x)
#define _EXPECT_U_GO(e, n, g, o, x, l) \
    _EXPECT_NUM_GO(uintmax_t, FU, e, n, g, o, x, l)

#define _EXPECT_I(e, n, g, o, x) \
    _EXPECT_NUM(intmax_t, FI, e, n, g, o, x)
#define _EXPECT_I_GO(e, n, g, o, x, l) \
    _EXPECT_NUM_GO(intmax_t, FI, e, n, g, o, x, l)

#define EXPECT_U(e, name, got, exp) _EXPECT_U(e, name, got, ==, exp)
#define EXPECT_U_GT(e, name, got, exp) _EXPECT_U(e, name, got, >, exp)
#define EXPECT_U_GE(e, name, got, exp) _EXPECT_U(e, name, got, >=, exp)
#define EXPECT_U_LT(e, name, got, exp) _EXPECT_U(e, name, got, <, exp)
#define EXPECT_U_LE(e, name, got, exp) _EXPECT_U(e, name, got, <=, exp)
#define EXPECT_U_GO(e, name, got, exp, label) _EXPECT_U_GO(e, name, got, ==, exp, label)
#define EXPECT_U_GT_GO(e, name, got, exp, label) _EXPECT_U_GO(e, name, got, >, exp, label)
#define EXPECT_U_GE_GO(e, name, got, exp, label) _EXPECT_U_GO(e, name, got, >=, exp, label)
#define EXPECT_U_LT_GO(e, name, got, exp, label) _EXPECT_U_GO(e, name, got, <, exp, label)
#define EXPECT_U_LE_GO(e, name, got, exp, label) _EXPECT_U_GO(e, name, got, <=, exp, label)

#define EXPECT_I(e, name, got, exp) _EXPECT_I(e, name, got, ==, exp)
#define EXPECT_I_GT(e, name, got, exp) _EXPECT_I(e, name, got, >, exp)
#define EXPECT_I_GE(e, name, got, exp) _EXPECT_I(e, name, got, >=, exp)
#define EXPECT_I_LT(e, name, got, exp) _EXPECT_I(e, name, got, <, exp)
#define EXPECT_I_LE(e, name, got, exp) _EXPECT_I(e, name, got, <=, exp)
#define EXPECT_I_GO(e, name, got, exp, label) _EXPECT_I_GO(e, name, got, ==, exp, label)
#define EXPECT_I_GT_GO(e, name, got, exp, label) _EXPECT_I_GO(e, name, got, >, exp, label)
#define EXPECT_I_GE_GO(e, name, got, exp, label) _EXPECT_I_GO(e, name, got, >=, exp, label)
#define EXPECT_I_LT_GO(e, name, got, exp, label) _EXPECT_I_GO(e, name, got, <, exp, label)
#define EXPECT_I_LE_GO(e, name, got, exp, label) _EXPECT_I_GO(e, name, got, <=, exp, label)

#define EXPECT_F(e, name, got, exp, eps) do { \
    long double _got = (got); \
    long double _exp = (exp); \
    long double _eps = (eps); \
    long double _diff = _got - _exp; \
    if(ABS(_diff) > _eps){ \
        ORIG(e, \
            E_VALUE, \
            "expected %x == %x but got %x", \
            FS(name), FF(_exp),  FF(_got) \
        ); \
    } \
} while(0)

#define EXPECT_F_GO(e, name, got, exp, eps, label) do { \
    long double _got = (got); \
    long double _exp = (exp); \
    long double _eps = (eps); \
    long double _diff = _got - _exp; \
    if(ABS(_diff) > _eps){ \
        ORIG_GO(e, \
            E_VALUE, \
            "expected %x == %x but got %x", \
            label, \
            FS(name), FF(_exp),  FF(_got) \
        ); \
    } \
} while(0)

#define EXPECT_B(e, name, got, exp) do { \
    bool _got = (got); \
    bool _exp = (exp); \
    if(_got != _exp){ \
        ORIG(e, \
            E_VALUE, \
            "expected %x == %x but got %x", \
            FS(name), FB(_exp),  FB(_got) \
        ); \
    } \
} while(0)

#define EXPECT_B_GO(e, name, got, exp, label) do { \
    bool _got = (got); \
    bool _exp = (exp); \
    if(_got != _exp){ \
        ORIG_GO(e, \
            E_VALUE, \
            "expected %x == %x but got %x", \
            label, \
            FS(name), FB(_exp),  FB(_got) \
        ); \
    } \
} while(0)

#define EXPECT_S(e, name, got, exp) do { \
    const char *_got = (got); \
    const char *_exp = (exp); \
    if(!_got != !_exp || (_got && strcmp(_got, _exp) != 0)){ \
        ORIG(e, \
            E_VALUE, \
            "expected %x == \"%x\" but got \"%x\"", \
            FS(name), FS(_exp), FS(_got) \
        ); \
    } \
} while (0)

#define EXPECT_S_GO(e, name, got, exp, label) do { \
    const char *_got = (got); \
    const char *_exp = (exp); \
    if(!_got != !_exp || (_got && strcmp(_got, _exp) != 0)){ \
        ORIG_GO(e, \
            E_VALUE, \
            "expected %x == \"%x but got \"%x\"", \
            label, \
            FS(name), FS(_exp), FS(_got) \
        ); \
    } \
} while (0)

#define _EXPECT_LIST_LENGTH(e, name, list, exp, action) do { \
    link_t *_list = (list); \
    size_t _exp = (exp); \
    link_t *_ptr = _list->next ? _list->next : _list; \
    size_t _got = 0; \
    while(_ptr != _list){ _got++; _ptr = _ptr->next; } \
    if(_got == _exp) break; \
    TRACE_ORIG((e), \
        E_VALUE, \
        "expected len(%x) == %x but got %x", \
        FS(name), FU(_exp), FU(_got) \
    ); \
    action; \
} while(0)

#define EXPECT_LIST_LENGTH(e, name, list, exp) \
    _EXPECT_LIST_LENGTH(e, name, list, exp, return *(e)) \

#define EXPECT_LIST_LENGTH_GO(e, name, list, exp, label) \
    _EXPECT_LIST_LENGTH(e, name, list, exp, goto label) \

#define _EXPECT_LIST_EMPTY(e, name, list, action) do { \
    link_t *_list = (got); \
    if(link_list_isempty(_list)) break; \
    link_t *_ptr = _list->next; \
    size_t _len = 0; \
    while(_ptr != _list){ _len++; _ptr = _ptr->next; } \
    TRACE_ORIG((e), \
        E_VALUE, \
        "expected %x to be empty but found %x items", \
        FS(name), FU(_len) \
    ); \
    action; \
} while(0)

// single line output for short strings
#define EXPECT_D(e, name, got, exp) do { \
    const dstr_t *_got = (got); \
    const dstr_t *_exp = (exp); \
    if(dstr_cmp(_got, _exp) != 0){ \
        ORIG(e, \
            E_VALUE, \
            "expected %x == %x but got %x", \
            FS(name), FD_DBG(_exp), FD_DBG(_got) \
        ); \
    } \
} while (0)

#define EXPECT_D_GO(e, name, got, exp, label) do { \
    const dstr_t *_got = (got); \
    const dstr_t *_exp = (exp); \
    if(dstr_cmp(_got, _exp) != 0){ \
        ORIG_GO(e, \
            E_VALUE, \
            "expected %x == %x but got %x", \
            label, \
            FS(name), FD_DBG(_exp), FD_DBG(_got) \
        ); \
    } \
} while (0)

// triple line output for mid-length strings
#define EXPECT_D3(e, name, got, exp) do { \
    const dstr_t *_got = (got); \
    const dstr_t *_exp = (exp); \
    if(dstr_cmp(_got, _exp) != 0){ \
        ORIG(e, \
            E_VALUE, \
            "for value '%x'\n" \
            "expected: %x\n" \
            "but got:  %x", \
            FS(name), FD_DBG(_exp), FD_DBG(_got) \
        ); \
    } \
} while (0)

#define EXPECT_D3_GO(e, name, got, exp, label) do { \
    const dstr_t *_got = (got); \
    const dstr_t *_exp = (exp); \
    if(dstr_cmp(_got, _exp) != 0){ \
        ORIG_GO(e, \
            E_VALUE, \
            "for value '%x'\n" \
            "expected: %x\n" \
            "but got:  %x", \
            label, \
            FS(name), FD_DBG(_exp), FD_DBG(_got) \
        ); \
    } \
} while (0)

// multiline string support
#define EXPECT_DM(e, name, got, exp) do { \
    const dstr_t *_got = (got); \
    const dstr_t *_exp = (exp); \
    if(dstr_cmp(_got, _exp) != 0){ \
        char *_line_end = "\n";  \
        if(dstr_endswith(_got, &DSTR_LIT("\n"))) \
            if(dstr_endswith(_exp, &DSTR_LIT("\n"))) \
                _line_end = ""; \
        dstr_t zoom_exp, zoom_got, caret; \
        firstdiff(_exp, _got, &zoom_exp, &zoom_got, &caret); \
        ORIG(e, \
            E_VALUE, \
            "-- for value '%x', expected:\n" \
            "%x%x" \
            "-- but got:\n" \
            "%x%x-- (end) --" \
            "-- first difference: --\n" \
            "expected: \"%x\"\n" \
            "but got:  \"%x\"\n" \
            "           %x\n", \
            FS(name), FD(_exp), FS(_line_end), FD(_got), FS(_line_end), \
            FD_DBG(&zoom_exp), FD_DBG(&zoom_got), FD(&caret) \
        ); \
    } \
} while (0)

#define EXPECT_DM_GO(e, name, got, exp, label) do { \
    const dstr_t *_got = (got); \
    const dstr_t *_exp = (exp); \
    if(dstr_cmp(_got, _exp) != 0){ \
        char *_line_end = "\n";  \
        if(dstr_endswith(_got, &DSTR_LIT("\n"))) \
            if(dstr_endswith(_exp, &DSTR_LIT("\n"))) \
                _line_end = ""; \
        dstr_t zoom_exp, zoom_got, caret; \
        firstdiff(_exp, _got, &zoom_exp, &zoom_got, &caret); \
        ORIG_GO(e, \
            E_VALUE, \
            "-- for value '%x', expected:\n" \
            "%x%x" \
            "-- but got:\n" \
            "%x%x-- (end) --\n" \
            "-- first difference: --\n" \
            "expected: \"%x\"\n" \
            "but got:  \"%x\"\n" \
            "           %x\n", \
            label, \
            FS(name), FD(_exp), FS(_line_end), FD(_got), FS(_line_end), \
            FD_DBG(&zoom_exp), FD_DBG(&zoom_got), FD(&caret) \
        ); \
    } \
} while (0)

#define EXPECT_NULL(e, name, got) do { \
    const void *_got = (got); \
    if(_got != NULL){ \
        ORIG(e, \
            E_VALUE, \
            "expected %x == NULL but got %x", \
            FS(name), \
            FP(_got) \
        ); \
    } \
} while (0)

#define EXPECT_NULL_GO(e, name, got, label) do { \
    const void *_got = (got); \
    if(_got != NULL){ \
        ORIG_GO(e, \
            E_VALUE, \
            "expected %x == NULL but got %x", \
            label, \
            FS(name), \
            FP(_got) \
        ); \
    } \
} while (0)

#define EXPECT_NOT_NULL(e, name, got) do { \
    const void *_got = (got); \
    if(_got == NULL){ \
        ORIG(e, \
            E_VALUE, \
            "expected %x != NULL", \
            FS(name) \
        ); \
    } \
} while (0)

#define EXPECT_NOT_NULL_GO(e, name, got, label) do { \
    const void *_got = (got); \
    if(_got == NULL){ \
        ORIG_GO(e, \
            E_VALUE, \
            "expected %x != NULL", \
            label, \
            FS(name) \
        ); \
    } \
} while (0)

#define EXPECT_P(e, name, got, exp) do { \
    const void *_got = (got); \
    const void *_exp = (exp); \
    if(_got != _exp){ \
        ORIG(e, \
            E_VALUE, \
            "expected %x == %x but got %x", \
            FS(name), FP(_exp),  FP(_got) \
        ); \
    } \
} while(0)

#define EXPECT_P_GO(e, name, got, exp, label) do { \
    const void *_got = (got); \
    const void *_exp = (exp); \
    if(_got != _exp){ \
        ORIG_GO(e, \
            E_VALUE, \
            "expected %x == %x but got %x", \
            label, \
            FS(name), FP(_exp),  FP(_got) \
        ); \
    } \
} while(0)

#define EXPECT_E_VAR(e, name, got, exp) do { \
    derr_t *_got = (got); \
    const derr_type_t _exp = (exp); \
    if(_got->type == _exp){ \
        DROP_VAR(_got); \
    } else if(is_error(*_got)) { \
        PROP_VAR((e), _got); \
    }else{ \
        ORIG((e), E_VALUE, "expected an error"); \
    } \
} while(0)

#define EXPECT_E_VAR_GO(e, name, got, exp, label) do { \
    derr_t *_got = (got); \
    const derr_type_t _exp = (exp); \
    if(_got->type == _exp){ \
        DROP_VAR(_got); \
    } else if(is_error(*_got)) { \
        PROP_VAR_GO((e), _got, label); \
    }else{ \
        ORIG_GO((e), E_VALUE, "expected an error", label); \
    } \
} while(0)

#endif // TEST_UTILS_H
