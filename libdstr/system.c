#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libdstr.h"
#include "errno.h"

void *dmalloc(derr_t *e, size_t n){
    if(is_error(*e)) return NULL;
    void *out =  malloc(n);
    if(out == NULL) {
        TRACE_ORIG(e, E_NOMEM, "nomem");
    }else{
        memset(out, 0, n);
    }
    return out;
}

dstr_t dgetenv(const dstr_t varname, bool *found){
    for(size_t i = 0; compat_environ[i]; i++){
        // wrap the environ entry
        dstr_t envvar;
        DSTR_WRAP(envvar, compat_environ[i], strlen(compat_environ[i]), true);

        // split on '='
        dstr_t var, value;
        size_t len;
        dstr_split2_soft(envvar, DSTR_LIT("="), &len, &var, &value);

        // validate the split
        if(len != 2){
            LOG_ERROR(
                "ignoring invalid environment variable: %x\n", FD(envvar)
            );
            continue;
        }

        if(dstr_cmp(&var, &varname) == 0){
            if(found) *found = true;
            return value;
        }
    }
    if(found) *found = false;
    return (dstr_t){0};
}

derr_t dsetenv(const dstr_t name, const dstr_t value){
#ifdef _WIN32 // WINDOWS

    /* windows' putenv takes a copy of the string passed in, so in windows we
       use putenv to set environment variables */
    dstr_t heap = {0};

    derr_t e = E_OK;

    DSTR_VAR(stack, 256);
    dstr_t* buf;

    size_t len = name.len + 1 + value.len;
    if(len < stack.size){
        buf = &stack;
    }else{
        PROP_GO(&e, dstr_new(&heap, len), cu);
        buf = &heap;
    }

    PROP_GO(&e, FMT(buf, "%x=%x", FD(name), FD(value)), cu);

    int ret = _putenv(buf->data);
    if(ret){
        TRACE(&e, "putenv(\"%x=...\"): %x\n", FD_DBG(name), FE(errno));
        ORIG_GO(&e, E_OS, "putenv failed", cu);
    }

cu:
    dstr_free(&heap);
    return e;

#else // UNIX

    /* unix's putenv would use the string right in the environ, which would be
       crazy, so in unix we use setenv */
    dstr_t heap_name = {0};
    dstr_t heap_value = {0};

    derr_t e = E_OK;

    DSTR_VAR(stack_name, 256);
    DSTR_VAR(stack_value, 256);

    char *n, *v;

    if(name.size > name.len && name.data[name.len] == '\0'){
        // dstr is null-terminated already
        n = name.data;
    }else if(name.len < stack_name.size){
        // dstr fits on the stack
        PROP_GO(&e, dstr_append(&stack_name, &name), cu);
        PROP_GO(&e, dstr_null_terminate(&stack_name), cu);
        n = stack_name.data;
    }else{
        // dstr goes on the heap
        PROP_GO(&e, dstr_new(&heap_name, name.len + 1), cu);
        PROP_GO(&e, dstr_append(&stack_name, &name), cu);
        PROP_GO(&e, dstr_null_terminate(&stack_name), cu);
        n = heap_name.data;
    }

    if(value.size > value.len && value.data[value.len] == '\0'){
        v = value.data;
    }else if(value.len < stack_value.size){
        PROP_GO(&e, dstr_append(&stack_value, &value), cu);
        PROP_GO(&e, dstr_null_terminate(&stack_value), cu);
        v = stack_value.data;
    }else{
        PROP_GO(&e, dstr_new(&heap_value, value.len + 1), cu);
        PROP_GO(&e, dstr_append(&stack_value, &value), cu);
        PROP_GO(&e, dstr_null_terminate(&stack_value), cu);
        v = heap_value.data;
    }

    // always overwrite
    int ret = setenv(n, v, 1);
    if(ret){
        TRACE(&e, "setenv(%x, ...): %x\n", FD_DBG(name), FE(errno));
        ORIG_GO(&e, E_OS, "setenv failed", cu);
    }

cu:
    dstr_free(&heap_name);
    dstr_free(&heap_value);
    return e;

#endif
}

