#include "libcli/libcli.h"

#include <libcli/print_help.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>

// os-specific defaults
#ifdef _WIN32
DSTR_STATIC(os_default_sm_dir, "C:/ProgramData/splintermail");
DSTR_STATIC(os_default_sock, "\\\\.\\pipe\\splintermail-citm");
#else
DSTR_STATIC(os_default_sm_dir, "/var/lib/splintermail");
DSTR_STATIC(os_default_sock, "/var/run/splintermail.sock");
static derr_t detect_system_fds(listener_list_t l, int *sockfd);
#endif

// pass all calls to the real thing
ui_i default_ui_harness(void){
    return (ui_i){
        .dir_w_access_path = dir_w_access_path,
        .dir_rw_access_path = dir_rw_access_path,
        .file_r_access_path = file_r_access_path,
        .file_w_access_path = file_w_access_path,
        .file_rw_access_path = file_rw_access_path,
        .exists_path = exists_path,
        .for_each_file_in_dir = for_each_file_in_dir,
        .ssl_library_init = ssl_library_init,
        .ssl_library_close = ssl_library_close,
        .api_token_read_increment_write_path =
            api_token_read_increment_write_path,
        .api_token_free0 = api_token_free0,
        .register_api_token_path_sync = register_api_token_path_sync,
        .api_pass_sync = api_pass_sync,
        .api_token_sync = api_token_sync,
        .user_prompt = user_prompt,
        .status_main = status_main,
        .configure_main = uv_configure_main,
        .uv_citm = uv_citm,
        #ifndef _WIN32
        .detect_system_fds = detect_system_fds,
        #endif
    };
}

static derr_t maybe_read_path(
    const ui_i ui,
    dstr_t *config_text,
    string_builder_t path,
    dstr_off_t *out
){
    derr_t e = E_OK;

    bool ok;
    PROP(&e, ui.file_r_access_path(&path, &ok) );
    if(!ok) return e;

    size_t start = config_text->len;
    PROP(&e, dstr_read_path(&path, config_text) );
    size_t len = config_text->len - start;
    *out = (dstr_off_t){ .buf = config_text, .start = start, .len = len };

    return e;
}

static derr_t maybe_parse_config(
    string_builder_t path,
    dstr_off_t off,
    opt_spec_t **spec,
    size_t speclen
){
    derr_t e = E_OK;

    if(!off.len) return e;

    dstr_t sub = dstr_from_off(off);

    derr_t e2 = conf_parse(&sub, spec, speclen);
    CATCH_ANY(&e2){
        DROP_VAR(&e2);
        ORIG(&e, E_VALUE, "unable to parse config file at \"%x\"", FSB(path));
    }

    return e;
}

static derr_t load_os_config_files(
    const ui_i ui, dstr_t* config_text, opt_spec_t** spec, size_t speclen
){
#ifndef _WIN32
    derr_t e = E_OK;

    // Linux / Mac code

    // try to read /etc/splintermail.conf
    string_builder_t default_path = SBS("/etc/splintermail.conf");
    dstr_off_t default_text = {0};
    PROP(&e, maybe_read_path(ui, config_text, default_path, &default_text) );

    // try to read $HOME/.splintermail.conf
    char* home = getenv("HOME");
    string_builder_t home_base = SBS(home);
    string_builder_t home_dir_path =
        sb_append(&home_base, SBS(".splintermail.conf"));
    dstr_off_t home_dir_text = {0};
    if(home){
        PROP(&e,
            maybe_read_path(ui, config_text, home_dir_path, &home_dir_text)
        );
    }

    // try to read $XDG_CONFIG_HOME/splintermail.conf
    char* conf = getenv("XDG_CONFIG_HOME");
    string_builder_t conf_base = SBS(conf);
    string_builder_t conf_dir_path =
        sb_append(&conf_base, SBS(".splintermail.conf"));
    dstr_off_t conf_dir_text = {0};
    if(conf){
        PROP(&e,
            maybe_read_path(ui, config_text, conf_dir_path, &conf_dir_text)
        );
    }

    // now that there is no more realloc risk, parse config text
    PROP(&e, maybe_parse_config(default_path, default_text, spec, speclen) );
    PROP(&e, maybe_parse_config(home_dir_path, home_dir_text, spec, speclen) );
    PROP(&e, maybe_parse_config(conf_dir_path, conf_dir_text, spec, speclen) );

    return e;

#else
    // Windows code

// no point in seeing MSVC warnings in MS's own damn code
#pragma warning(push, 0)
#include <Shlwapi.h>
#pragma warning(pop)

    derr_t e = E_OK;

    // try to read %APPDATA%\splintermail\splintermail.conf
    char* appdata = getenv("APPDATA");
    string_builder_t appdata_base = SBS(appdata);
    string_builder_t appdata_path =
        sb_append(&appdata_base, SBS("splintermail/splintermail.conf"));
    dstr_off_t appdata_text = {0};
    if(appdata){
        PROP(&e,
            maybe_read_path(ui, config_text, appdata_path, &appdata_text)
        );
    }

    // try to read splintermail.conf in parent of directory of executable
    char temp[MAX_PATH];
    DWORD dret;
    // first get the filename of the executable (/path/to/bin/splintermail.exe)
    // https://msdn.microsoft.com/en-us/library/windows/desktop/ms683197(v=vs.85).aspx
    dret = GetModuleFileNameA(NULL, temp, sizeof(temp));
    if(dret == 0){
        fprintf(stderr, "unable to get name of executable\n");
        ORIG(&e, E_VALUE, "unable to get name of executable");
    }
    dstr_t dtemp = dstr_from_cstr(temp);
    // get the first parent (/path/to/bin)
    dstr_t p1 = ddirname(dtemp);
    // get the second parent (/path/to)
    dstr_t p2 = ddirname(p1);
    // defualt path is /path/to/splintermail.conf
    string_builder_t p2_path = SBD(p2);
    string_builder_t default_path =
        sb_append(&p2_path, SBS("splintermail.conf"));
    dstr_off_t default_text = {0};
    PROP(&e, maybe_read_path(ui, config_text, default_path, &default_text) );

    // now that there is no more realloc risk, parse config text
    PROP(&e, maybe_parse_config(default_path, default_text, spec, speclen) );
    PROP(&e, maybe_parse_config(appdata_path, appdata_text, spec, speclen) );

    return e;

#endif
}


