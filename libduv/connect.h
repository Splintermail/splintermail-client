struct duv_connect_t;
typedef struct duv_connect_t duv_connect_t;

// tcp will always be connected if e is E_OK, or cleaned up if is_error(e)
/* e will be E_CANCELED if duv_connect_cancel() was called an no other error
   was encountered */
typedef void (*duv_connect_cb)(duv_connect_t*, derr_t e);

// c.data may be set already
// tcp MUST NOT be initialized
// tcp.data MUST NOT be used by caller while duv_connect_t is active
// memory at c/tcp must remain valid until cb is called
// cb will always be called exactly once
derr_t duv_connect(
    uv_loop_t *loop,
    uv_tcp_t *tcp,
    unsigned int tcp_flags,
    duv_connect_t *c,
    duv_connect_cb cb,
    const char *node,
    const char *service,
    const struct addrinfo *hints
);

// it is a noop to cancel after duv_connect_cb is called
void duv_connect_cancel(duv_connect_t *c);

struct duv_connect_t {
    // for the user to use
    void *data;
    // true after duv_connect() and before duv_connect_cb is called
    bool active;
    // true after duv_cancel() and until duv_connect_cb is called
    bool canceling;

    // configuration
    uv_loop_t *loop;
    uv_tcp_t *tcp_handle;
    unsigned int tcp_flags;
    duv_connect_cb cb;
    char *node;
    char *service;

    // state machine
    derr_t e;
    bool done;
    struct {
        // gai begins in an in-flight state
        uv_getaddrinfo_t req;
        bool canceling;
        bool returned;
        int status;
        // memory that needs freeing
        struct addrinfo *res;
        bool done;
        // our current position in the linked list
        struct addrinfo *ptr;
    } gai;
    struct {
        bool open;
        bool closing;
        bool closed;
    } tcp;
    struct {
        uv_connect_t req;
        bool started;
        bool canceling;
        bool returned;
        int status;
    } connect;
};

// test hook
extern void (*_connect_started_hook)(void);
