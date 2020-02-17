#ifndef FAKE_IMAP_CLIENT
#define FAKE_IMAP_CLIENT

#include <uv.h>

#include <libdstr/common.h>
#include <networking.h>
#include <queue.h>

// path to where the test files can be found
extern const char* g_test_files;

typedef struct {
    const dstr_t *url;
    unsigned short port;
    bool threads_running;
    bool threads_continue;
    uv_thread_t writer; // makes decisions and handles fic_*() calls
    uv_thread_t reader; // just listens to server
    derr_t reader_error;
    derr_t writer_error;
    // the connection
    ssl_context_t ssl;
    connection_t conn;
    // the big, growing response buffer
    dstr_t resp;
    uv_mutex_t resp_mutex;
    uv_cond_t resp_cond; // for fic_get_resp
    // a smaller, reader-owned buffer
    dstr_t resp_chunk;
    // issued but unexecuted commands
    queue_t cmd_q;
} fic_t;

derr_t fic_create(fic_t *fic, const dstr_t *url, unsigned short port);
derr_t fic_destroy(fic_t *fic); // watch out, this returns a value!!

// commands that can be given:
derr_t fic_connect(fic_t *fic);
derr_t fic_disconnect(fic_t *fic);
derr_t fic_command(fic_t *fic, dstr_t command);
derr_t fic_get_resp(fic_t *fic, const dstr_t *tag, dstr_t *response);
/* note that to get unilateral server messages you'd have send a command (maybe
   NOOP) and you would see the uniltaeral server message above the response */

#endif // FAKE_IMAP_CLIENT