derr_t dunsetenv(const dstr_t name){
#ifdef _WIN32 // WINDOWS

    // windows uses putenv("varname=") to clear the environment
    dstr_t heap = {0};

    derr_t e = E_OK;

    DSTR_VAR(stack, 256);
    dstr_t* buf;

    size_t len = name.len + 1;
    if(len < stack.size){
        buf = &stack;
    }else{
        PROP_GO(&e, dstr_new(&heap, len), cu);
        buf = &heap;
    }

    PROP_GO(&e, FMT(buf, "%x=", FD(name)), cu);

    int ret = _putenv(buf->data);
    if(ret){
        TRACE(&e, "putenv(\"%x=\"): %x\n", FD_DBG(name), FE(errno));
        ORIG_GO(&e, E_OS, "putenv failed", cu);
    }

cu:
    dstr_free(&heap);
    return e;

#else // UNIX

    dstr_t heap_name = {0};

    derr_t e = E_OK;

    DSTR_VAR(stack_name, 256);

    char *n;

    if(name.size > name.len && name.data[name.len] == '\0'){
        // dstr is null-terminated already
        n = name.data;
    }else if(name.len < stack_name.size){
        // dstr fits on the stack
        PROP_GO(&e, dstr_append(&stack_name, &name), cu);
        PROP_GO(&e, dstr_null_terminate(&stack_name), cu);
        n = stack_name.data;
    }else{
        // dstr goes on the heap
        PROP_GO(&e, dstr_new(&heap_name, name.len + 1), cu);
        PROP_GO(&e, dstr_append(&stack_name, &name), cu);
        PROP_GO(&e, dstr_null_terminate(&stack_name), cu);
        n = heap_name.data;
    }

    // always overwrite
    int ret = unsetenv(n);
    if(ret){
        TRACE(&e, "unsetenv(%x): %x\n", FD_DBG(name), FE(errno));
        ORIG_GO(&e, E_OS, "setenv failed", cu);
    }

cu:
    dstr_free(&heap_name);
    return e;

#endif
}

derr_type_t dtime_quiet(time_t *out){
    errno = 0;
    time_t ret = time(out);
    if(errno || ret == ((time_t)-1)){
        return E_OS;
    }
    return E_NONE;
}

derr_t dtime(time_t *out){
    derr_t e = E_OK;
    derr_type_t etype = dtime_quiet(out);
    if(etype) ORIG(&e, etype, "time(): %x", FE(errno));
    return e;
}

static void _ensure_tzset(void){
    // this is not thread-safe, but tzset() seems to be so this seems ok
    static bool tz_is_set = false;
    if(!tz_is_set){
        tz_is_set = true;
        compat_tzset();
    }
}

derr_t dtimezone(long int *tz){
#ifndef _WIN32 // UNIX
    derr_t e = E_OK;
    _ensure_tzset();
    *tz = timezone;
    return e;
#else // WINDOWS
    derr_t e = E_OK;
    // https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/get-timezone
    int ret = _get_timezone(tz);
    if(ret != 0){
        // I see no docs at all about what error_t means
        TRACE(&e, "_get_timezone: %x\n", FE(errno));
        ORIG(&e, E_OS, "_get_timezone failed");
    }
    return e;
#endif
}

// localtime_r in unix or localtime_s in windows
derr_t dlocaltime(time_t t, struct tm *tm){
#ifndef _WIN32 // UNIX
    derr_t e = E_OK;
    _ensure_tzset();
    struct tm *tret = localtime_r(&t, tm);
    if(tret == NULL){
        TRACE(&e, "localtime_r(%x): %x\n", FI(t), FE(errno));
        ORIG(&e, E_OS, "localtime_r failed");
    }
    return e;
#else
    derr_t e = E_OK;
    _ensure_tzset();
    int ret = localtime_s(tm, &t);
    if(ret){
        TRACE(&e, "localtime_s(%x): %x\n", FI(t), FE(ret));
        ORIG(&e, E_OS, "localtime_s failed");
    }
    return e;
#endif
}

dtm_t dtm_from_tm(struct tm tm){
    return (dtm_t){
        .year = tm.tm_year + 1900,
        .month = tm.tm_mon + 1,
        .day = tm.tm_mday,
        .hour = tm.tm_hour,
        .min = tm.tm_min,
        .sec = tm.tm_sec,
    };
}