static derr_t get_os_default_account_path(
    const ui_i ui,
    string_builder_t *mem,
    string_builder_t *account_path,
    bool* account_dir_access
){
#ifdef _WIN32

    derr_t e = E_OK;

    // Windows default accounts dir is %APPDATA%/splintermail/
    char* appdata = getenv("APPDATA");
    if(!appdata) return e;

    *mem = SBS(appdata);
    *account_path = sb_append(mem, SBS("splintermail"));
    PROP(&e, ui.dir_rw_access_path(account_path, true, account_dir_access) );

    return e;

#else // not _WIN32
#ifdef __APPLE__

    derr_t e = E_OK;

    // OSX always sets account_dir to in ~/Library/splintermail
    char* home = getenv("HOME");
    if(!home) return e;
    *mem = SBS(home);
    *account_path = sb_append(mem, SBS("Library/splintermail"));
    PROP(&e, ui.dir_rw_access_path(account_path, true, account_dir_access) );

    return e;

#else

    derr_t e = E_OK;

    // Linux default account_dir is $XDG_CACHE_HOME/splintermail
    char* cache = getenv("XDG_CACHE_HOME");
    if(cache){
        *mem = SBS(cache);
        *account_path = sb_append(mem, SBS("splintermail"));
        PROP(&e,
            ui.dir_rw_access_path(account_path, true, account_dir_access)
        );
        return e;
    }

    // or $HOME/.cache/splintermail
    char* home = getenv("HOME");
    if(!home) return e;
    // make sure there is a $HOME/.cache
    *mem = SBS(home);
    string_builder_t cache_path = sb_append(mem, SBS(".cache"));
    bool ok;
    PROP(&e, ui.dir_rw_access_path(&cache_path, true, &ok) );
    if(!ok) return e;
    // then make sure there is a $HOME/.cache/splintermail
    *account_path = sb_append(mem, SBS(".cache/splintermail"));
    PROP(&e, ui.dir_rw_access_path(account_path, true, account_dir_access) );
    return e;

#endif // __APPLE__
#endif // _WIN32
}

struct user_search_data_t {
    dstr_t* user;
    int nfolders;
};

static derr_t user_search_hook(
    const string_builder_t *base,
    const dstr_t *file,
    bool isdir,
    void *userdata
){
    (void) base;
    derr_t e = E_OK;
    struct user_search_data_t* search_data = userdata;
    // ignore regular files
    if(!isdir) return e;
    // sort out folders that don't end contain exactly one "@"
    DSTR_STATIC(arroba, "@");
    if(dstr_count(file, &arroba) != 1) return e;
    // store the name of the folder
    if(search_data->nfolders == 0){
        PROP(&e, dstr_copy(file, search_data->user) );
    }
    // this will be an error indication if we reach this more than once
    search_data->nfolders += 1;
    return e;
}

static derr_t guess_user(
    ui_i ui, const string_builder_t account_path, dstr_t *user
){
    derr_t e = E_OK;

    DSTR_VAR(temp, 128);
    struct user_search_data_t search_data = { &temp };
    PROP(&e,
        ui.for_each_file_in_dir(
            &account_path, user_search_hook, (void*)&search_data
        )
    );
    // make sure we got a valid result
    if(search_data.nfolders == 1){
        PROP(&e, dstr_copy(&temp, user) );
    }

    return e;
}


static derr_t check_api_token_register(
    const ui_i ui,
    const string_builder_t *account_path,
    const dstr_t user,
    bool *do_reg
){
    derr_t e = E_OK;
    derr_t e2;
    *do_reg = false;
    // check if the user indicated they never want to register
    string_builder_t user_path = sb_append(account_path, SBD(user));
    string_builder_t noregister_path =
        sb_append(&user_path, SBS("noregister"));
    // if a noregister file exists, don't do anything
    bool ok;
    PROP(&e, ui.file_r_access_path(&noregister_path, &ok) );
    if(ok){
        LOG_DEBUG("found noregister for user %x, not registering\n", FD(user));
        return e;
    }
    // prompt user for what to do
    DSTR_STATIC(prompt,
        "Register an API token for this device (for password-free access)?\n"
        "[y (yes) / n (not now) / e (not ever)]:"
    );
    size_t ret = 99;
    PROP(&e, prompt_one_of(ui.user_prompt, prompt, "yne", &ret) );
    switch(ret){
        // "yes"
        case 0:
            // make sure the user directory exists
            PROP(&e, ui.dir_w_access_path(&user_path, true, &ok) );
            if(!ok){
                LOG_DEBUG("no write access to save API token; not registering\n");
                return e;
            }
            *do_reg = true;
            break;
        // "not now"
        case 1: /*do nothing*/ break;
        // "not ever"
        case 2:
            // make sure the user directory exists
            PROP(&e, ui.dir_w_access_path(&user_path, true, &ok) );
            if(!ok){
                LOG_DEBUG(
                    "no write access to save noregister; doing nothing\n"
                );
                return e;
            }
            // now write to the noregister file
            e2 = touch_path(&noregister_path);
            CATCH_ANY(&e2){
                DROP_VAR(&e2);
                LOG_DEBUG("failed to save noregister\n");
                return e;
            }
            break;
        default: ORIG(&e, E_INTERNAL, "unallowed response");
    }
    return e;
}

/* find device keys from ditm and copy them for citm, so ditm users don't have
   to register new keys when the switch to citm. */
