#ifndef DITM_THREAD_H
#define DITM_THREAD_H

#define HAVE_STRUCT_TIMESPEC
#include <pthread.h>

#include <libdstr/libdstr.h>
#include <libditm/libditm.h>

// path to where the test files can be found
extern const char* g_test_files;

extern unsigned int ditm_thread_pop_port;
extern const char* ditm_path;

void ditm_thread_start_test(void);
derr_t ditm_thread_end_test(void); // returns error if test failed
void ditm_thread_done(void);

derr_t ditm_thread_start(unsigned int pop_port);
derr_t ditm_thread_join(void);

#endif // DITM_THREAD_H