struct tm dtm_to_tm(dtm_t dtm){
    return (struct tm){
        .tm_year = dtm.year - 1900,
        .tm_mon = dtm.month - 1,
        .tm_mday = dtm.day,
        .tm_hour = dtm.hour,
        .tm_min = dtm.min,
        .tm_sec = dtm.sec,
    };
}

// read a local time stamp
derr_type_t dmktime_local_quiet(dtm_t dtm, time_t *out){
    struct tm tm = {
        .tm_year = dtm.year - 1900,  // 1900 is year 0
        .tm_mon = dtm.month - 1,     // jan is month 0
        .tm_mday = dtm.day,
        .tm_hour = dtm.hour,
        .tm_min = dtm.min,
        .tm_sec = dtm.sec,
    };
    _ensure_tzset();
    time_t t = mktime(&tm);
    if(t == (time_t)-1){
        *out = 0;
        return E_OS;
    }
    *out = t;
    return E_NONE;
}

derr_t dmktime_local(dtm_t dtm, time_t *out){
    derr_t e = E_OK;
    derr_type_t etype = dmktime_local_quiet(dtm, out);
    if(etype) ORIG(&e, etype, "mktime(): %x", FE(errno));
    return e;
}


// read a utc timestamp
derr_type_t dmktime_utc_quiet(dtm_t dtm, time_t *out){
    time_t local;
    derr_type_t etype = dmktime_local_quiet(dtm, &local);
    if(etype) return etype;
    // calculate seconds offset between localtime and UTC with stdlib functions
    static time_t offset = LONG_MAX;
    if(offset == LONG_MAX){
        // get a gmt timestamp at time=0, then see what localtime we read it as
        time_t zero = 0;
        struct tm utc;
        #ifdef _WIN32
        gmtime_s(&utc, &zero);
        #else
        gmtime_r(&zero, &utc);
        #endif
        offset = mktime(&utc);
    }
    // apply offset
    *out = local - offset;
    return E_NONE;
}

derr_t dmktime_utc(dtm_t dtm, time_t *out){
    derr_t e = E_OK;
    derr_type_t etype = dmktime_utc_quiet(dtm, out);
    if(etype) ORIG(&e, etype, "mktime(): %x", FE(errno));
    return e;
}

#ifndef _WIN32 // UNIX

#include <pthread.h>

derr_t dthread_create(dthread_t *thread, void *(*fn)(void*), void *arg){
    derr_t e = E_OK;

    int ret = pthread_create(thread, NULL, fn, arg);
    if(ret != 0){
        TRACE(&e, "pthread_create: %x\n", FE(errno));
        ORIG(&e, E_OS, "pthread_create failed");
    }

    return e;
}

void *dthread_join(dthread_t *thread){
    void *result;
    pthread_join(*thread, &result);
    return result;
}

derr_t dmutex_init(dmutex_t *mutex){
    derr_t e = E_OK;
    int ret = pthread_mutex_init(mutex, NULL);
    if(ret){
        TRACE(&e, "pthread_mutex_init: %x\n", FE(errno));
        ORIG(&e,
            errno == ENOMEM ? E_NOMEM : E_OS, "pthread_mutex_init failed"
        );
    }
    return e;
}

void dmutex_free(dmutex_t *mutex){
    pthread_mutex_destroy(mutex);
}

void dmutex_lock(dmutex_t *mutex){
    // skip error checks; fancy mutex patterns are not supported
    pthread_mutex_lock(mutex);
}

void dmutex_unlock(dmutex_t *mutex){
    pthread_mutex_unlock(mutex);
}

derr_t dcond_init(dcond_t *cond){
    derr_t e = E_OK;
    int ret = pthread_cond_init(cond, NULL);
    if(ret){
        TRACE(&e, "pthread_cond_init: %x\n", FE(errno));
        ORIG(&e, errno == ENOMEM ? E_NOMEM : E_OS, "pthread_cond_init failed");
    }
    return e;
}

void dcond_free(dcond_t *cond){
    pthread_cond_destroy(cond);
}

