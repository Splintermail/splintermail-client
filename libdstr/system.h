// allocate memory and zeroize it.
// dmalloc uses the builder api to avoid upsetting -Wstrict-aliasing
void *dmalloc(derr_t *e, size_t n);
// by far the most common malloc pattern in splintermail
#define DMALLOC_STRUCT_PTR(e, var) dmalloc((e), sizeof(*(var)))

// found can be NULL if an empty string is the same to you as "not present"
dstr_t dgetenv(const dstr_t varname, bool *found);

// no option to not overwrite
derr_t dsetenv(const dstr_t name, const dstr_t value);
derr_t dunsetenv(const dstr_t name);

// sets time to epoch time or raises an error
derr_t dtime(time_t *time);

// get the timezone (offset from UTC in seconds)
derr_t dtimezone(long int *tz);

// localtime_r in unix or localtime_s in windows
derr_t dlocaltime(time_t t, struct tm *tm);

#ifndef _WIN32 // UNIX

#include <pthread.h>

typedef pthread_mutex_t dmutex_t;
typedef pthread_cond_t dcond_t;
typedef pthread_t dthread_t;

#else // WINDOWS

typedef CRITICAL_SECTION dmutex_t;
typedef CONDITION_VARIABLE dcond_t;
typedef struct {
    HANDLE h;
    void *(*fn)(void*);
    void *arg;
    void *result;
    dmutex_t mutex;
    dcond_t cond;
    bool exited;
} dthread_t;

#endif

// mutex
derr_t dmutex_init(dmutex_t *mutex);
void dmutex_free(dmutex_t *mutex);
void dmutex_lock(dmutex_t *mutex);
void dmutex_unlock(dmutex_t *mutex);

// condition variable
derr_t dcond_init(dcond_t *cond);
void dcond_free(dcond_t *cond);
void dcond_wait(dcond_t *cond, dmutex_t *mutex);
void dcond_signal(dcond_t *cond);
void dcond_broadcast(dcond_t *cond);

// thread
derr_t dthread_create(dthread_t *thread, void *(*fn)(void*), void *arg);

// no error handling; return value is result from the thread
void *dthread_join(dthread_t *thread);

#ifndef _WIN32 // UNIX

derr_t dpipe(int *read_fd, int *write_fd);
derr_t ddup2(int oldfd, int newfd);
derr_t dfork(pid_t *pid, bool *child);

extern int devnull;

// NULL for std_in/out/err means "do nothing"
// &devnull means "read/write to /dev/null"
derr_t dfork_ex(
    pid_t *pid,
    bool *child,
    int *std_in,
    int *std_out,
    int *std_err
);

derr_t dkill(pid_t pid, int sig);

// wraps waitpid with retries
derr_t dwaitpid(pid_t pid, int *wstatus, int options);

// simplified api, only returns exit code and the signum that killed it (or -1)
derr_t dwait(pid_t pid, int *exit_code, int *signum);

// useful in child process before dexec()
// e.g. close_extra_fds(3); dexec(...)
derr_t close_extra_fds(int starting_at);

derr_t _make_env(
    char ***out, char *const base[], const dstr_t *vars, size_t nvars
);
#define make_env(out, base, ...) \
    _make_env( \
        (out), \
        (base), \
        &(const dstr_t[]){(const dstr_t){0}, __VA_ARGS__}[1], \
        sizeof((const dstr_t[]){(const dstr_t){0}, __VA_ARGS__}) / sizeof(const dstr_t) - 1 \
    )


// uses execvp under the hood
derr_t _dexec(
    const dstr_t file,
    char *const env[],
    // args will always be prefixed by the filename as the first argument
    const dstr_t *args,
    size_t nargs
);
#define dexec(file, env, ...) \
    _dexec( \
        (file), \
        (env), \
        &(const dstr_t[]){(const dstr_t){0}, __VA_ARGS__}[1], \
        sizeof((const dstr_t[]){(const dstr_t){0}, __VA_ARGS__}) / sizeof(const dstr_t) - 1 \
    )

/* close file descriptors, run a derr_t function (which probably calls dexec),
   and dump an error to stdout if it does */
#define RUN_CHILD_PROC(cmd, close_fds_starting_at) \
    do { \
        derr_t e = E_OK; \
        IF_PROP(&e, close_extra_fds(close_fds_starting_at) ){ \
            _after_child_proc(e); \
        } \
        e = (cmd); \
        _after_child_proc(e); \
    } while (0)

void _after_child_proc(derr_t e);

#endif // UNIX
