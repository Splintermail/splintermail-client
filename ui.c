#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>

#include "libdstr/libdstr.h"
#include "libcitm/citm.h"
#include "libditm/libditm.h"

#include "ui.h"
#include "ui_harness.h"
#include "api_client.h"
#include "console_input.h"
#include "print_help.h"



// default ditm directory
#ifndef _WIN32
DSTR_STATIC(os_default_sm_dir, "/var/lib/splintermail");
#else
DSTR_STATIC(os_default_sm_dir, "C:/ProgramData/splintermail");
#endif


static derr_t load_os_config_files(dstr_t* config_text, opt_spec_t** spec,
                                   size_t speclen){
    derr_t e = E_OK;
    derr_t e2;
    /* it is useful to read all the config files first so we don't have to deal
       with obnoxious reallocations */

#ifndef _WIN32
    // Linux / Mac code
    DSTR_VAR(config_dir_conf, 4096);
    DSTR_VAR(home_dir_conf, 4096);
    DSTR_STATIC(default_conf, "/etc/splintermail.conf");
    size_t config_dir_conf_start = 0;
    size_t config_dir_conf_end = 0;
    size_t home_dir_conf_start = 0;
    size_t home_dir_conf_end = 0;
    size_t default_conf_start = 0;
    size_t default_conf_end = 0;

    // try to read $XDG_CONFIG_HOME/splintermail.conf
    char* config_dir = getenv("XDG_CONFIG_HOME");
    if(config_dir){
        PROP(&e, FMT(&config_dir_conf, "%x/splintermail.conf", FS(config_dir)) );
        if(harness.file_r_access(config_dir_conf.data)){
            config_dir_conf_start = config_text->len;
            PROP(&e, dstr_read_file(config_dir_conf.data, config_text) );
            config_dir_conf_end = config_text->len;
        }
    }

    // try to read $HOME/.splintermail.conf
    char* home = getenv("HOME");
    if(home){
        PROP(&e, FMT(&home_dir_conf, "%x/.splintermail.conf", FS(home)) );
        if(harness.file_r_access(home_dir_conf.data)){
            home_dir_conf_start = config_text->len;
            PROP(&e, dstr_read_file(home_dir_conf.data, config_text) );
            home_dir_conf_end = config_text->len;
        }
    }

    // try to read /etc/splintermail.conf
    if(harness.file_r_access(default_conf.data)){
        default_conf_start = config_text->len;
        PROP(&e, dstr_read_file(default_conf.data, config_text) );
        default_conf_end = config_text->len;
    }

    // now that everything has been read (all memory allocated), let's parse:

    if(config_dir_conf_end - config_dir_conf_end > 0){
        dstr_t subconf = dstr_sub(config_text, config_dir_conf_start, config_dir_conf_end);
        e2 = conf_parse(&subconf, spec, speclen);
        CATCH(e2, E_ANY){
            DROP_VAR(&e2);
            fprintf(stderr, "error parsing \"%s\", exiting\n", config_dir_conf.data);
            ORIG(&e, E_VALUE, "unable to parse config file");
        }
    }
    if(home_dir_conf_end - home_dir_conf_start > 0){
        dstr_t subconf = dstr_sub(config_text, home_dir_conf_start, home_dir_conf_end);
        e2 = conf_parse(&subconf, spec, speclen);
        CATCH(e2, E_ANY){
            DROP_VAR(&e2);
            fprintf(stderr, "error parsing \"%s\", exiting\n", home_dir_conf.data);
            ORIG(&e, E_VALUE, "unable to parse config file");
        }
    }

    if(default_conf_end - default_conf_start > 0){
        dstr_t subconf = dstr_sub(config_text, default_conf_start, default_conf_end);
        e2 = conf_parse(&subconf, spec, speclen);
        CATCH(e2, E_ANY){
            DROP_VAR(&e2);
            fprintf(stderr, "error parsing \"%s\", exiting\n", default_conf.data);
            ORIG(&e, E_VALUE, "unable to parse config file");
        }
    }

    return e;

#else
    // Windows code
// no point in seeing MSVC warnings in MS's own damn code
#pragma warning(push, 0)
#include <Shlwapi.h>
#pragma warning(pop)

    DSTR_VAR(user_conf, 4096);
    DSTR_VAR(default_conf, 4096);
    size_t user_conf_start = 0;
    size_t user_conf_end = 0;
    size_t default_conf_start = 0;
    size_t default_conf_end = 0;

    // try to read %APPDATA%\splintermail\splintermail.conf
    char* app_data = getenv("APPDATA");
    if(app_data){
        PROP(&e, FMT(&user_conf, "%x/splintermail/splintermail.conf", FS(app_data)) );
        if(harness.file_r_access(user_conf.data)){
            user_conf_start = config_text->len;
            PROP(&e, dstr_read_file(user_conf.data, config_text) );
            user_conf_end = config_text->len;
        }
    }

    // try to read splintermail.conf in parent of directory of executable
    char temp[MAX_PATH];
    DWORD dret;
    // first get the filename of the executable
    // https://msdn.microsoft.com/en-us/library/windows/desktop/ms683197(v=vs.85).aspx
    dret = GetModuleFileNameA(NULL, temp, sizeof(temp));
    if(dret == 0){
        fprintf(stderr, "unable to get name of executable\n");
        ORIG(&e, E_VALUE, "unable to get name of executable");
    }
    // then get the path from that filename
    HRESULT hret;
    // https://msdn.microsoft.com/en-us/library/windows/desktop/bb773748(v=vs.85).aspx
    hret = PathRemoveFileSpecA(temp);
    if(hret == 0){
        // a 0 return value means nothing was removed, which shouldn't happen
        fprintf(stderr, "unable to get path of executable\n");
        ORIG(&e, E_VALUE, "unable to get path of executable");
    }
    // get the parent directory from that path
    hret = PathRemoveFileSpecA(temp);
    if(hret == 0){
        // a 0 return value means nothing was removed, which shouldn't happen
        fprintf(stderr, "unable to get path of executable\n");
        ORIG(&e, E_VALUE, "unable to get path of executable");
    }
    PROP(&e, FMT(&default_conf, "%x/splintermail.conf", FS(temp)) );
    if(harness.file_r_access(default_conf.data)){
        default_conf_start = config_text->len;
        PROP(&e, dstr_read_file(default_conf.data, config_text) );
        default_conf_end = config_text->len;
    }

    // now that everything has been read (all memory allocated), let's parse:

    if(user_conf_end - user_conf_start > 0){
        dstr_t subconf = dstr_sub(config_text, user_conf_start, user_conf_end);
        e2 = conf_parse(&subconf, spec, speclen);
        CATCH(e2, E_ANY){
            DROP_VAR(&e2);
            fprintf(stderr, "error parsing \"%s\", exiting\n", user_conf.data);
            ORIG(&e, E_VALUE, "unable to parse config file");
        }
    }

    if(default_conf_end - default_conf_start > 0){
        dstr_t subconf = dstr_sub(config_text, default_conf_start, default_conf_end);
        e2 = conf_parse(&subconf, spec, speclen);
        CATCH(e2, E_ANY){
            DROP_VAR(&e2);
            fprintf(stderr, "error parsing \"%s\", exiting\n", default_conf.data);
            ORIG(&e, E_VALUE, "unable to parse config file");
        }
    }
    return e;

#endif
}


