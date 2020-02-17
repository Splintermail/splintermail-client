#ifndef FAKE_POP_SERVER_H
#define FAKE_POP_SERVER_H

#include <libdstr/common.h>

// path to where the test files can be found
extern const char* g_test_files;

typedef struct{
    LIST(dstr_t) uids;
    LIST(dstr_t) messages;
    LIST(size_t) lengths;
    LIST(bool) deletions;
    pop_server_t ps;
    bool logged_in;
} fake_pop_server_t;

void fps_start_test(void);
derr_t fps_end_test(void); // returns error if test failed
void fps_done(void);

extern char g_username[];
extern char g_password[];
extern unsigned int fps_pop_port;
extern unsigned int fps_ver_maj;
extern unsigned int fps_ver_min;
extern unsigned int fps_ver_bld;

derr_t fake_pop_server_new(fake_pop_server_t* fps, const char** files,
                           size_t nfiles, const char** uids);
void fake_pop_server_free(fake_pop_server_t* fps);

derr_t fake_pop_server_start(fake_pop_server_t* fps);
derr_t fake_pop_server_join(void);

#endif // FAKE_POP_SERVER_H
