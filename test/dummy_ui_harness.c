#include <stdlib.h>
#include <string.h>

#include "dummy_ui_harness.h"

#include <libdstr/libdstr.h>
#include <ditm.h>
#include <networking.h>
#include <console_input.h>
#include <ui_harness.h>

// ditm.h
bool looked_good;
dstr_t reason_log;
struct ditm_loop_args_t* ditm_loop_args;
bool ditm_called;
derr_t ditm_loop(const char* rhost, unsigned int rport,
                 const char* ditm_dir, unsigned int port,
                 const char* api_host, unsigned int api_port,
                 const char* certpath, const char* keypath){
    derr_t e = E_OK;
    if(ditm_loop_args == NULL){
        UH_OH("ditm_loop called but nothing is prepared\n");
        ORIG(&e, E_INTERNAL, "bad ditm_loop");
    }
    // compare args against global "right answer" values:
    struct ditm_loop_args_t* DLA = ditm_loop_args;
    if(DLA->rhost && strcmp(DLA->rhost, rhost) != 0)
        UH_OH("DLA rhost exp '%x' but got '%x'\n", FS(DLA->rhost), FS(rhost));
    if(DLA->rport && DLA->rport != rport)
        UH_OH("DLA rport exp '%x' but got '%x'\n", FU(DLA->rport), FU(rport));
    if(DLA->ditm_dir && strcmp(DLA->ditm_dir, ditm_dir) != 0)
        UH_OH("DLA ditm_dir exp '%x' but got '%x'\n", FS(DLA->ditm_dir), FS(ditm_dir));
    if(DLA->port && DLA->port != port)
        UH_OH("DLA port exp '%x' but got '%x'\n", FU(DLA->port), FU(port));
    if(DLA->api_host && strcmp(DLA->api_host, api_host) != 0)
        UH_OH("DLA api_host exp '%x' but got '%x'\n", FS(DLA->api_host), FS(api_host));
    if(DLA->api_port && DLA->api_port != api_port)
        UH_OH("DLA api_port exp '%x' but got '%x'\n", FU(DLA->api_port), FU(api_port));
    if((DLA->cert == NULL) != (certpath == NULL)){
        UH_OH("DLA cert exp %x but got %x\n", FP(DLA->cert), FP(certpath));
    }else if(DLA->cert && strcmp(DLA->cert, certpath) != 0)
        UH_OH("DLA cert exp '%x' but got '%x'\n", FS(DLA->cert), FS(certpath));
    if((DLA->key == NULL) != (keypath == NULL)){
        UH_OH("DLA key exp %x but got %x\n", FP(DLA->key), FP(keypath));
    }else if(DLA->key && strcmp(DLA->key, keypath) != 0)
        UH_OH("DLA key exp '%x' but got '%x'\n", FS(DLA->key), FS(keypath));
    ditm_called = true;
    return DLA->to_return;
}

// fileops.h
// a list of folders which will be created
char** creatables;

enum access_mode_t {
    AM_EXIST,
    AM_ACCESS,
    AM_CREATE,
};

static inline bool fake_access(const char* path, enum access_mode_t mode){
    static const char* off_limits[] = {"splintermail.conf",
                                       "fake_file",
                                       "no.perms.user@fqdn",
                                       "no.creds.access.user@fqdn/api_token.json",
                                       };
    size_t plen = strlen(path);
    // check if file is forbidden
    bool is_off_limits = false;
    for(size_t i = 0; i < sizeof(off_limits)/sizeof(*off_limits); i++){
        size_t flen = strlen(off_limits[i]);
        if(plen >= flen){
            const char* end = path + (plen - flen);
            if(strcmp(end, off_limits[i]) == 0){
                is_off_limits = true;
                break;
            }
        }
    }
    if(is_off_limits){
        // file exists, but otherwise no access
        return mode == AM_EXIST;
    }
    if(creatables){
        // check if file is creatable
        char** c_match = NULL;
        for(char** c = creatables; *c != NULL ; c++){
            // check if this is a match
            size_t clen = strlen(*c);
            if(plen >= clen){
                const char* end = path + (plen - clen);
                if(strcmp(end, *c) == 0){
                    c_match = c;
                    break;
                }
            }
        }
        // check if we matched to a creatable
        if(c_match){
            if(mode == AM_EXIST) return false;
            if(mode == AM_ACCESS) return false;
            if(mode == AM_CREATE){
                // remove from list of creatables
                for(char** c = c_match; *c != NULL; c++){
                    *c = *(c + 1);
                }
                return true;
            }
        }
    }
    // otherwise assume all files exist and are readable
    return true;
}

