#ifndef DUMMY_UI_HARNESS_C
#define DUMMY_UI_HARNESS_C

#include <libdstr/libdstr.h>
#include <libcitm/citm.h>
#include <api_client.h>

// Shitty MSVC preprocessor won't let us stack __VA_ARGS__ macros
#define UH_OH(fstr, ...) { \
    /* register that we recieved an error */ \
    looked_good = false; \
    /* previously was derr_t error = FMT(&reason_log, __VA_ARGS__); */ \
    pvt_fmt_quiet(&reason_log, \
                  fstr, \
                  (const fmt_t[]){FI(1), __VA_ARGS__}, \
                  sizeof((const fmt_t[]){FI(1), __VA_ARGS__})/sizeof(fmt_t)); \
}


// global "everything looked good that run" variable
extern bool looked_good;
extern dstr_t reason_log;

// ditm.h
struct ditm_loop_args_t {
    const char* rhost;
    unsigned int rport;
    const char* ditm_dir;
    unsigned int port;
    const char* api_host;
    unsigned int api_port;
    const char* cert;
    const char* key;
    derr_t to_return;
};
// global "right answers"
extern struct ditm_loop_args_t* ditm_loop_args;
extern bool ditm_called;

// fileops.h
// a list of folders which will be created
extern char** creatables;
// a list of which users we are going to "find"
extern char** users;

// api_client.h
extern api_token_t* token_to_read;
extern bool find_token;
extern derr_t read_token_error;

struct register_token_args_t {
    const char* host;
    unsigned int port;
    const dstr_t* user;
    const dstr_t* pass;
    const char* creds_path;
    derr_t to_return;
};
extern struct register_token_args_t* register_token_args;
extern bool register_token_called;

struct api_password_args_t {
    const char* host;
    unsigned int port;
    dstr_t* command;
    dstr_t* arg;
    char* user;
    char* pass;
    int code;
    const char* reason;
    // const char* recv; // not actually read in ui.c, just memory for *json
    char* json;
    derr_t to_return;
};
extern struct api_password_args_t* api_password_args;
extern bool api_password_called;

struct api_token_args_t {
    const char* host;
    unsigned int port;
    dstr_t* command;
    dstr_t* arg;
    api_token_t token;
    int code;
    const char* reason;
    // const char* recv; // not actually read in ui.c, just memory for *json
    char* json;
    derr_t to_return;
};
extern struct api_token_args_t* api_token_args;
extern bool api_token_called;

// console_input.h
extern char** passwords;
extern char** strings;

#endif // DUMMY_UI_HARNESS_C