static derr_t migrate_ditm_keys_hook(
    const string_builder_t *base,
    const dstr_t* file,
    bool isdir,
    void* userdata
){
    derr_t e = E_OK;
    (void)userdata;

    // ignore regular files
    if(!isdir) return e;
    // sort out folders that don't end contain exactly one "@"
    if(dstr_count(file, &DSTR_LIT("@")) != 1) return e;

    // check for a key to migrate
    string_builder_t ditm_user = sb_append(base, SBD(*file));
    string_builder_t src = sb_append(&ditm_user, SBS("device.pem"));
    bool ok;
    PROP(&e, exists_path(&src, &ok) );
    if(!ok) return e;

    // check if a migration is unnecessary
    string_builder_t citm_path = sb_append(base, SBS("citm"));
    string_builder_t citm_user = sb_append(&citm_path, SBD(*file));
    string_builder_t user_keys = sb_append(&citm_user, SBS("keys"));
    string_builder_t dst = sb_append(&user_keys, SBS("mykey.pem"));
    PROP(&e, exists_path(&dst, &ok) );
    if(ok) return e;

    // do the migration
    PROP(&e, mkdirs_path(&user_keys, 0700) );
    PROP(&e, file_copy_path(&src, &dst, 0600) );

    return e;
}

derr_t trim_logfile(const char *path, long maxlen){
    derr_t e = E_OK;

    // skip if file doesn't exist
    bool ok;
    PROP(&e, dexists(path, &ok) );
    if(!ok) return e;

    FILE *flog = NULL;
    FILE *ftmp = NULL;

    DSTR_VAR(temp, 4096);
    PROP_GO(&e, FMT(&temp, "%x.tmp", FS(path)), cu);

    // check the size of the log file
    compat_stat_t s;
    PROP_GO(&e, dfopen(path, "r", &flog), cu);
    PROP_GO(&e, dffstat(flog, &s), cu);
    intmax_t size = s.st_size;
    if(size <= (intmax_t)maxlen){
        // file is within limit
        goto cu;
    }

    // skip to the tail of the file
    long offset = (long)(size) - maxlen;
    PROP_GO(&e, dfseek(flog, offset, SEEK_SET), cu);

    // skip to the end of the current line
    int c;
    do {
        c = fgetc(flog);
    } while(c != EOF && c != '\n');
    if(c == EOF){
        fprintf(stderr, "encountered EOF while seeking end-of-line in log\n");
        goto cu;
    }

    // copy the remaining lines to a temp file
    PROP_GO(&e, dfopen(temp.data, "w", &ftmp), cu);
    while((void)(c = fgetc(flog)), c != EOF){
        if(fputc(c, ftmp) == EOF){
            ORIG_GO(&e, E_OS, "failed to write log bytes", cu);
        }
    }
    if(ferror(flog)){
        ORIG_GO(&e, E_OS, "failed to read log bytes", cu);
    }

    // read side can close without error checks
    fclose(flog);
    flog = NULL;

    // write side needs checking
    derr_t e2 = dfclose(ftmp);
    ftmp = NULL;
    PROP_VAR_GO(&e, &e2, cu);

    // now replace original file with tail file
    PROP_GO(&e, drename_atomic(temp.data, path), cu);
    compat_unlink(temp.data);

cu:
    if(flog) fclose(flog);
    if(ftmp) fclose(ftmp);
    return e;
}

static void trim_logfile_quiet(const char *path, long maxlen){
    derr_t e = trim_logfile(path, maxlen);
    if(!is_error(e)){
        return;
    }
    size_t len = MAX(e.msg.len, INT_MAX);
    if(len == 0){
        fprintf(stderr, "trim_logfile failed but left no error message\n");
    }else{
        fprintf(stderr, "trim_logfile failed: %.*s\n", (int)len, e.msg.data);
    }
    DROP_VAR(&e);
}

static derr_t listener_cb(void *data, dstr_t val){
    derr_t e = E_OK;

    listener_list_t *l = data;

    if(l->len >= l->cap){
        FFMT_QUIET(stderr,
            "too many --listen flags, limit %x\n", FU(l->cap)
        );
        l->invalid = true;
        return e;
    }

    size_t idx = l->len++;
    addrspec_t *spec = &l->specs[idx];

    // val.data is persisted, but the dstr_t box is not
    l->dstrs[idx] = val;

    DSTR_VAR(errbuf, 512);
    bool ok = parse_addrspec_ex(&l->dstrs[idx], spec, &errbuf);
    if(!ok){
        FFMT_QUIET(stderr, "%x\n", FD(errbuf));
        l->invalid = true;
        return e;
    }
    dstr_t specstr = dstr_from_off(dstr_off_extend(spec->scheme, spec->port));

    // --listen requires each of SCHEME, HOST, and PORT
    if(!spec->scheme.len || !spec->host.len || !spec->port.len){
        FFMT_QUIET(stderr,
            "--listen %x is missing elements, must be SCHEME://HOST:PORT\n",
            FD(specstr)
        );
        l->invalid = true;
    }
    if(l->invalid) return e;

    // check for a valid scheme
    dstr_t scheme = dstr_from_off(spec->scheme);
    imap_security_e sec;
    ok = imap_scheme_parse(scheme, &sec);
    if(!ok){
        FFMT_QUIET(stderr,
            "--listen %x has invalid scheme '%x', "
            "must be one of tls, starttls, or insecure\n",
            FD(specstr),
            FD(scheme)
        );
        l->invalid = true;
        return e;
    }

    // check if we require a --key and --cert
    if(sec != IMAP_SEC_INSECURE) l->key_required = true;

    return e;
}

#ifdef _WIN32

// windows

static void indicate_ready(void *user_data, uv_citm_t *uv_citm){
    (void)user_data;
    (void)uv_citm;
    ReportSvcStatus(SERVICE_RUNNING, NO_ERROR, 0 );
}

#else
#ifdef __APPLE__

// macos:

#include <launch.h>