static derr_t get_os_default_account_dir(dstr_t* account_dir, bool* account_dir_access){
    derr_t e = E_OK;
#ifdef _WIN32
    // Windows default accounts dir is %APPDATA%/splintermail/
    char* appdata = getenv("APPDATA");
    if(appdata){
        PROP(&e, FMT(account_dir, "%x/splintermail", FS(appdata)) );
        *account_dir_access = harness.dir_rw_access(account_dir->data, true);
    }
#else // not _WIN32
#ifdef __APPLE__
    // OSX always sets account_dir to in ~/Library/splintermail
    char* home = getenv("HOME");
    if(home){
        PROP(&e, FMT(account_dir, "%x/Library/splintermail", FS(home)) );
        *account_dir_access = harness.dir_rw_access(account_dir->data, true);
    }
#else
    // Linux default account_dir is $XDG_CACHE_HOME/splintermail
    char* cache = getenv("XDG_CACHE_HOME");
    if(cache){
        PROP(&e, FMT(account_dir, "%x/splintermail", FS(cache)) );
        *account_dir_access = harness.dir_rw_access(account_dir->data, true);
    }else{
        // or $HOME/.cache/splintermail
        char* home = getenv("HOME");
        if(home){
            // first make sure there is a .cache directory
            PROP(&e, FMT(account_dir, "%x/.cache", FS(home)) );
            if(harness.dir_rw_access(account_dir->data, true)){
                // then make sure there is a a splintermail subdir
                PROP(&e, FMT(account_dir, "/splintermail") );
                *account_dir_access = harness.dir_rw_access(account_dir->data, true);
            }
        }
    }
#endif // __APPLE__
#endif // _WIN32
    return e;
}

struct user_search_data_t {
    int* nfolders;
    dstr_t* user;
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
    if(*search_data->nfolders == 0){
        PROP(&e, dstr_copy(file, search_data->user) );
    }
    // this will be an error indication if we reach this more than once
    *search_data->nfolders += 1;
    return e;
}

static derr_t user_prompt(const char* prompt, dstr_t* resp, bool hide){
    derr_t e = E_OK;
    PROP(&e, FFMT(stderr, NULL, prompt) );
    fflush(stderr);
    if(hide){
        PROP(&e, get_password(resp) );
    }else{
        PROP(&e, get_string(resp) );
        // ignore the newline at the end of the string
        while(resp->len && (resp->data[resp->len-1] == '\r'
                            || resp->data[resp->len-1] == '\n'))
            resp->len -= 1;
    }
    return e;
}

