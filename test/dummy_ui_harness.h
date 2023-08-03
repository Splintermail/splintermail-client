#ifndef DUMMY_UI_HARNESS_C
#define DUMMY_UI_HARNESS_C

#include <libdstr/libdstr.h>
#include <libcitm/libcitm.h>
#include <api_client.h>

// Shitty MSVC preprocessor won't let us stack __VA_ARGS__ macros
#define UH_OH(fstr, ...) do { \
    /* register that we recieved an error */ \
    looked_good = false; \
    /* previously was derr_t error = FMT(&reason_log, __VA_ARGS__); */ \
    _fmt_quiet( \
        WD(&reason_log), \
        fstr, \
        &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
        sizeof((const fmt_i*[]){NULL, __VA_ARGS__})/sizeof(fmt_i*) - 1 \
    ); \
} while(0)


// global "everything looked good that run" variable
extern bool looked_good;
extern dstr_t reason_log;

// citm.h
typedef struct {
    size_t nlspecs;
    char *lspecs[8];
    const char *key;
    const char *cert;
    char *remote;
    const char *maildir_root;
    bool indicate_ready;
    derr_t to_return;
} citm_args_t;
extern citm_args_t* citm_args;
extern bool citm_called;

// fileops.h
// a list of folders which will be created
extern char** creatables;
// a list of which users we are going to "find"
extern char** users;

// api_client.h
extern api_token_t* token_to_read;
extern derr_t read_token_error;
extern bool read_token_notok;

struct register_token_args_t {
    char *baseurl;
    char *user;
    char *pass;
    char *creds_path;
    derr_t to_return;
};
extern struct register_token_args_t* register_token_args;
extern bool register_token_called;

struct api_password_args_t {
    char *baseurl;
    char *path;
    char *arg;
    char *user;
    char *pass;
    // return json, will be parsed
    char* json;
    derr_t to_return;
};
extern struct api_password_args_t* api_password_args;
extern bool api_password_called;

struct api_token_args_t {
    char *baseurl;
    char *path;
    char *arg;
    api_token_t token;
    // return json, will be parsed
    char* json;
    derr_t to_return;
};
extern struct api_token_args_t* api_token_args;
extern bool api_token_called;

// console_input.h
extern char** passwords;
extern char** strings;

#endif // DUMMY_UI_HARNESS_C
