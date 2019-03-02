#ifndef FAKE_API_CLIENT_H
#define FAKE_API_CLIENT_H

#include <common.h>

// a way for an external program to kill the server nicely
extern unsigned int fas_api_port;

// path to where the test files can be found
extern const char* g_test_files;

typedef derr_t (*exp_hook_t)(const dstr_t*, const dstr_t*, unsigned int);

derr_t fas_expect_put(const dstr_t* path, const dstr_t* arg,
                      exp_hook_t hook, unsigned int counter);
derr_t fas_expect_get(dstr_t* path, dstr_t* arg,
                      exp_hook_t* hook, unsigned int* counter);
derr_t fas_response_put(int code, const dstr_t* response);
derr_t fas_response_get(int* code, dstr_t* response);

derr_t fas_assert_done(void);

derr_t fas_start(void);
derr_t fas_join(void);

#endif // FAKE_API_CLIENT_H