void dcond_wait(dcond_t *cond, dmutex_t *mutex){
    pthread_cond_wait(cond, mutex);
}

void dcond_signal(dcond_t *cond){
    pthread_cond_signal(cond);
}

void dcond_broadcast(dcond_t *cond){
    pthread_cond_broadcast(cond);
}

#else // WINDOWS

#include <process.h>
#include <stdio.h>

derr_t dmutex_init(dmutex_t *mutex){
    // yes, this really returns no value
    // docs.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-initializecriticalsection
    InitializeCriticalSection(mutex);
    return E_OK;
}

void dmutex_free(dmutex_t *mutex){
    DeleteCriticalSection(mutex);
}

void dmutex_lock(dmutex_t *mutex){
    EnterCriticalSection(mutex);
}

void dmutex_unlock(dmutex_t *mutex){
    LeaveCriticalSection(mutex);
}

derr_t dcond_init(dcond_t *cond){
    // yes, this really returns no value
    // docs.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-initializeconditionvariable
    InitializeConditionVariable(cond);
    return E_OK;
}

void dcond_free(dcond_t *cond){
    // windows doesn't have any cleanup function
    (void)cond;
}

void dcond_wait(dcond_t *cond, dmutex_t *mutex){
    SleepConditionVariableCS(cond, mutex, INFINITE);
}

void dcond_signal(dcond_t *cond){
    WakeConditionVariable(cond);
}

void dcond_broadcast(dcond_t *cond){
    WakeAllConditionVariable(cond);
}

static void exec_pthread_fn(void *arg){
    dthread_t *thread = arg;
    thread->result = thread->fn(thread->arg) ;

    /* It seems to be a race condition to call WaitForSingleObject on a thread;
       if the thread exits first, the wait hangs.  Presumably this is due to
       _endthread() being called automatically and calling CloseHandle() in
       turn.  But who knows.  Instead we'll just approximate joining. */
    dmutex_lock(&thread->mutex);
    thread->exited = true;
    dcond_broadcast(&thread->cond);
    dmutex_unlock(&thread->mutex);
}

derr_t dthread_create(dthread_t *thread, void *(*fn)(void*), void *arg){
    derr_t e = E_OK;
    *thread = (dthread_t){ .fn = fn, .arg = arg };

    PROP(&e, dmutex_init(&thread->mutex) );
    PROP_GO(&e, dcond_init(&thread->cond), fail_mutex);

    uintptr_t uret = _beginthread(exec_pthread_fn, 0, thread);
    if(uret == (uintptr_t)-1){
        TRACE(&e, "_beginthread: %x\n", FE(errno));
        ORIG_GO(&e, E_OS, "_beginthread failed", fail_cond);
    }
    thread->h = (HANDLE)uret;

    return e;

fail_cond:
    dcond_free(&thread->cond);
fail_mutex:
    dmutex_free(&thread->mutex);
    return e;
}

void *dthread_join(dthread_t *thread){
    // safe against double-joins
    if(!thread->exited){
        // wait for the thread to exit (it cleans itself up)
        dmutex_lock(&thread->mutex);
        while(!thread->exited){
            dcond_wait(&thread->cond, &thread->mutex);
        }
        dmutex_unlock(&thread->mutex);

        // clean up our dthread_t
        thread->h = NULL;
        dcond_free(&thread->cond);
        dmutex_free(&thread->mutex);
    }

    return thread->result;
}

#endif

#ifndef _WIN32 // UNIX-only

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

derr_t dpipe(int *read_fd, int *write_fd){
    derr_t e = E_OK;

    *read_fd = -1;
    *write_fd = -1;

    int pipefd[2];

    int ret = pipe(pipefd);
    if(ret){
        TRACE(&e, "pipe: %x\n", FE(errno));
        ORIG(&e, E_OS, "pipe failed");
    }

    *read_fd = pipefd[0];
    *write_fd = pipefd[1];

    return e;
}

derr_t ddup2(int oldfd, int newfd){
    derr_t e = E_OK;

    int ret = dup2(oldfd, newfd);
    if(ret < 0){
        TRACE(&e, "dup2: %x\n", FE(errno));
        ORIG(&e, E_OS, "dup2 failed");
    }

    return e;
}