static derr_t detect_system_fds(listener_list_t l, int *sockfd){
    derr_t e = E_OK;

    /* listener fds, each named `listenN` since launchd offers arrays of
       sockets but doesn't presrve their order (!) */
    int *fds = NULL;
    size_t nfds;
    size_t i = 0;
    for(; true; i++){
        DSTR_VAR(key, 64);
        PROP_GO(&e, FMT(&key, "listen%x", FU(i)), cu);
        int ret = launch_activate_socket(key.data, &fds, &nfds);
        // end when a key lookup fails
        if(ret == ESRCH) break;
        if(ret){
            ORIG_GO(&e,
                E_OS,
                "failed to get launchd socket \"%x\": %x",
                cu,
                FD(key),
                FE(ret)
            );
        }
        if(nfds != 1){
            ORIG_GO(&e,
                E_PARAM,
                "launchd socket \"%x\" contains %x listeners, "
                "but exactly 1 is required",
                cu,
                FD(key),
                FU(nfds)
            );
        }
        if(i < l.len){
            l.lfds[i] = fds[0];
        }
        free(fds);
        fds = NULL;
    }

    if(i != l.len){
        ORIG_GO(&e,
            E_PARAM,
            "launchd service provided %x listener sockets, "
            "but splintermail is configured for exactly %x listeners\n",
            cu,
            FU(nfds),
            FU(l.len)
        );
    }

    // status fds
    int ret = launch_activate_socket("status", &fds, &nfds);
    if(ret){
        ORIG_GO(&e,
            E_OS,
            "failed to get launchd status socket: %x",
            cu,
            FE(ret)
        );
    }
    if(nfds != 1){
        ORIG_GO(&e,
            E_PARAM,
            "launchd service provided %x status sockets, "
            "but splintermail requires exactly 1\n",
            cu,
            FU(nfds)
        );
    }
    *sockfd = fds[0];

cu:
    if(fds) free(fds);

    return e;
}

static void indicate_ready(void *user_data, uv_citm_t *uv_citm){
    (void)user_data;
    (void)uv_citm;
    // launchd doesn't seem to have an sd_notify equivalent
}

#else

// linux

#include <systemd/sd-daemon.h>

static derr_t dsd_is_socket(int i, int fd, int family, bool *ok){
    derr_t e = E_OK;

    *ok = false;

    // always SOCK_STREAM, always listening=1
    int ret = sd_is_socket(fd, family, SOCK_STREAM, 1);
    if(ret < 0){
        ORIG(&e, E_OS, "sd_is_socket(%x): %x", FI(i), FE(-ret));
    }

    *ok = ret;

    return e;
}

static derr_t detect_system_fds(listener_list_t l, int *sockfd){
    derr_t e = E_OK;

    // there should be one unix socket, and 1 or more tcp sockets

    int nfds = sd_listen_fds(1); // 1 means "unset_env = true"
    if(nfds < 0) ORIG(&e, E_OS, "sd_listen_fds: %x", FE(-nfds));

    size_t nunix = 0;
    size_t ntcp = 0;
    for(int i = 0; i < nfds; i++){
        int fd = SD_LISTEN_FDS_START + i;
        bool is_unix;
        bool is_ipv4;
        bool is_ipv6;
        PROP(&e, dsd_is_socket(i, fd, AF_UNIX, &is_unix) );
        PROP(&e, dsd_is_socket(i, fd, AF_INET, &is_ipv4) );
        PROP(&e, dsd_is_socket(i, fd, AF_INET6, &is_ipv6) );
        if(is_unix){
            if(!nunix) *sockfd = fd;
            nunix++;
        }else if(is_ipv4 || is_ipv6){
            if(ntcp < l.len) l.lfds[ntcp] = fd;
            ntcp++;
        }else{
            ORIG(&e, E_PARAM, "got systemd socket of unrecognized family");
        }
    }

    // make sure we got the right counts
    if(nunix != 1){
        ORIG(&e,
            E_PARAM,
            "systemd service provided %x AF_UNIX status sockets, "
            "but splintermail requires exactly 1\n",
            FU(nunix)
        );
    }
    if(ntcp != l.len){
        ORIG(&e,
            E_PARAM,
            "systemd service provided %x listener sockets, "
            "but splintermail is configured for exactly %x listeners\n",
            FU(ntcp),
            FU(l.len)
        );
    }

    return e;
}

static void indicate_ready(void *user_data, uv_citm_t *uv_citm){
    (void)user_data;
    (void)uv_citm;
    (void)sd_notify(0, "READY=1");
}

#endif
#endif

static derr_t citm_main(
    const ui_i ui,
    bool system, // windows_service or --system
    const opt_spec_t o_cert,
    const opt_spec_t o_key,
    listener_list_t listeners,
    const string_builder_t sm_dir_path,
    const string_builder_t status_sock,
    const dstr_t baseurl,
    const dstr_t acmeurl,
    int *retval
){
    derr_t e = E_OK;

    dstr_t cert = {0};
    dstr_t key = {0};
    int sockfd = -1;

    if(listeners.len == 0){
        DSTR_STATIC(default_starttls4, "starttls://127.0.0.1:143");
        DSTR_STATIC(default_starttls6, "starttls://[::1]:143");
        DSTR_STATIC(default_tls4, "tls://127.0.0.1:993");
        DSTR_STATIC(default_tls6, "tls://[::1]:993");
        listeners.specs[0] = must_parse_addrspec(&default_starttls4);
        listeners.specs[1] = must_parse_addrspec(&default_starttls6);
        listeners.specs[2] = must_parse_addrspec(&default_tls4);
        listeners.specs[3] = must_parse_addrspec(&default_tls6);
        listeners.len = 4;
        listeners.key_required = true;
    }

    DSTR_STATIC(default_remote, "tls://splintermail.com:993");
    addrspec_t remote = must_parse_addrspec(&default_remote);

    // figure cert and key paths
    if((bool)o_cert.found != (bool)o_key.found){
        fprintf(stderr, "you must specify --key and --cert together\n");
        *retval = 18;
        goto cu;
    }
    if(listeners.key_required && o_cert.found){
        PROP_GO(&e, FMT(&cert, "%x", FD(o_cert.val)), cu);
        PROP_GO(&e, FMT(&key, "%x", FD(o_key.val)), cu);
    }

    #ifndef _WIN32
    if(system) PROP_GO(&e, ui.detect_system_fds(listeners, &sockfd), cu);
    #endif

    // migrate pre-citm device keys for use with citm
    PROP_GO(&e,
        ui.for_each_file_in_dir(&sm_dir_path, migrate_ditm_keys_hook, NULL),
    cu);

    PROP_GO(&e,
        ui.uv_citm(
            listeners.specs,
            listeners.lfds,
            listeners.len,
            remote,
            key.data,
            cert.data,
            acmeurl,
            NULL,  // acme_verify_name
            baseurl,  // sm_baseurl
            status_sock,
            &sockfd,
            NULL,  // client_ctx
            sm_dir_path,
            system ? indicate_ready : NULL,
            NULL, // user_async_hook
            NULL
        ),
    cu);

    *retval = 0;

cu:
    dstr_free(&cert);
    dstr_free(&key);
    if(sockfd > -1) compat_close(sockfd);

    return e;
}