static bool fake_dir_r_access(const char* path, bool create){
    return fake_access(path, create ? AM_CREATE : AM_ACCESS);
}
static bool fake_dir_w_access(const char* path, bool create){
    return fake_access(path, create ? AM_CREATE : AM_ACCESS);
}
static bool fake_dir_rw_access(const char* path, bool create){
    return fake_access(path, create ? AM_CREATE : AM_ACCESS);
}
static bool fake_file_r_access(const char* path){
    return fake_access(path, AM_ACCESS);
}
static bool fake_file_w_access(const char* path){
    return fake_access(path, AM_ACCESS);
}
static bool fake_file_rw_access(const char* path){
    return fake_access(path, AM_ACCESS);
}
static bool fake_exists(const char* path){
    return fake_access(path, AM_EXIST);
}

char** users;
static derr_t fake_for_each_file_in_dir(
    const char* path, for_each_file_hook_t hook, void* userdata
){
    (void) path;
    derr_t e = E_OK;
    if(users == NULL){
        UH_OH("unexpected call to for_each_file_in_dir\n");
        ORIG(&e, E_INTERNAL, "unexpected call to for_each_file_in_dir");
    }
    for(char** u = users; *u != NULL; u++){
        dstr_t temp;
        DSTR_WRAP(temp, *u, strlen(*u), true);
        PROP(&e, hook(NULL, &temp, true, userdata) );
    }
    return e;
}

// intercept all calls
ui_harness_t harness = {
    .dir_r_access = fake_dir_r_access,
    .dir_w_access = fake_dir_w_access,
    .dir_rw_access = fake_dir_rw_access,
    .file_r_access = fake_file_r_access,
    .file_w_access = fake_file_w_access,
    .file_rw_access = fake_file_rw_access,
    .exists = fake_exists,
    .for_each_file_in_dir = fake_for_each_file_in_dir,
};

// networking.h
derr_t ssl_library_init(void){
    derr_t e = E_OK;
    return e;
}

void ssl_library_close(void){
    return;
}

// api_client.h
api_token_t* token_to_read;
bool find_token;
derr_t read_token_error = E_OK;
derr_t api_token_read(const char* path, api_token_t* token){
    (void)path;
    derr_t e = E_OK;
    if(find_token == false){
        // returning any error causes the credentials to be ignored
        return (derr_t){.type = E_INTERNAL};
    }
    if(read_token_error.type != E_NONE){
        return read_token_error;
    }
    if(token_to_read == NULL){
        UH_OH("unexpected call to api_token_read\n");
        ORIG(&e, E_INTERNAL, "unexpected call to api_token_read");
    }
    token->key = token_to_read->key;
    DSTR_WRAP_ARRAY(token->secret, token->secret_buffer);
    dstr_copy(&token_to_read->secret, &token->secret);
    token->nonce = token_to_read->nonce;
    return e;
}
// never write anything
derr_t api_token_write(const char* path, api_token_t* token){
    (void) path;
    (void) token;
    derr_t e = E_OK;
    return e;
}

struct register_token_args_t* register_token_args;
bool register_token_called;
derr_t register_api_token(const char* host,
                          unsigned int port,
                          const dstr_t* user,
                          const dstr_t* pass,
                          const char* creds_path){
    derr_t e = E_OK;
    if(register_token_args == NULL){
        UH_OH("register_api_token called but nothing is prepared\n");
        ORIG(&e, E_INTERNAL, "bad register_api_token");
    }
    struct register_token_args_t* RTA = register_token_args;
    if(RTA->host && strcmp(RTA->host, host) != 0)
        UH_OH("RTA host exp '%x' but got '%x'\n", FS(RTA->host), FS(host));
    if(RTA->port && RTA->port != port)
        UH_OH("RTA port exp '%x' but got '%x'\n", FU(RTA->port), FU(port));
    if(RTA->user && dstr_cmp(RTA->user, user) != 0)
        UH_OH("RTA user exp '%x' but got '%x'\n", FD(RTA->user), FD(user));
    if(RTA->pass && dstr_cmp(RTA->pass, pass) != 0)
        UH_OH("RTA pass exp '%x' but got '%x'\n", FD(RTA->pass), FD(pass));
    if(RTA->creds_path && strcmp(RTA->creds_path, creds_path) != 0)
        UH_OH("RTA creds_path exp '%x' but got '%x'\n", FS(RTA->creds_path), FS(creds_path));
    register_token_called = true;
    return RTA->to_return;
}

