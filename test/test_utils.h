#ifndef TEST_UTILS_H
#define TEST_UTILS_H

#include <string.h>

#include <common.h>
#include <opt_parse.h>

// if a test ever needs extra CLI options a VA_ARG could be added to this macro
// if you need to get the test_files_path, that arg should be type const char**
#define PARSE_TEST_OPTIONS(argc, argv, test_files_path, default_lvl){ \
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
    if(opt_parse(argc, argv, spec, speclen, &newargc)){ \
        args_valid = false; \
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
}

derr_t file_cmp(const char* fa, const char* fb, int* result);
derr_t file_cmp_dstr(const char* fa, const dstr_t* b, int* result);

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

#endif // TEST_UTILS_H
