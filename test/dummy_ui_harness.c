#include <stdlib.h>
#include <string.h>

#include "dummy_ui_harness.h"

#include <libdstr/libdstr.h>
#include <libcrypto/libcrypto.h>

#include <console_input.h>
#include <ui_harness.h>

#include "test/test_utils.h"

static char cslash = '/';
static dstr_t slash = {
    .data = &cslash, .len = 1, .size = 1, .fixed_size = true
};

bool looked_good;
dstr_t reason_log;

static const char *strend(const char *s, const char *ending){
    // return the last len(ending) characters of s
    if(!ending || !s) return s;
    size_t slen = strlen(s);
    size_t elen = strlen(ending);
    return slen > elen ? s + (slen - elen) : s;
}

#define EXPECT_ADDR(e, name, spec, str) do { \
    dstr_t __got = dstr_from_off(dstr_off_extend(spec.scheme, spec.port)); \
    dstr_t __exp = dstr_from_cstr(str); \
    EXPECT_D(e, name, &__got, &__exp); \
} while(0)

// libcitm/citm.h
citm_args_t *citm_args;
bool citm_called;
derr_t uv_citm(
    const addrspec_t *lspecs,
    size_t nlspecs,
    const addrspec_t remote,
    const char *key,
    const char *cert,
    string_builder_t maildir_root,
    bool indicate_ready
){
    derr_t e = E_OK;

    EXPECT_NOT_NULL(&e, "citm args", citm_args);
    EXPECT_U(&e, "nlspecs", nlspecs, citm_args->nlspecs);
    for(size_t i = 0; i < nlspecs; i++){
        EXPECT_ADDR(&e, "lspec", lspecs[i], citm_args->lspecs[i]);
    }
    fprintf(stderr, "key: %s\n", key);
    EXPECT_S(&e, "key", strend(key, citm_args->key), citm_args->key);
    EXPECT_S(&e, "cert", strend(cert, citm_args->cert), citm_args->cert);
    EXPECT_ADDR(&e, "remote", remote, citm_args->remote);
    DSTR_VAR(buf, 128);
    PROP(&e, FMT(&buf, "%x", FSB(&maildir_root, &slash)) );
    EXPECT_S(&e,
        "maildir_root",
        strend(buf.data, citm_args->maildir_root),
        citm_args->maildir_root
    );
    EXPECT_B(&e, "indicate_ready", indicate_ready, citm_args->indicate_ready);
    EXPECT_B(&e, "citm_called", citm_called, false);
    citm_called = true;

    return citm_args->to_return;
}

// fileops.h
// a list of folders which will be created
char** creatables;

enum access_mode_t {
    AM_EXIST,
    AM_ACCESS,
    AM_CREATE,
};

static inline derr_t fake_access(
    dstr_t path, enum access_mode_t mode, bool *ret
){
    derr_t e = E_OK;

    const dstr_t off_limits[] = {
        DSTR_LIT("splintermail.conf"),
        DSTR_LIT("fake_file"),
        DSTR_LIT("no.perms.user@fqdn"),
        DSTR_LIT("no.creds.access.user@fqdn/api_token.json"),
    };
    // check if file is forbidden
    bool is_off_limits = false;
    for(size_t i = 0; i < sizeof(off_limits)/sizeof(*off_limits); i++){
        dstr_t f = off_limits[i];
        if(dstr_endswith2(path, f)){
            is_off_limits = true;
            break;
        }
    }
    if(is_off_limits){
        // file exists, but otherwise no access
        *ret = mode == AM_EXIST;
        return e;
    }
    if(creatables){
        // check if file is creatable
        char** c_match = NULL;
        for(char** c = creatables; *c != NULL ; c++){
            // check if this is a match
            dstr_t dc = dstr_from_cstr(*c);
            if(dstr_endswith2(path, dc)){
                c_match = c;
                break;
            }
        }
        // check if we matched to a creatable
        if(c_match){
            if(mode == AM_EXIST){
                *ret = false;
                return e;
            }
            if(mode == AM_ACCESS){
                *ret = false;
                return e;
            }
            if(mode == AM_CREATE){
                // remove from list of creatables
                for(char** c = c_match; *c != NULL; c++){
                    *c = *(c + 1);
                }
                *ret = true;
                return e;
            }
        }
    }
    // otherwise assume all files exist and are readable
    *ret = true;
    return e;
}