derr_t dfork(pid_t *pid, bool *child){
    derr_t e = E_OK;

    *child = false;
    *pid = fork();

    if(*pid < 0){
        TRACE(&e, "fork: %x\n", FE(errno));
        ORIG(&e, E_OS, "fork failed");
    }

    *child = (*pid == 0);

    return e;
}

// this is used for its address, not its value
int devnull = 0;

#define FDCLOSE(var) do { if(var >= 0){ close(var); } var = -1; } while(0)
#define FDSTEAL(var, out) do { *out = var; var = -1; } while(0)

// NULL for std_in/out/err means "do nothing"
// &devnull means "read/write to /dev/null"
derr_t dfork_ex(
    pid_t *pid,
    bool *child,
    int *std_in,
    int *std_out,
    int *std_err
){
    derr_t e = E_OK;

    *pid = -1;
    *child = false;
    if(std_in) *std_in = -1;
    if(std_out) *std_out = -1;
    if(std_err) *std_err = -1;

    int pipe_in_rd = -1;
    int pipe_in_wr = -1;
    int pipe_out_rd = -1;
    int pipe_out_wr = -1;
    int pipe_err_rd = -1;
    int pipe_err_wr = -1;
    int fd = -1;

    // prep the pipes
    if(std_in && std_in != &devnull){
        PROP_GO(&e, dpipe(&pipe_in_rd, &pipe_in_wr), cu);
    }
    if(std_out && std_out != &devnull){
        PROP_GO(&e, dpipe(&pipe_out_rd, &pipe_out_wr), cu);
    }
    if(std_err && std_err != &devnull){
        PROP_GO(&e, dpipe(&pipe_err_rd, &pipe_err_wr), cu);
    }

    PROP_GO(&e, dfork(pid, child), cu);

    if(*child){
        // child does a bunch of dup2'ing
        if(std_in){
            if(std_in == &devnull){
                // ./child < /dev/null
                PROP_GO(&e, dopen("/dev/null", O_RDONLY, 0, &fd), cu_forked);
                PROP_GO(&e, ddup2(fd, 0), cu_forked);
            }else{
                PROP_GO(&e, ddup2(pipe_in_rd, 0), cu_forked);
            }
        }
        if(std_out){
            if(std_out == &devnull){
                // ./child > /dev/null
                PROP_GO(&e, dopen("/dev/null", O_WRONLY, 0, &fd), cu_forked);
                PROP_GO(&e, ddup2(fd, 1), cu_forked);
            }else{
                PROP_GO(&e, ddup2(pipe_out_wr, 1), cu_forked);
            }
        }
        if(std_err){
            if(std_err == &devnull){
                // ./child 2> /dev/null
                PROP_GO(&e, dopen("/dev/null", O_WRONLY, 0, &fd), cu_forked);
                PROP_GO(&e, ddup2(fd, 2), cu_forked);
            }else{
                PROP_GO(&e, ddup2(pipe_err_wr, 2), cu_forked);
            }
        }
    }else{
        // parent just returns the pipe ends
        if(std_in && std_in != &devnull) FDSTEAL(pipe_in_wr, std_in);
        if(std_out && std_out != &devnull) FDSTEAL(pipe_out_rd, std_out);
        if(std_err && std_err != &devnull) FDSTEAL(pipe_err_rd, std_err);
    }

cu_forked:
    // handle child errors
    if(child && is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
        exit(1);
    }

    // there are no possible parent errors after fork succeeds

cu:
    FDCLOSE(pipe_in_rd);
    FDCLOSE(pipe_in_wr);
    FDCLOSE(pipe_out_rd);
    FDCLOSE(pipe_out_wr);
    FDCLOSE(pipe_err_rd);
    FDCLOSE(pipe_err_wr);
    FDCLOSE(fd);

    return e;
}


derr_t dkill(pid_t pid, int sig){
    derr_t e = E_OK;

    int ret = kill(pid, sig);
    if(ret){
        TRACE(&e, "kill(pid=%x, sig=%x): %x", FI(pid), FI(sig), FE(errno));
        ORIG(&e, E_OS, "kill() failed");
    }

    return e;
}