static derr_t api_command_main(
    const ui_i ui,
    const opt_spec_t o_account_dir,
    const opt_spec_t o_user,
    int newargc,
    char **argv,
    const dstr_t baseurl,
    int *retval
){
    dstr_t user = {0};
    dstr_t apipath = {0};
    dstr_t password = {0};
    dstr_t new_password = {0};
    dstr_t confirm_password = {0};
    dstr_t recv = {0};
    json_t json = {0};
    api_token_t token = {0};

    string_builder_t mem = {0};
    string_builder_t account_path = {0};
    string_builder_t user_path = {0};
    string_builder_t creds_path = {0};

    http_sync_t sync = {0};

    derr_t e = E_OK;

    PROP_GO(&e, http_sync_init(&sync, NULL), cu);

    // --account_dir option
    bool account_dir_access = false;
    if(o_account_dir.found){
        account_path = SBD(o_account_dir.val);
        // make sure we can access the account_dir
        PROP_GO(&e,
            ui.dir_rw_access_path(&account_path, true, &account_dir_access),
        cu);
    }else{
        // default is determined by OS and environment variables
        get_os_default_account_path(
            ui, &mem, &account_path, &account_dir_access
        );
    }

    if(!account_dir_access){
        FFMT_QUIET(stderr,
            "account directory %x not found or not "
            "accessible; API token access disabled\n",
            FSB(account_path)
        );
    }

    // figure out who our user is
    if(o_user.found){
        PROP_GO(&e, FMT(&user, "%x", FD(o_user.val)), cu);
    }else if(account_dir_access){
        PROP_GO(&e, guess_user(ui, account_path, &user), cu);
    }
    if(!user.len){
        fprintf(stderr, "Unable to determine user and --user not specified\n");
        fprintf(stderr, "try `%s --help` for usage\n", argv[0]);
        *retval = 5;
        goto cu;
    }

    // make sure we have access to users' folder or that we could make it
    /* this step is necessary because we don't want to create a folder for
       the user until they choose to register an API token or say they never
       want to register */
    bool user_dir_access = false;
    bool can_register = false; // have permissions to save an token?
    if(account_dir_access){
        // start the creds_path
        user_path = sb_append(&account_path, SBD(user));
        // if the folder doesn't exist, we know we can make it
        bool ok;
        PROP_GO(&e, ui.exists_path(&user_path, &ok), cu);
        if(!ok){
            user_dir_access = true;
            can_register = true;
        // if it does exist we must have rw access to it
        }else{
            PROP_GO(&e, ui.dir_rw_access_path(&user_path, false, &ok), cu);
            if(ok){
                user_dir_access = true;
            }else{
                FFMT_QUIET(stderr,
                    "Insufficient permissions for user directory %x; "
                    "API token access disabled\n",
                    FSB(user_path)
                );
            }
        }
    }

    // try to load the user's api_token
    bool creds_found = false;  // have good creds?
    if(user_dir_access){
        // complete the creds_path
        creds_path = sb_append(&user_path, SBS("api_token.json"));
        // check if the file already exists
        bool ok;
        PROP_GO(&e, ui.exists_path(&creds_path, &ok), cu);
        if(ok){
            // check if we have RW access to it
            PROP_GO(&e, ui.file_rw_access_path(&creds_path, &ok), cu);
            if(ok){
                // read/increment/write token
                PROP_GO(&e,
                    ui.api_token_read_increment_write_path(
                        &creds_path, &token, &ok
                    ),
                cu);
                if(!ok){
                    // broken token file now deleted, tell user
                    FFMT_QUIET(stderr,
                        "deleted corrupted api token (%x), please retry\n",
                        FSB(creds_path)
                    );
                    *retval = 17;
                    goto cu;
                }else{
                    creds_found = true;
                }
            }else{
                // file exists, but we have no access to it
                FFMT_QUIET(stderr,
                    "Insufficient permissions for %x; "
                    "API token access disabled\n",
                    FSB(creds_path)
                );
            }
        }else{
            can_register = true;
        }
    }

    // get the api command
    dstr_t command = dstr_from_cstr(argv[1]);
    // get the argument if it exists
    dstr_t apiarg = {0};
    if(newargc > 2){
        apiarg = dstr_from_cstr(argv[2]);
    }

    // list of commands requiring special handling
    DSTR_STATIC(add_device, "add_device");
    DSTR_STATIC(add_token, "add_token");
    DSTR_STATIC(change_password, "change_password");
    DSTR_STATIC(delete_account, "delete_account");
    DSTR_STATIC(delete_all_mail, "delete_all_mail");
    DSTR_STATIC(delete_all_aliases, "delete_all_aliases");

    // (we know we need a password if we don't have creds)
    bool need_password = !creds_found;

    // check if the endpoint will require a password
    if(dstr_cmp(&command, &add_device) == 0) need_password = true;
    if(dstr_cmp(&command, &add_token) == 0) need_password = true;
    if(dstr_cmp(&command, &delete_account) == 0) need_password = true;
    if(dstr_cmp(&command, &delete_all_mail) == 0) need_password = true;
    if(dstr_cmp(&command, &delete_all_aliases) == 0) need_password = true;

    // request any passwords that are needed
    if(dstr_cmp(&command, &change_password) == 0){
        // we will need to submit this API request with a password
        need_password = true;
        // prompt for passwords
        PROP_GO(&e,
            ui.user_prompt(
                DSTR_LIT("Old Splintermail.com Account Password:"),
                &password,
                true
            ),
        cu);
        PROP_GO(&e,
            ui.user_prompt(DSTR_LIT("New Password:"), &new_password, true),
        cu);
        PROP_GO(&e,
            ui.user_prompt(
                DSTR_LIT("Confirm Password:"), &confirm_password, true
            ),
        cu);
        // make sure confirmation was valid
        if(dstr_cmp(&new_password, &confirm_password) != 0){
            FFMT_QUIET(stderr, "Password confirmation failed.\n");
            *retval = 6;
            goto cu;
        }
        // set the argument for the API call
        apiarg = new_password;
    }else if(need_password){
        PROP_GO(&e,
            ui.user_prompt(
                DSTR_LIT("Splintermail.com Account Password:"), &password, true
            ),
        cu);
    }

    // now check if we should register for an API token:
    if(can_register && !creds_found){
        bool do_reg;
        PROP_GO(&e,
            check_api_token_register(ui, &account_path, user, &do_reg),
        cu);
        if(do_reg){
            // do the registration
            derr_t e2 = ui.register_api_token_path_sync(
                &sync, baseurl, user, password, &creds_path
            );
            CATCH_ANY(&e2){
                LOG_ERROR("failed to register API token with server\n");
                LOG_DEBUG("%x", FD(e2.msg));
                DROP_VAR(&e2);
                *retval = 7;
                goto cu;
            }
        }
    }

    // check for commands which require confirmation
    bool need_confirmation = false
        || dstr_eq(command, delete_all_mail)
        || dstr_eq(command, delete_all_aliases)
        || dstr_eq(command, delete_account);

    if(need_confirmation){
        // prompt for confirmation
        DSTR_STATIC(confirmation, "I really want to do this");
        DSTR_VAR(prompt, 256);
        PROP_GO(&e,
            FMT(&prompt,
                "`%x` needs confirmation. Type the following text:\n"
                 "%x\n",
                 FD(command),
                 FD(confirmation)
            ),
        cu);
        // get confirmation
        DSTR_VAR(temp, 256);
        PROP_GO(&e, ui.user_prompt(prompt, &temp, false), cu);
        // ignore the newline at the end of the string
        while(temp.len && (temp.data[temp.len-1] == '\r'
                           || temp.data[temp.len-1] == '\n'))
            temp.len -= 1;
        // verify confirmation
        if(!dstr_eq(temp, confirmation)){
            fprintf(stderr, "confirmation failed, aborting.\n");
            *retval = 8;
            goto cu;
        }
    }


    // now we can actually do the API request
    // allow overflow (fixedsize=false)
    JSON_PREP_PREALLOCATED(json, 4096, 256, false);

    PROP_GO(&e, FMT(&apipath, "/api/%x", FD(command)), cu);

    if(need_password){
        PROP_GO(&e,
            ui.api_pass_sync(
                &sync, baseurl, apipath, apiarg, user, password, &json
            ),
        cu);
    }else{
        derr_t e2 = ui.api_token_sync(
            &sync, baseurl, apipath, apiarg, token, &json
        );
        CATCH(&e2, E_TOKEN){
            FFMT_QUIET(stderr,
                "API Token rejected, deleting token.  Run this "
                "command again to generate a new token.\n"
            );
            LOG_DEBUG("%x", FD(e2.msg));
            DROP_VAR(&e2);
            e2 = dunlink_path(&creds_path);
            CATCH_ANY(&e2){
                TRACE(&e2, "error removing token\n");
                FFMT_QUIET(stderr,
                    "error removing token:\n %x", FD(e2.msg)
                );
                DROP_VAR(&e2);
            }
            *retval = 9;
            goto cu;
        }else PROP_VAR_GO(&e, &e2, cu);
    }

    dstr_t status;
    json_ptr_t contents;
    bool ok, contents_ok;

    jspec_t *jspec = JOBJ(true,
        JKEYOPT("contents", &contents_ok, JPTR(&contents)),
        JKEY("status", JDREF(&status)),
    );

    DSTR_VAR(errbuf, 1024);
    PROP_GO(&e, jspec_read_ex(jspec, json.root, &ok, &errbuf), cu);
    if(!ok){
        TRACE(&e, "%x\n", FD(errbuf));
        ORIG_GO(&e, E_RESPONSE, "invalid server response", cu);
    }

    if(!dstr_eq(status, DSTR_LIT("success"))){
        if(contents_ok){
            dstr_t why;
            bool why_ok;
            derr_t e2 = jspec_read_ex(JDREF(&why), contents, &why_ok, NULL);
            if(is_error(e2)){
                DROP_VAR( &e2 );
            }else if(ok){
                FFMT_QUIET(stderr, "REST API call failed: \"%x\"\n", FD(why));
            }
            *retval = 14;
            goto cu;
        }
    }

    // now dump the return json
    PROP_GO(&e, json_fdump(contents, stdout), cu);

    *retval = 0;

cu:
    dstr_free(&user);
    dstr_free(&apipath);
    dstr_free(&password);
    dstr_free(&new_password);
    dstr_free(&confirm_password);
    dstr_free(&recv);
    json_free(&json);
    ui.api_token_free0(&token);
    http_sync_free(&sync);
    return e;
}