// multi-choice prompt, will enforce a valid response
static derr_t prompt_one_of(const char* prompt, char* opts, size_t* ret){
    derr_t e = E_OK;
    dstr_t dopts;
    DSTR_WRAP(dopts, opts, strlen(opts), true);
    DSTR_VAR(temp, 256);
    while(true){
        PROP(&e, user_prompt(prompt, &temp, false) );
        if(temp.len == 1){
            // check if the character we got in response is a valid option
            LIST_VAR(dstr_t, patterns, 1);
            LIST_APPEND(dstr_t, &patterns, temp);
            char* pos = dstr_find(&dopts, &patterns, NULL, NULL);
            if(pos){
                *ret = (uintptr_t)pos - (uintptr_t)dopts.data;
                break;
            }
        }
        PROP(&e, FFMT(stderr, NULL, "Response must be one of [%x]\n", FS(opts)) );
    }
    return e;
}

static derr_t check_api_token_register(const dstr_t* account_dir,
                                       const dstr_t* user, bool* do_reg){
    derr_t e = E_OK;
    derr_t e2;
    *do_reg = false;
    // check if the user indicated they never want to register
    DSTR_VAR(temp, 4096);
    PROP(&e, FMT(&temp, "%x/%x/noregister", FD(account_dir), FD(user)) );
    // if a noregister file exists, don't do anything
    if(harness.file_r_access(temp.data)){
        LOG_DEBUG("found noregister for user %x, not registering\n", FD(user));
        return e;
    }
    // prompt user for what to do
    static const char* prompt =
        "Register an API token for this device (for password-free access)?\n"
        "[y (yes) / n (not now) / e (not ever)]:";
    size_t ret = 99;
    PROP(&e, prompt_one_of(prompt, "yne", &ret) );
    switch(ret){
        // "yes"
        case 0:
            // make sure the user directory exists
            temp.len = 0;
            PROP(&e, FMT(&temp, "%x/%x", FD(account_dir), FD(user)) );
            if(!harness.dir_w_access(temp.data, true)){
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
            temp.len = 0;
            PROP(&e, FMT(&temp, "%x/%x", FD(account_dir), FD(user)) );
            if(!harness.dir_w_access(temp.data, true)){
                LOG_DEBUG("no write access to save noregister; doing nothing\n");
                return e;
            }
            // now write to the noregister file
            PROP(&e, FMT(&temp, "/noregister") );
            DSTR_STATIC(empty_dstr, "");
            e2 = dstr_write_file(temp.data, &empty_dstr);
            CATCH(e2, E_ANY){
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
    string_builder_t ditm_user = sb_append(base, FD(file));
    string_builder_t src = sb_append(&ditm_user, FS("device.pem"));
    bool ok;
    PROP(&e, exists_path(&src, &ok) );
    if(!ok) return e;

    // check if a migration is unnecessary
    string_builder_t citm_path = sb_append(base, FS("citm"));
    string_builder_t citm_user = sb_append(&citm_path, FD(file));
    string_builder_t user_keys = sb_append(&citm_user, FS("keys"));
    string_builder_t dst = sb_append(&user_keys, FS("mykey.pem"));
    PROP(&e, exists_path(&dst, &ok) );
    if(ok) return e;

    // do the migration
    PROP(&e, mkdirs_path(&user_keys, 0700) );
    PROP(&e, file_copy_path(&src, &dst, 0600) );

    return e;
}

static derr_t api_command_main(
    const opt_spec_t o_account_dir,
    const opt_spec_t o_user,
    int newargc,
    char **argv,
    const char *rhost,
    unsigned int api_port,
    int *retval
){
    dstr_t account_dir = {0};
    dstr_t user = {0};
    dstr_t creds_path = {0};
    dstr_t argument_var = {0};
    dstr_t password = {0};
    dstr_t new_password = {0};
    dstr_t confirm_password = {0};

    derr_t e = E_OK;

    // account_dir option
    bool account_dir_access = false;
    if(o_account_dir.found){
        PROP_GO(&e, FMT(&account_dir, "%x", FD(&o_account_dir.val)), cu);
        // make sure we can access the account_dir
        account_dir_access = harness.dir_rw_access(account_dir.data, true);
    }else{
        // default is determined by OS and environment variables
        PROP_GO(&e,
            get_os_default_account_dir(&account_dir, &account_dir_access),
        cu);
    }

    if(!account_dir_access){
        DROP_CMD(
            FFMT(stderr, NULL,
                "account directory %x not found or not "
                "accessible; API token access disabled\n",
                FD(&account_dir)
            )
        );
    }

    // figure out who our user is
    bool user_found = false;
    if(o_user.found){
        PROP_GO(&e, FMT(&user, "%x", FD(&o_user.val)), cu);
        user_found = true;
    }else if(account_dir_access){
        // verify that we can auto-decide who the user is
        int nfolders = 0;
        // wrap "user" and "nfolders" in struct for the "for_each_file" hook
        struct user_search_data_t search_data = {&nfolders, &user};
        // loop through files in the folder in a platform-independent way
        string_builder_t sb = SB(FD(&account_dir));
        PROP_GO(&e,
            harness.for_each_file_in_dir(
                &sb, user_search_hook, (void*)&search_data
            ),
        cu);
        // make sure we got a username
        if(nfolders == 1){
            user_found = true;
        }
    }
    if(!user_found){
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
        PROP_GO(&e, FMT(&creds_path, "%x/%x", FD(&account_dir), FD(&user)), cu);
        // if the folder doesn't exist, we know we can make it
        if(!harness.exists(creds_path.data)){
            user_dir_access = true;
            can_register = true;
        // if it does exist we must have rw access to it
        }else if(harness.dir_rw_access(creds_path.data, false)){
            user_dir_access = true;
        }else{
            DROP_CMD(
                FFMT(stderr, NULL,
                    "Insufficient permissions for user directory %x; "
                    "API token access disabled\n", FD(&creds_path)
                )
            );
        }
    }

    // try to load the user's api_token
    api_token_t token = {0}; // MSVC wrongly complains about uninit'd variable
    bool creds_found = false;  // have good creds?
    if(user_dir_access){
        // complete the creds_path
        PROP_GO(&e, FMT(&creds_path, "/api_token.json"), cu);
        // check if the file already exists
        if(harness.exists(creds_path.data)){
            // check if we have RW access to it
            if(harness.file_rw_access(creds_path.data)){
                /* no need to set can_register = true because we aren't
                   auto-deleting (or overwriting) bad files here. */
                // can_register = true;
                // now see if we have a good token on file
                derr_t e2 = api_token_read(creds_path.data, &token);
                CATCH(e2, E_PARAM, E_INTERNAL){
                    DROP_VAR(&e2);
                    // broken token, warn user
                    DROP_CMD( FFMT(stderr, NULL,
                         "api token at \"%x\" appears invalid; ignoring it.\n",
                         FD(&creds_path)) );
                }else CATCH(e2, E_ANY){
                    DROP_VAR(&e2);
                    // NOMEM is about the only plausible error we could get here
                    DROP_CMD( FFMT(stderr, NULL,
                         "unexpected error reading api token at \"%x\"; "
                         "disabling API token access.\n", FD(&creds_path)) );
                }else{
                    creds_found = true;
                }
            }else{
                // file exists, but we have no access to it
                DROP_CMD( FFMT(stderr, NULL,
                     "Insufficient permissions for %x; "
                     "API token access disabled\n", FD(&creds_path)) );
                /* no need to set can_register = false here, because it's
                   impossible to arrive here with can_register == true */
            }
        }else{
            can_register = true;
        }
    }

    // get the api command
    dstr_t command;
    DSTR_WRAP(command, argv[1], strlen(argv[1]), true);
    // get the argument if it exists
    dstr_t* argument = NULL;
    if(newargc > 2){
        dstr_t argv2;
        DSTR_WRAP(argv2, argv[2], strlen(argv[2]), true);
        PROP_GO(&e, json_encode(&argv2, &argument_var), cu);
        argument = &argument_var;
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
        PROP_GO(&e, user_prompt("Old Splintermail.com Account Password:", &password, true), cu);
        PROP_GO(&e, user_prompt("New Password:", &new_password, true), cu);
        PROP_GO(&e, user_prompt("Confirm Password:", &confirm_password, true), cu);
        // make sure confirmation was valid
        if(dstr_cmp(&new_password, &confirm_password) != 0){
            DROP_CMD( FFMT(stderr, NULL, "Password confirmation failed.\n") );
            *retval = 6;
            goto cu;
        }
        // set the argument for the API call
        argument_var.len = 0;
        PROP_GO(&e, json_encode(&new_password, &argument_var), cu);
        argument = &argument_var;
    }else if(need_password){
        PROP_GO(&e, user_prompt("Splintermail.com Account Password:", &password, true), cu);
    }

    // now check if we should register for an API token:
    if(can_register && !creds_found){
        bool do_reg;
        PROP_GO(&e, check_api_token_register(&account_dir, &user, &do_reg), cu);
        if(do_reg){
            // do the registration
            derr_t e2 = register_api_token(
                rhost, api_port, &user, &password, creds_path.data
            );
            CATCH(e2, E_ANY){
                DROP_VAR(&e2);
                LOG_ERROR("failed to register API token with server\n");
                *retval = 7;
                goto cu;
            }
        }
    }

    // check for commands which require confirmation
    bool need_confirmation = false;
    if(dstr_cmp(&command, &delete_all_mail) == 0) need_confirmation = true;
    if(dstr_cmp(&command, &delete_all_aliases) == 0) need_confirmation = true;
    if(dstr_cmp(&command, &delete_account) == 0) need_confirmation = true;

    if(need_confirmation){
        // prompt for confirmation
        DSTR_STATIC(confirmation, "I really want to do this");
        DROP_CMD( FFMT(stderr, NULL, "`%x` needs confirmation. Type the following text:\n"
             "%x\n", FD(&command), FD(&confirmation)) );
        // get confirmation
        DSTR_VAR(temp, 256);
        PROP_GO(&e, get_string(&temp), cu);
        // ignore the newline at the end of the string
        while(temp.len && (temp.data[temp.len-1] == '\r'
                           || temp.data[temp.len-1] == '\n'))
            temp.len -= 1;
        // verify confirmation
        if(dstr_cmp(&temp, &confirmation) != 0){
            fprintf(stderr, "confirmation failed, aborting.\n");
            *retval = 8;
            goto cu;
        }
    }


    // now we can actually do the API request
    int code;
    DSTR_VAR(reason, 1024);
    DSTR_VAR(recv, 4096);
    LIST_VAR(json_t, json, 256);

    if(need_password){
        PROP_GO(&e,
            api_password_call(
                rhost, api_port, &command, argument, &user, &password, &code,
                &reason, &recv, &json
            ),
        cu);
    }else{
        // update nonce
        token.nonce++;
        PROP_GO(&e, api_token_write(creds_path.data, &token), cu);
        PROP_GO(&e, api_token_call(rhost, api_port, &command, argument, &token,
                                &code, &reason, &recv, &json), cu);
        // check for rejection of API token
        if(code == 401 || code == 403){
            DROP_CMD( FFMT(stderr, NULL, "API Token rejected, deleting token.  Run this "
                               "command again to generate a new token.\n") );
            int ret = compat_unlink(creds_path.data);
            if(ret != 0){
                DROP_CMD( FFMT(stderr, NULL, "Error removing token: %x\n", FE(&errno)) );
            }
            *retval = 9;
            goto cu;
        }
    }


    if(code < 200 || code > 299){
        DROP_CMD( FFMT(stderr, NULL, "api request rejected: %x %x\n", FI(code), FD(&reason)) );
        *retval = 10;
        goto cu;
    }

    json_t jroot = json.data[0];
    dstr_t status;

    // check the status of the returned json
    PROP_GO(&e, j_to_dstr(jk(jroot, "status"), &status), cu);
    DSTR_STATIC(okstr, "success");
    if(dstr_cmp(&status, &okstr) != 0){
        dstr_t contents;
        PROP_GO(&e, j_to_dstr(jk(jroot, "contents"), &contents), cu);
        DROP_CMD( FFMT(stderr, NULL, "REST API call failed: \"%x\"\n", FD(&contents)) );
        DROP_VAR(&e);
        *retval = 11;
        goto cu;
    }

    // now dump the return json
    PROP_GO(&e, json_fdump(stdout, jk(jroot, "contents") ), cu);

    *retval = 0;

cu:
    dstr_free(&account_dir);
    dstr_free(&user);
    dstr_free(&creds_path);
    dstr_free(&argument_var);
    dstr_free(&password);
    dstr_free(&new_password);
    dstr_free(&confirm_password);
    return e;
}

// returns zero when the all options provided are all allowed
static int _limit_options(
    const char *action,
    opt_spec_t **spec,
    size_t speclen,
    opt_spec_t **allowed,
    size_t nallowed
){
    int failed = 0;

    for(size_t i = 0; i < speclen; i++){
        opt_spec_t *opt = spec[i];
        if(!opt->found) goto next_opt;
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
#define limit_options(action, spec, speclen, ok_opts) \
    _limit_options( \
        action, \
        spec, \
        speclen, \
        ok_opts, \
        sizeof(ok_opts) / sizeof(*ok_opts) \
    )

// ugh... abstracting main() feels dirty.  Thanks, Windows.
int do_main(int argc, char* argv[], bool windows_service){
    dstr_t config_text = {0};
    dstr_t logfile_path = {0};
    dstr_t local_host = {0};
    dstr_t local_svc = {0};
    dstr_t cert = {0};
    dstr_t key = {0};
    dstr_t remote_svc = {0};
    dstr_t sm_dir = {0};

    derr_t e = E_OK;
    int retval = 0;
    // ignore SIGPIPE, required to work with OpenSSL
    // see https://mta.openssl.org/pipermail/openssl-users/2017-May/005776.html
    // (but SIGPIPE doesnt exist in windows)
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    // setup the ssl library (application-wide step)
    PROP_GO(&e, ssl_library_init(), fail);

    // set up options to be parsed
    // options independent of subcommand
    opt_spec_t o_help       = {'h',  "help",       false, OPT_RETURN_INIT};
    opt_spec_t o_version    = {'v',  "version",    false, OPT_RETURN_INIT};
    opt_spec_t o_dump_conf  = {'\0', "dump-conf",  false, OPT_RETURN_INIT};
    // common options
    opt_spec_t o_debug      = {'D',  "debug",      false, OPT_RETURN_INIT};
    opt_spec_t o_config     = {'c',  "config",     true,  OPT_RETURN_INIT};
    // ditm options
    opt_spec_t o_pop_port   = {'\0', "pop-port",   true,  OPT_RETURN_INIT};
    opt_spec_t o_ditm_dir   = {'\0', "ditm-dir",   true,  OPT_RETURN_INIT};  // can accept --splintermail-dir
    // citm options
    opt_spec_t o_lstn_port  = {'\0', "listen-port",true,  OPT_RETURN_INIT};
    opt_spec_t o_lstn_addr  = {'\0', "listen-addr",true,  OPT_RETURN_INIT};
    // citm and ditm options
    opt_spec_t o_sm_dir     = {'d',  "splintermail-dir",true,OPT_RETURN_INIT};
    opt_spec_t o_logfile    = {'l',  "logfile",    true,  OPT_RETURN_INIT};
    opt_spec_t o_no_logfile = {'L',  "no-logfile", false, OPT_RETURN_INIT};
    opt_spec_t o_cert       = {'\0', "cert",       true,  OPT_RETURN_INIT};
    opt_spec_t o_key        = {'\0', "key",        true,  OPT_RETURN_INIT};
    // options specific to the api_client
    opt_spec_t o_user       = {'u',  "user",       true,  OPT_RETURN_INIT};
    opt_spec_t o_account_dir= {'a',  "account-dir",true,  OPT_RETURN_INIT};
#ifdef BUILD_DEBUG
    // debug-only options
    opt_spec_t o_r_host     = {'\0', "remote-host",     true, OPT_RETURN_INIT};
    opt_spec_t o_r_pop_port = {'\0', "remote-pop-port", true, OPT_RETURN_INIT};
    opt_spec_t o_r_imap_port= {'\0', "remote-imap-port", true, OPT_RETURN_INIT};
    opt_spec_t o_r_api_port = {'\0', "remote-api-port", true, OPT_RETURN_INIT};
#endif // BUILD_DEBUG

    //                    option               citm  ditm  api_client
    opt_spec_t* spec[] = {&o_help,          // -     -     -
                          &o_version,       // -     -     -
                          &o_dump_conf,     // -     -     -
                          &o_debug,         // y     y     y
                          &o_config,        // y     y     y
                          &o_pop_port,      // n     y     n
                          &o_ditm_dir,      // n     y     n  // can accept o_sm_dir
                          &o_lstn_port,     // y     n     n
                          &o_lstn_addr,     // y     n     n
                          &o_sm_dir,        // y     y     n
                          &o_logfile,       // y     y     n
                          &o_no_logfile,    // y     y     n
                          &o_cert,          // y     y     n
                          &o_key,           // y     y     n
                          &o_user,          // n     n     y
                          &o_account_dir,   // n     n     y
#ifdef BUILD_DEBUG
                          &o_r_host,        // y     y     y
                          &o_r_pop_port,    // n     y     n
                          &o_r_imap_port,   // y     n     y
                          &o_r_api_port,    // n     y     y
#endif // BUILD_DEBUG
                         };
    size_t speclen = sizeof(spec) / sizeof(*spec);
    int newargc;

    // parse options
    derr_t e2 = opt_parse(argc, argv, spec, speclen, &newargc);
    CATCH(e2, E_ANY){
        DROP_VAR(&e2);
        fprintf(stderr, "try `%s --help` for usage\n", argv[0]);
        retval = 1;
        goto cu;
    }

    // help option
    if(o_help.found){
        print_help();
        retval = 0;
        goto cu;
    }

    // version option
    if(o_version.found){
        printf("%d.%d.%d\n", DITM_VERSION_MAJOR, DITM_VERSION_MINOR,
                             DITM_VERSION_BUILD);
        retval = 0;
        goto cu;
    }


    /* dump_conf option should not be respected if it is found in a config file
       so we store the after-command-line-parsing value now but dump later */
    bool should_dump_config = (o_dump_conf.found != 0);

    if(!should_dump_config){
        /* limit options before loading the config, so the config can have
           options useful to multiple commands without causing errors */
        if((newargc > 1 && strcmp("citm", argv[1]) == 0)){
            opt_spec_t *ok_opts[] = {
                &o_debug, &o_config,
                #ifdef BUILD_DEBUG
                &o_r_host, &o_r_imap_port,
                #endif
                &o_lstn_port, &o_lstn_addr, &o_sm_dir, &o_logfile,
                &o_no_logfile, &o_cert, &o_key,
            };
            bool failed = limit_options(
                "splintermail citm", spec, speclen, ok_opts
            );
            if(failed){
                retval = 1;
                goto cu;
            }
        }else if(newargc > 1 && strcmp("ditm", argv[1]) == 0){
            opt_spec_t *ok_opts[] = {
                &o_debug, &o_config,
                #ifdef BUILD_DEBUG
                &o_r_host, &o_r_pop_port, &o_r_api_port,
                #endif
                &o_pop_port, &o_ditm_dir, &o_sm_dir, &o_logfile, &o_no_logfile,
                &o_cert, &o_key
            };
            bool failed = limit_options(
                "splintermail ditm", spec, speclen, ok_opts
            );
            if(failed){
                retval = 1;
                goto cu;
            }
        }else{
            opt_spec_t *ok_opts[] = {
                &o_debug, &o_config,
                #ifdef BUILD_DEBUG
                &o_r_api_port,
                #endif
                &o_user, &o_account_dir
            };
            bool failed = limit_options(
                "splintermail api commands", spec, speclen, ok_opts
            );
            if(failed){
                retval = 1;
                goto cu;
            }
        }
    }

    // load up config files
    if(o_config.found){
        // this should never fail because o_config.val comes from argv
        PROP_GO(&e, dstr_null_terminate(&o_config.val), cu);
        // if `-c` or `--config` was specified, load that only
        if(harness.file_r_access(o_config.val.data) == false){
            fprintf(stderr, "unable to access config file \"%s\"\n",
                            o_config.val.data);
            retval = 2;
            goto cu;
        }
        PROP_GO(&e, dstr_read_file(o_config.val.data, &config_text), cu);
        PROP_GO(&e, conf_parse(&config_text, spec, speclen), cu);
    }else{
        // if no `-c` or `--config`, load the OS-specific file locations
        PROP_GO(&e, load_os_config_files(&config_text, spec, speclen), cu);
    }

    // if we had --dump_conf on the command line, this is where we dump config
    if(should_dump_config){
        opt_fdump(spec, speclen, stdout, NULL);
        retval = 0;
        goto cu;
    }

    // in all other cases we need more than one argument
    if(!windows_service && newargc < 2){
        fprintf(stderr, "you must specify \"citm\" or an api command\n");
        fprintf(stderr, "try `%s --help` for usage\n", argv[0]);
        retval = 3;
        goto cu;
    }

    // debug printing
    log_level_t log_level = o_debug.found ? LOG_LVL_DEBUG : LOG_LVL_INFO;

    // always print to stderr
    logger_add_fileptr(log_level, stderr);

    // --splintermail-dir option
    if(o_sm_dir.found){
        PROP_GO(&e, FMT(&sm_dir, "%x", FD(&o_sm_dir.val)), cu);
    }else if(o_ditm_dir.found){
        // --ditm-dir used to have -d (now --splintermail-dir does)
        PROP_GO(&e, FMT(&sm_dir, "%x", FD(&o_ditm_dir.val)), cu);
    }else{
        PROP_GO(&e, FMT(&sm_dir, "%x", FD(&os_default_sm_dir)), cu);
    }
    string_builder_t sm_dir_path = SB(FD(&sm_dir));

#ifdef BUILD_DEBUG
    // debug options
    DSTR_VAR(r_host_d, 256);
    if(o_r_host.found){
        PROP_GO(&e, FMT(&r_host_d, "%x", FD(&o_r_host.val)), cu);
    }else{
        PROP_GO(&e, FMT(&r_host_d, "splintermail.com"), cu);
    }
    const char* rhost = r_host_d.data;

    unsigned int api_port = 443;
    if(o_r_api_port.found){
        PROP_GO(&e, dstr_tou(&o_r_api_port.val, &api_port, 10), cu);
    }

    unsigned int pop_port = 995;
    if(o_r_pop_port.found){
        PROP_GO(&e, dstr_tou(&o_r_pop_port.val, &pop_port, 10), cu);
    }

    unsigned int imap_port = 993;
    if(o_r_imap_port.found){
        PROP_GO(&e, dstr_tou(&o_r_imap_port.val, &imap_port, 10), cu);
    }
#else
    const char* rhost = "splintermail.com";
    unsigned int api_port = 443;
    unsigned int pop_port = 995;
    unsigned int imap_port = 993;
#endif // BUILD_DEBUG

    //////////////// handle the citm-specific options

    if(windows_service || strcmp("citm", argv[1]) == 0){
        // then print to a log file, unless --no-logfile is specifed
        if(o_logfile.found > o_no_logfile.found){
            PROP_GO(&e, FMT(&logfile_path, "%x", FD(&o_logfile.val)), cu);
            logger_add_filename(log_level, logfile_path.data);
        }
        // log file defaults to on, in ${sm_dir}/ditm_log
        else if(o_logfile.found == 0 && o_no_logfile.found == 0){
            PROP_GO(&e, FMT(&logfile_path, "%x/citm_log", FD(&sm_dir)), cu);
            logger_add_filename(log_level, logfile_path.data);
        }

        if(o_lstn_addr.found){
            PROP_GO(&e, FMT(&local_host, "%x", FD(&o_lstn_addr.val)), cu);
        }else{
            PROP_GO(&e, FMT(&local_host, "127.0.0.1"), cu);
        }

        if(o_lstn_port.found){
            PROP_GO(&e, FMT(&local_svc, "%x", FD(&o_lstn_port.val)), cu);
        }else{
            PROP_GO(&e, FMT(&local_svc, "1993"), cu);
        }

        // get certificate path
        if(o_cert.found){
            PROP_GO(&e, FMT(&cert, "%x", FD(&o_cert.val)), cu);
        }else{
            // look for the default certificate
            bool ok;
            string_builder_t path = sb_append(
                &sm_dir_path, FS("citm-127.0.0.1-cert.pem")
            );
            PROP_GO(&e, exists_path(&path, &ok), cu);
            if(!ok){
                // detect the pre-citm certificate, if present
                path = sb_append(&sm_dir_path, FS("ditm-127.0.0.1-cert.pem"));
                PROP_GO(&e, exists_path(&path, &ok), cu);
            }
            if(!ok){
                DROP_CMD(
                    FFMT(stderr, NULL,
                        "did not find certificate file in %x, "
                        "please re-install splintermail or provide a "
                        "certificate explictly via --cert\n",
                        FD(&sm_dir)
                    )
                );
                retval = 12;
                goto cu;
            }
            PROP_GO(&e, sb_to_dstr(&path, &DSTR_LIT("/"), &cert), cu);
        }

        // get key path
        if(o_key.found){
            PROP_GO(&e, FMT(&key, "%x", FD(&o_key.val)), cu);
        }else{
            // look for the default key
            bool ok;
            string_builder_t path = sb_append(
                &sm_dir_path, FS("citm-127.0.0.1-key.pem")
            );
            PROP_GO(&e, exists_path(&path, &ok), cu);
            if(!ok){
                // detect the pre-citm key, if present
                path = sb_append(&sm_dir_path, FS("ditm-127.0.0.1-key.pem"));
                PROP_GO(&e, exists_path(&path, &ok), cu);
            }
            if(!ok){
                DROP_CMD(
                    FFMT(stderr, NULL,
                        "did not find key file in %x, please re-install "
                        "splintermail or provide a key explictly via --key\n",
                        FD(&sm_dir)
                    )
                );
                retval = 13;
                goto cu;
            }
            PROP_GO(&e, sb_to_dstr(&path, &DSTR_LIT("/"), &key), cu);
        }

        // migrate pre-citm device keys for use with citm
        PROP_GO(&e,
            for_each_file_in_dir(&sm_dir_path, migrate_ditm_keys_hook, NULL),
        cu);

        PROP_GO(&e, FMT(&remote_svc, "%x", FU(imap_port)), cu);

        string_builder_t citm_path = sb_append(&sm_dir_path, FS("citm"));

#ifdef _WIN32
        if(windows_service == true){
            ReportSvcStatus( SERVICE_RUNNING, NO_ERROR, 0 );
        }
#endif

        PROP_GO(&e,
            citm(
                local_host.data,
                local_svc.data,
                key.data,
                cert.data,
                NULL,  // const char *dh
                rhost,
                remote_svc.data,
                &citm_path,
                false  // bool indicate_ready
            ),
        cu);

        retval = 0;

        goto cu;
    }

    //////////////// keep legacy ditm behavior as a fallback, for now

    if(strcmp("ditm", argv[1]) == 0){

        // port option
        unsigned int port = 1995;
        if(o_pop_port.found){
            // intelligently convert port to a number
            e2 = dstr_tou(&o_pop_port.val, &port, 10);
            if(is_error(e2) || port < 1 || port > 65535){
                DROP_VAR(&e2);
                fprintf(stderr, "invalid port number\n");
                fprintf(stderr, "try `%s --help` for usage\n", argv[0]);
                retval = 4;
                goto cu;
            }
        }

        // then print to a log file, unless --no-logfile is specifed
        if(o_logfile.found > o_no_logfile.found){
            PROP_GO(&e, FMT(&logfile_path, "%x", FD(&o_logfile.val)), cu);
            logger_add_filename(log_level, logfile_path.data);
        }
        // log file defaults to on, in ${sm_dir}/ditm_log
        else if(o_logfile.found == 0 && o_no_logfile.found == 0){
            PROP_GO(&e, FMT(&logfile_path, "%x/ditm_log", FD(&sm_dir)), cu);
            logger_add_filename(log_level, logfile_path.data);
        }

        // get certificate path
        char* cert_arg = NULL;
        if(o_cert.found){
            PROP_GO(&e, FMT(&cert, "%x", FD(&o_cert.val)), cu);
            cert_arg = cert.data;
        }

        // get key path
        char* key_arg = NULL;
        if(o_key.found){
            PROP_GO(&e, FMT(&key, "%x", FD(&o_key.val)), cu);
            key_arg = key.data;
        }

        PROP_GO(&e, ditm_loop(rhost, pop_port, sm_dir.data, port,
                          rhost, api_port, cert_arg, key_arg), cu);

        retval = 0;
        goto cu;
    }

    //////////////// now handle the api client options


    PROP_GO(&e,
        api_command_main(
            o_account_dir, o_user, newargc, argv, rhost, api_port, &retval
        ),
    cu);

    //////////////// now clean up

cu:
    ssl_library_close();
fail:

    // if we have an uncaught error return 127
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
        retval = 127;
    }

    // free memory after DUMP, since logfile_path will be read during DUMP
    dstr_free(&config_text);
    dstr_free(&logfile_path);
    dstr_free(&local_host);
    dstr_free(&local_svc);
    dstr_free(&cert);
    dstr_free(&key);
    dstr_free(&remote_svc);
    dstr_free(&sm_dir);

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