static derr_t fake_access_path(
    const string_builder_t *sb, enum access_mode_t mode, bool *ret
){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    PROP_GO(&e, fake_access(*path, mode, ret), cu);

cu:
    dstr_free(&heap);
    return e;
}

static derr_t fake_dir_r_access_path(
    const string_builder_t *sb, bool create, bool *ret
){
    return fake_access_path(sb, create ? AM_CREATE : AM_ACCESS, ret);
}
static derr_t fake_dir_w_access_path(
    const string_builder_t *sb, bool create, bool *ret
){
    return fake_access_path(sb, create ? AM_CREATE : AM_ACCESS, ret);
}
static derr_t fake_dir_rw_access_path(
    const string_builder_t *sb, bool create, bool *ret
){
    return fake_access_path(sb, create ? AM_CREATE : AM_ACCESS, ret);
}
static derr_t fake_file_r_access_path(
    const string_builder_t *sb, bool *ret
){
    return fake_access_path(sb, AM_ACCESS, ret);
}
static derr_t fake_file_w_access_path(
    const string_builder_t *sb, bool *ret
){
    return fake_access_path(sb, AM_ACCESS, ret);
}
static derr_t fake_file_rw_access_path(
    const string_builder_t *sb, bool *ret
){
    return fake_access_path(sb, AM_ACCESS, ret);
}
static derr_t fake_exists_path(const string_builder_t *sb, bool *ret){
    return fake_access_path(sb, AM_EXIST, ret);
}

char** users;
static derr_t fake_for_each_file_in_dir(
    const string_builder_t *path, for_each_file_hook_t hook, void* userdata
){
    (void)path;
    derr_t e = E_OK;
    if(users){
        for(char** u = users; *u != NULL; u++){
            dstr_t temp;
            DSTR_WRAP(temp, *u, strlen(*u), true);
            PROP(&e, hook(NULL, &temp, true, userdata) );
        }
        return e;
    }
    // for citm, iteration is for a hook we don't care to mock
    if(citm_args){
        return e;
    }
    UH_OH("unexpected call to for_each_file_in_dir\n");
    ORIG(&e, E_INTERNAL, "unexpected call to for_each_file_in_dir");
}

// intercept all calls
ui_harness_t harness = {
    .dir_r_access_path = fake_dir_r_access_path,
    .dir_w_access_path = fake_dir_w_access_path,
    .dir_rw_access_path = fake_dir_rw_access_path,
    .file_r_access_path = fake_file_r_access_path,
    .file_w_access_path = fake_file_w_access_path,
    .file_rw_access_path = fake_file_rw_access_path,
    .exists_path = fake_exists_path,
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
derr_t read_token_error = {0};
derr_t api_token_read_path(const string_builder_t *sb, api_token_t *token){
    (void)sb;
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
derr_t api_token_write_path(const string_builder_t *sb, api_token_t* token){
    (void)sb;
    (void)token;
    derr_t e = E_OK;
    return e;
}

struct register_token_args_t* register_token_args;
bool register_token_called;
derr_t register_api_token(
    const char* host,
    unsigned int port,
    const dstr_t* user,
    const dstr_t* pass,
    const char* creds_path
){
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

derr_t register_api_token_path(
    const char* host,
    unsigned int port,
    const dstr_t* user,
    const dstr_t* pass,
    const string_builder_t *sb
){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &slash, &stack, &heap, &path) );

    PROP_GO(&e, register_api_token(host, port, user, pass, path->data), cu);

cu:
    dstr_free(&heap);
    return e;

}

struct api_password_args_t* api_password_args;
bool api_password_called;
derr_t api_password_call(const char* host, unsigned int port, dstr_t* command,
                         dstr_t* arg, const dstr_t* username,
                         const dstr_t* password, int* code, dstr_t* reason,
                         dstr_t* recv, json_t *json){
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
    PROP(&e, json_parse(text, json) );
    api_password_called = true;
    return e;
}

struct api_token_args_t* api_token_args;
bool api_token_called;
derr_t api_token_call(const char* host, unsigned int port, dstr_t* command,
                      dstr_t* arg, api_token_t* token, int* code,
                      dstr_t* reason, dstr_t* recv, json_t *json){
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
    PROP(&e, json_parse(text, json) );
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