// returns zero when the all options provided are all allowed
// counts must be captured before parsing cli options
static int _limit_options(
    const char *action,
    opt_spec_t **spec,
    size_t speclen,
    int *counts,
    opt_spec_t **allowed,
    size_t nallowed
){
    int failed = 0;

    for(size_t i = 0; i < speclen; i++){
        opt_spec_t *opt = spec[i];
        // detect if the count is unchanged after the cli parse happened
        if(opt->count == counts[i]) goto next_opt;
        for(size_t j = 0; j < nallowed; j++){
            if(opt == allowed[j]) goto next_opt;
        }

        if(opt->oshort == '\0'){
            fprintf(stderr,
                "--%s is not an allowed option for %s\n",
                opt->olong,
                action
            );
        }else{
            fprintf(stderr,
                "-%c/--%s is not an allowed option for %s\n",
                opt->oshort,
                opt->olong,
                action
            );
        }
        failed += 1;

    next_opt:
        continue;
    }

    return failed;
}
#define limit_options(action, spec, speclen, counts, ok_opts) \
    _limit_options( \
        action, \
        spec, \
        speclen, \
        counts, \
        ok_opts, \
        sizeof(ok_opts) / sizeof(*ok_opts) \
    )

static void fdump_opt(opt_spec_t *spec, FILE *f){
    if(!spec->found) return;
    if(spec->val_req){
        FFMT_QUIET(f, "%x %x\n", FS(spec->olong), FD(spec->val));
    }else{
        FFMT_QUIET(f, "%x\n", FS(spec->olong));
    }
}

