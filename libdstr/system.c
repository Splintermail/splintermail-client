#include <stdlib.h>
#include <string.h>

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
                "ignoring invalid environment variable: %x\n", FD(&envvar)
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

derr_t dtime(time_t *out){
    derr_t e = E_OK;

    errno = 0;
    time_t ret = time(out);
    if(errno || ret == ((time_t)-1)){
        TRACE(&e, "time(): %x\n", FE(&errno));
        ORIG(&e, E_OS, "time() failed");
    }

    return e;
}

#ifndef _WIN32

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
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
        TRACE(&e, "pipe: %x\n", FE(&errno));
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
        TRACE(&e, "dup2: %x\n", FE(&errno));
        ORIG(&e, E_OS, "dup2 failed");
    }

    return e;
}

derr_t dfork(pid_t *pid, bool *child){
    derr_t e = E_OK;

    *child = false;
    *pid = fork();

    if(*pid < 0){
        TRACE(&e, "fork: %x\n", FE(&errno));
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
        TRACE(&e, "kill(pid=%x, sig=%x): %x", FI(pid), FI(sig), FE(&errno));
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
            "wait(pid=%x, options=%x): %x", FI(pid), FI(options), FE(&errno)
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

static derr_t _close_extra_fd(
    const string_builder_t *base, const dstr_t *file, bool isdir, void *data
){
    derr_t e = E_OK;

    (void)base;
    int starting_at = *((int*)data);

    if(isdir) return e;

    int fd;
    PROP_GO(&e, dstr_toi(file, &fd, 10), fail);

    if(fd < starting_at) return e;

    // none of the possible errors of close are relevant here
    close(fd);

    return e;

fail:
    LOG_WARN("/proc/self/fd contained non-int filename %x\n", FD(file));
    DROP_VAR(&e);
    return e;
}

derr_t close_extra_fds(int starting_at){
    derr_t e = E_OK;

    // first close the lowest file descriptor, then ignore it thereafter.
    // (so that we don't accidentally close our own DIR* while we iterate)
    close(starting_at);
    starting_at++;

    string_builder_t fd_dir = SB(FS("/proc/self/fd"));
    PROP(&e, for_each_file_in_dir2(&fd_dir, _close_extra_fd, &starting_at) );

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
            TRACE(&e, "invalid environment variable: \"%x\"\n", FD(&vars[i]));
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
        string_builder_t sb = sb_append(base, FD(file));
        PROP(&e, sb_to_dstr(&sb, &DSTR_LIT("/"), arg->out) );
        PROP(&e, dstr_null_terminate(arg->out) );
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

        string_builder_t elem_path = SB(FD(&elem));
        derr_t e2 = for_each_file_in_dir2(&elem_path, which_hook, &arg);
        CATCH(e2, E_FS){
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
        TRACE(&e, "%x not found on PATH\n", FD(&file));
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

    TRACE(&e, "execve: %x\n", FE(&errno));
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
            FFMT(stderr, NULL, "error trace (%x):\n", FD(error_to_dstr(e.type)))
        );
        if(e.msg.len > 0){
            DROP_CMD( FFMT(stderr, NULL, "%x", FD(&(e).msg)) );
        }
        DROP_VAR(&e);
        exit(1);
    }
    exit(0);
}

#endif // _WIN32