derr_t dwaitpid(pid_t pid, int *wstatus, int options){
    derr_t e = E_OK;

    int ret;
    do {
        ret = waitpid(pid, wstatus, options);
    } while(ret && errno == EINTR);

    if(ret < 0){
        TRACE(&e,
            "wait(pid=%x, options=%x): %x", FI(pid), FI(options), FE(errno)
        );
        ORIG(&e, E_OS, "wait() failed");
    }

    return e;
}

derr_t dwait(pid_t pid, int *exit_code, int *signum){
    derr_t e = E_OK;

    int wstatus;
    PROP(&e, dwaitpid(pid, &wstatus, 0) );
    if(WIFEXITED(wstatus)){
        if(exit_code) *exit_code = WEXITSTATUS(wstatus);
        if(signum) *signum = -1;
    }

    if(WIFSIGNALED(wstatus)){
        if(exit_code) *exit_code = 128 + WTERMSIG(wstatus);
        if(signum) *signum = WTERMSIG(wstatus);
    }

    return e;
}

derr_t close_extra_fds(int starting_at){
    derr_t e = E_OK;

    if(starting_at < 0){
        ORIG(&e, E_PARAM, "starting_at must be >= 0");
    }

    // brute-force close all the file descriptors we could possibly have open
    struct rlimit rl;
    int ret = getrlimit(RLIMIT_NOFILE, &rl);
    if(ret != 0){
        TRACE(&e, "getrlimit(): %x\n", FE(errno));
        ORIG(&e, E_OS, "getrlimit failed");
    }

    for(int fd = starting_at; (unsigned int)fd < rl.rlim_cur; fd++){
        close(fd);
    }
    errno = 0;

    return e;
}

// returns the "VAR=" from "VAR=VALUE", or (dstr_t){0}
static dstr_t _get_var(const dstr_t *envvar){
    size_t i;
    for(i = 0; i < envvar->len && envvar->data[i] != '='; i++){}
    if(i == envvar->len){
        return (dstr_t){0};
    }

    return dstr_sub(envvar, 0, i + 1);
}

derr_t _make_env(
    char ***out, char *const base[], const dstr_t *vars, size_t nvars
){
    derr_t e = E_OK;

    *out = NULL;

    // validate inputs
    for(size_t i = 0; i < nvars; i++){
        if(_get_var(&vars[i]).len == 0){
            TRACE(&e, "invalid environment variable: \"%x\"\n", FD(vars[i]));
            ORIG(&e, E_PARAM, "invalid environment variable in make_env");
        }
    }

    size_t len = nvars;
    if(base){
        // count the environ length
        size_t i;
        for(i = 0; base[i]; i++){}
        len += i;
    }

    char **env = dmalloc(&e, (len + 1) * sizeof(*env));
    CHECK(&e);
    // explicitly null terminate, for paranoia
    env[len] = NULL;

    char **ptr = env;

    // first copy the old env over
    if(base){
        for(size_t i = 0; base[i]; i++){
            *ptr = strdup(base[i]);
            if(!*ptr) ORIG_GO(&e, E_NOMEM, "nomem", fail);
            ptr++;
        }
    }

    // then copy the new env over, taking into account override behaviors
    for(size_t i = 0; i < nvars; i++){
        // make a cstring the dstr_t
        char *copy;
        PROP_GO(&e, dstr_dupstr(vars[i], &copy), fail);

        // is this variable overriding an existing variable?
        const dstr_t newvar = _get_var(&vars[i]);
        bool override = false;
        size_t j;
        for(j = 0; env[j]; j++){
            dstr_t d_oldvar;
            DSTR_WRAP(d_oldvar, env[j], strlen(env[j]), true);
            const dstr_t oldvar = _get_var(&d_oldvar);
            if(dstr_cmp(&newvar, &oldvar) == 0){
                override = true;
                break;
            }
        }

        if(override){
            free(env[j]);
            env[j] = copy;
        }else{
            *ptr = copy;
            ptr++;
        }
    }

    *out = env;

    return e;

fail:
    for(size_t i = 0; env[i]; i++){
        free(env[i]);
    }
    free(env);
    return e;
}


typedef struct {
    const dstr_t file;
    dstr_t *out;
    bool found;
} dwhich_arg_t;