int do_main(const ui_i ui, int argc, char* argv[], bool windows_service){
    dstr_t config_text = {0};
    dstr_t logfile_path = {0};

    // support multiple listeners
    dstr_t dstrs[8] = {0};
    addrspec_t specs[8] = {0};
    int lfds[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    listener_list_t listeners = {
        .dstrs = dstrs, .specs = specs, .lfds = lfds, .cap = 8
    };

    derr_t e = E_OK;
    int retval = 98;
    // ignore SIGPIPE, required to work with OpenSSL
    // see https://mta.openssl.org/pipermail/openssl-users/2017-May/005776.html
    // (but SIGPIPE doesnt exist in windows)
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    // setup the ssl library (application-wide step)
    PROP_GO(&e, ui.ssl_library_init(), fail);

    // set up the pre-parse, with fields not allowed in the config file
    opt_spec_t o_help       = {'h',  "help",       false};
    opt_spec_t o_version    = {'v',  "version",    false};
    opt_spec_t o_config     = {'c',  "config",     true};
    opt_spec_t o_dump_conf  = {'\0', "dump-conf",  false};
    opt_spec_t* prespec[] = {
        &o_help,
        &o_version,
        &o_config,
        &o_dump_conf,
    };
    size_t prespeclen = sizeof(prespec) / sizeof(*prespec);
    int preargc;
    derr_t e2 = opt_parse_soft(argc, argv, prespec, prespeclen, &preargc);
    CATCH_ANY(&e2){
        DROP_VAR(&e2);
        fprintf(stderr, "try `%s --help` for usage\n", argv[0]);
        retval = 1;
        goto cu;
    }
    argc = preargc;

    // help option
    if(o_help.found){
        print_help();
        retval = 0;
        goto cu;
    }

    // version option
    if(o_version.found){
        printf(
            "%d.%d.%d\n",
            SPLINTERMAIL_VERSION_MAJOR,
            SPLINTERMAIL_VERSION_MINOR,
            SPLINTERMAIL_VERSION_PATCH
        );
        retval = 0;
        goto cu;
    }

    // dump-conf handled after config and command line

    // set up the main parse
    opt_spec_t o_debug      = {'D',  "debug",      false};
    opt_spec_t o_sock       = {'s',  "socket",     true};
    opt_spec_t o_sm_dir     = {'d',  "splintermail-dir", true};
    opt_spec_t o_logfile    = {'l',  "logfile",    true};
    opt_spec_t o_no_logfile = {'L',  "no-logfile", false};
    opt_spec_t o_listen     = {'\0', "listen", true, listener_cb, &listeners};
    opt_spec_t o_cert       = {'\0', "cert",       true};
    opt_spec_t o_key        = {'\0', "key",        true};
    opt_spec_t o_user       = {'u',  "user",       true};
    opt_spec_t o_account_dir= {'a',  "account-dir",true};
    opt_spec_t o_follow     = {'\0', "follow",     false};
    opt_spec_t o_force      = {'\0', "force",      false};
    #ifndef _WIN32
    opt_spec_t o_system     = {'\0', "system",     false};
    #endif

    opt_spec_t* spec[] = {
    //  option               citm   api_client  status  configure
        &o_debug,         // x      x           x       x
        &o_sock,          // x      .           x       x
        &o_sm_dir,        // x      .           .       x
        &o_logfile,       // x      .           .       .
        &o_no_logfile,    // x      .           .       .
        &o_listen,        // x      .           .       .
        &o_cert,          // x      .           .       .
        &o_key,           // x      .           .       .
        #ifndef _WIN32
        &o_system,        // x      .           .       .
        #endif
        &o_user,          // .      x           .       x
        &o_account_dir,   // .      x           .       .
        &o_follow,        // .      .           x       .
        &o_force,         // .      .           .       x
    };
    size_t speclen = sizeof(spec) / sizeof(*spec);
    int newargc;

    // load up config files
    if(o_config.found){
        // if `-c` or `--config` was specified, load that only
        string_builder_t config_path = SBD(o_config.val);
        bool ok;
        PROP_GO(&e, ui.file_r_access_path(&config_path, &ok), cu);
        if(!ok){
            FFMT_QUIET(
                stderr,
                "unable to access config file \"%x\"\n",
                FD(o_config.val)
            );
            retval = 2;
            goto cu;
        }
        PROP_GO(&e, dstr_read_path(&config_path, &config_text), cu);
        PROP_GO(&e, conf_parse(&config_text, spec, speclen), cu);
    }else{
        // if no `-c` or `--config`, load the OS-specific file locations
        PROP_GO(&e, load_os_config_files(ui, &config_text, spec, speclen), cu);
    }
    if(listeners.invalid){
        fprintf(stderr, "invalid config file\n");
        retval = 15;
        goto cu;
    }

    /* track which parameters were found in a config file, so we can limit the
       options allowed on the command line to what currently applies */
    int counts[sizeof(spec) / sizeof(*spec)];
    for(size_t i = 0; i < speclen; i++){
        counts[i] = spec[i]->count;
    }

    // parse commandline options
    e2 = opt_parse(argc, argv, spec, speclen, &newargc);
    CATCH_ANY(&e2){
        DROP_VAR(&e2);
        fprintf(stderr, "try `%s --help` for usage\n", argv[0]);
        retval = 1;
        goto cu;
    }
    if(listeners.invalid){
        retval = 16;
        goto cu;
    }

    // if we had --dump-conf on the command line, this is where we dump config
    if(o_dump_conf.found){
        // treat --listen specially
        for(size_t i = 0; i < listeners.len; i++){
            FFMT_QUIET(stdout, "listen %x\n", FD(dstrs[i]));
        }
        for(size_t i = 0; i < speclen; i++){
            if(strcmp(spec[i]->olong, "listen") == 0) continue;
            // all non --listen options
            fdump_opt(spec[i], stdout);
        }
        retval = 0;
        goto cu;
    }

    // limit options provided on the cli to the relevant commands
    if((newargc > 1 && strcmp("citm", argv[1]) == 0)){
        opt_spec_t *ok_opts[] = {
            &o_debug,
            &o_sock,
            &o_sm_dir,
            &o_logfile,
            &o_no_logfile,
            &o_listen,
            &o_cert,
            &o_key,
            #ifndef _WIN32
            &o_system,
            #endif
        };
        bool failed = limit_options(
            "splintermail citm", spec, speclen, counts, ok_opts
        );
        if(failed){
            retval = 1;
            goto cu;
        }
    }else if((newargc > 1 && strcmp("status", argv[1]) == 0)){
        opt_spec_t *ok_opts[] = { &o_debug, &o_sock, &o_follow };
        bool failed = limit_options(
            "splintermail status", spec, speclen, counts, ok_opts
        );
        if(failed){
            retval = 1;
            goto cu;
        }
    }else if((newargc > 1 && strcmp("configure", argv[1]) == 0)){
        opt_spec_t *ok_opts[] = {
            &o_debug, &o_sock, &o_sm_dir, &o_user, &o_force,
        };
        bool failed = limit_options(
            "splintermail configure", spec, speclen, counts, ok_opts
        );
        if(failed){
            retval = 1;
            goto cu;
        }
    }else{
        opt_spec_t *ok_opts[] = { &o_debug, &o_user, &o_account_dir };
        bool failed = limit_options(
            "splintermail api commands", spec, speclen, counts, ok_opts
        );
        if(failed){
            retval = 1;
            goto cu;
        }
    }

    // check for positional args
    if(!windows_service && newargc < 2){
        fprintf(stderr, "missing subcommand\n");
        fprintf(stderr, "try `%s --help` for usage\n", argv[0]);
        retval = 3;
        goto cu;
    }

    // debug printing
    log_level_t log_level = o_debug.found ? LOG_LVL_DEBUG : LOG_LVL_INFO;

    // always print to stderr
    logger_add_fileptr(log_level, stderr);

    // --socket option
    dstr_t socket = o_sock.found ? o_sock.val : os_default_sock;
    string_builder_t status_sock = SBD(socket);

    // --splintermail-dir option
    dstr_t sm_dir = o_sm_dir.found ? o_sm_dir.val : os_default_sm_dir;
    string_builder_t sm_dir_path = SBD(sm_dir);

    DSTR_STATIC(baseurl, "https://splintermail.com");
    DSTR_STATIC(acmeurl, LETSENCRYPT);

    // pick a subcommand-main and run it

    if(windows_service || strcmp("citm", argv[1]) == 0){
        /* configure logfile here, because logfile_path will be read during the
           final DUMP in error scenarios */

        #ifdef _WIN32
        bool system = windows_service;
        #else
        bool system = o_system.found;
        #endif

        // print to a log file, unless --no-logfile is specifed
        if(o_logfile.found > o_no_logfile.found){
            PROP_GO(&e, FMT(&logfile_path, "%x", FD(o_logfile.val)), cu);
            trim_logfile_quiet(logfile_path.data, 100000000);
            logger_add_filename(log_level, logfile_path.data);
        }
        // log file defaults to on, in ${sm_dir}/citm_log
        else if(!o_logfile.found && !o_no_logfile.found){
            PROP_GO(&e, FMT(&logfile_path, "%x/citm_log", FD(sm_dir)), cu);
            trim_logfile_quiet(logfile_path.data, 100000000);
            logger_add_filename(log_level, logfile_path.data);
        }

        PROP_GO(&e,
            citm_main(
                ui,
                system,
                o_cert,
                o_key,
                listeners,
                sm_dir_path,
                status_sock,
                baseurl,
                acmeurl,
                &retval
            ),
        cu);

    }else if(strcmp("status", argv[1]) == 0){

        PROP_GO(&e, ui.status_main(status_sock, o_follow.found), cu);

        retval = 0;

    }else if(strcmp("configure", argv[1]) == 0){

        PROP_GO(&e,
            ui.configure_main(
                baseurl,
                acmeurl,
                status_sock,
                sm_dir_path,
                o_user.val,
                o_force.found,
                &retval
            ),
        cu);

    }else{
        // api command
        PROP_GO(&e,
            api_command_main(
                ui, o_account_dir, o_user, newargc, argv, baseurl, &retval
            ),
        cu);
    }

cu:
    ui.ssl_library_close();
fail:

    // if we have an uncaught error return 125
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
        retval = 125;
    }

    for(size_t i = 0; i < listeners.len; i++){
        if(listeners.lfds[i] > -1){
            compat_close(listeners.lfds[i]);
        }
    }

    // free memory after DUMP, since logfile_path will be read during DUMP
    dstr_free(&config_text);
    dstr_free(&logfile_path);

    return retval;
}

#ifdef _WIN32

SERVICE_STATUS g_svc_status;
SERVICE_STATUS_HANDLE g_svc_status_h;

VOID ReportSvcStatus(DWORD cur_state, DWORD exit_code, DWORD wait_hint){
    static DWORD check_point = 1;

    // Fill in the SERVICE_STATUS structure.

    g_svc_status.dwCurrentState = cur_state;
    g_svc_status.dwWin32ExitCode = exit_code;
    g_svc_status.dwWaitHint = wait_hint;

    if(cur_state == SERVICE_START_PENDING){
        g_svc_status.dwControlsAccepted = 0;
    }else{
        g_svc_status.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    }

    if(cur_state == SERVICE_RUNNING || cur_state == SERVICE_STOPPED){
        g_svc_status.dwCheckPoint = 0;
    }else{
        g_svc_status.dwCheckPoint = check_point++;
    }

    // Report the status of the service to the SCM.
    SetServiceStatus(g_svc_status_h, &g_svc_status);
}

#endif // _WIN32