struct api_password_args_t* api_password_args;
bool api_password_called;
derr_t api_password_call(const char* host, unsigned int port, dstr_t* command,
                         dstr_t* arg, const dstr_t* username,
                         const dstr_t* password, int* code, dstr_t* reason,
                         dstr_t* recv, LIST(json_t)* json){
    derr_t e = E_OK;
    if(api_password_args == NULL){
        UH_OH("api_password_call called but nothing is prepared\n");
        ORIG(&e, E_INTERNAL, "bad api_password_call");
    }
    (void)recv;
    struct api_password_args_t* APA = api_password_args;
    if(APA->host && strcmp(APA->host, host) != 0)
        UH_OH("APA host exp '%x' but got '%x'\n", FS(APA->host), FS(host));
    if(APA->port && APA->port != port)
        UH_OH("APA port exp '%x' but got '%x'\n", FU(APA->port), FU(port));
    if(APA->command && dstr_cmp(APA->command, command) != 0)
        UH_OH("APA command exp '%x' but got '%x'\n", FD(APA->command), FD(command));
    if(APA->arg && dstr_cmp(APA->arg, arg) != 0){
        UH_OH("APA arg exp '%x' but got '%x'\n", FD(APA->arg), FD(arg));
    }else if(arg)
        UH_OH("APA expected null arg but got '%x'\n", FD(arg));
    if(APA->user){
        dstr_t duser;
        DSTR_WRAP(duser, APA->user, strlen(APA->user), true);
        if(dstr_cmp(&duser, username) != 0)
            UH_OH("APA username exp '%x' but got '%x'\n", FS(APA->user), FD(username));
    }
    if(APA->pass){
        dstr_t dpass;
        DSTR_WRAP(dpass, APA->pass, strlen(APA->pass), true);
        if(dstr_cmp(&dpass, password) != 0)
            UH_OH("APA password exp '%x' but got '%x'\n", FS(APA->pass), FD(password));
    }
    // load up the inputs from the arg struct
    *code = APA->code;
    PROP(&e, FMT(reason, "%x", FS(APA->reason)) );
    // wrap the json string in text and parse it
    dstr_t text;
    DSTR_WRAP(text, APA->json, strlen(APA->json), true);
    json_parse(json, &text);
    api_password_called = true;
    return e;
}

struct api_token_args_t* api_token_args;
bool api_token_called;
derr_t api_token_call(const char* host, unsigned int port, dstr_t* command,
                      dstr_t* arg, api_token_t* token, int* code,
                      dstr_t* reason, dstr_t* recv, LIST(json_t)* json){
    derr_t e = E_OK;
    if(api_token_args == NULL){
        UH_OH("api_token_call called but nothing is prepared\n");
        ORIG(&e, E_INTERNAL, "bad api_token call");
    }
    (void)recv;
    struct api_token_args_t* ATA = api_token_args;
    if(ATA->host && strcmp(ATA->host, host) != 0)
        UH_OH("ATA host exp '%x' but got '%x'\n", FS(ATA->host), FS(host));
    if(ATA->port && ATA->port != port)
        UH_OH("ATA port exp '%x' but got '%x'\n", FU(ATA->port), FU(port));
    if(ATA->command && dstr_cmp(ATA->command, command) != 0)
        UH_OH("ATA command exp '%x' but got '%x'\n", FD(ATA->command), FD(command));
    if(ATA->arg && dstr_cmp(ATA->arg, arg) != 0){
        UH_OH("ATA arg exp '%x' but got '%x'\n", FD(ATA->arg), FD(arg));
    }else if(arg)
        UH_OH("ATA expected null arg but got '%x'\n", FD(arg));
    if(ATA->token.key != token->key)
        UH_OH("ATA tkn->key exp '%x' but got '%x'\n", FU(ATA->token.key), FU(token->key));
    if(dstr_cmp(&ATA->token.secret, &token->secret) != 0)
        UH_OH("ATA tkn->secret exp '%x' but got '%x'\n", FD(&ATA->token.secret), FD(&token->secret));
    if(ATA->token.nonce != token->nonce)
        UH_OH("ATA tkn->nonce exp '%x' but got '%x'\n", FU(ATA->token.nonce), FU(token->nonce));
    // load up the inputs from the arg struct
    *code = ATA->code;
    PROP(&e, FMT(reason, "%x", FS(ATA->reason)) );
    // wrap the json string in text and parse it
    dstr_t text;
    DSTR_WRAP(text, ATA->json, strlen(ATA->json), true);
    json_parse(json, &text);
    api_token_called = true;
    return e;
}


// console_input.h
char** passwords;
derr_t get_password(dstr_t* password){
    derr_t e = E_OK;
    password->len = 0;
    if(passwords == NULL || passwords[0] == NULL){
        UH_OH("unexpected call to get_password\n");
        ORIG(&e, E_INTERNAL, "unexpected call to get_password");
    }
    // return the first password
    PROP(&e, FMT(password, "%x", FS(passwords[0])) );
    // leftshift
    for(char** p = passwords; *p != NULL; p++){
        *p = *(p+1);
    }
    return e;
}

char** strings;
derr_t get_string(dstr_t* input){
    derr_t e = E_OK;
    input->len = 0;
    if(strings == NULL || strings[0] == NULL){
        UH_OH("unexpected call to get_string\n");
        PROP(&e, FMT(input, "FAIL") );
        ORIG(&e, E_INTERNAL, "unexpected call to get_string");
    }
    // return the first input
    PROP(&e, FMT(input, "%x", FS(strings[0])) );
    // leftshift
    for(char** i = strings; *i != NULL; i++){
        *i = *(i+1);
    }
    return e;
}