static derr_t which_hook(
    const string_builder_t *base,
    const dstr_t *file,
    bool isdir,
    void *userdata
){
    derr_t e = E_OK;

    dwhich_arg_t *arg = userdata;

    if(isdir) return e;

    if(arg->found) return e;

    if(dstr_cmp(file, &arg->file) == 0){
        string_builder_t sb = sb_append(base, SBD(*file));
        PROP(&e, FMT(arg->out, "%x", FSB(sb)) );
        arg->found = true;
    }

    return e;
}

// implements `which` in code, letting us implement execvpe without _GNU_SOURCE
static derr_t dwhich(const dstr_t file, dstr_t *out, bool *found){
    derr_t e = E_OK;

    if(found) *found = false;

    // skip the search if file contains a slash
    if(dstr_count(&file, &DSTR_LIT("/"))){
        PROP(&e, dstr_copy(&file, out) );
        PROP(&e, dstr_null_terminate(out) );
        if(found) *found = true;
        return e;
    }

    // get the PATH variable
    dstr_t path = dgetenv(DSTR_LIT("PATH"), NULL);

    dwhich_arg_t arg = { .file = file, .out = out };

    while(path.len){
        // take the first ':' section
        dstr_t elem;
        dstr_split2_soft(path, DSTR_LIT(":"), NULL, &elem, &path);

        if(elem.len == 0) continue;

        string_builder_t elem_path = SBD(elem);
        derr_t e2 = for_each_file_in_dir(&elem_path, which_hook, &arg);
        CATCH(&e2, E_FS){
            // ignore filesystem errors
            DROP_VAR(&e2);
            continue;
        }else PROP_VAR(&e, &e2);

        if(arg.found){
            if(found) *found = true;
            break;
        }
    }

    return e;
}


// uses execve under the hood
// calls exit(1) on failure
derr_t _dexec(
    const dstr_t file,
    char *const env[],
    // args will always be prefixed by the filename as the first argument
    const dstr_t *args,
    size_t nargs
){
    derr_t e = E_OK;

    // search for the file on the PATH
    DSTR_VAR(path, 1024);
    bool found;
    PROP(&e, dwhich(file, &path, &found) );
    if(!found){
        TRACE(&e, "%x not found on PATH\n", FD(file));
        ORIG(&e, E_PARAM, "dexec(): file not found");
    }

    // create a null-terminated argv array
    char **argv = dmalloc(&e, (1 + nargs + 1) * sizeof(*argv));
    CHECK(&e);
    // explicitly null terminate, for paranoia
    argv[1 + nargs] = NULL;

    char **ptr = argv;

    // copy the path over
    PROP_GO(&e, dstr_dupstr(file, ptr), fail);
    ptr++;

    // copy the args over
    for(size_t i = 0; i < nargs; i++){
        PROP_GO(&e, dstr_dupstr(args[i], ptr), fail);
        ptr++;
    }

    // this only returns on failure
    execve(path.data, argv, env);

    TRACE(&e, "execve: %x\n", FE(errno));
    ORIG_GO(&e, E_OS, "execve failed", fail);

fail:
    for(size_t i = 0; argv[i]; i++){
        free(argv[i]);
    }
    free(argv);
    return e;
}

void _after_child_proc(derr_t e){
    if(is_error(e)){
        DROP_CMD(
            FFMT(stderr, "error trace (%x):\n", FD(error_to_dstr(e.type)))
        );
        if(e.msg.len > 0){
            DROP_CMD( FFMT(stderr, "%x", FD((e).msg)) );
        }
        DROP_VAR(&e);
        exit(1);
    }
    exit(0);
}

derr_t urandom_bytes(dstr_t *out, size_t count){
    derr_t e = E_OK;

    int fd;
    PROP(&e, dopen("/dev/urandom", O_RDONLY, 0, &fd) );

    size_t amnt_read;
    PROP_GO(&e, dstr_read(fd, out, count, &amnt_read), cu);

    if(amnt_read != count){
        ORIG_GO(&e, E_OS, "wrong number of bytes", cu);
    }

cu:
    close(fd);

    return e;
}

#endif // _WIN32
