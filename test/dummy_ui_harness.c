#include <stdlib.h>
#include <string.h>

#include "dummy_ui_harness.h"

#include <libdstr/libdstr.h>
#include <libcrypto/libcrypto.h>

#include "test/test_utils.h"

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
    EXPECT_D(e, name, __got, __exp); \
} while(0)

// libcitm/citm.h
citm_args_t *citm_args;
bool citm_called;
static derr_t fake_uv_citm(
    const addrspec_t *lspecs,
    size_t nlspecs,
    const addrspec_t remote,
    const char *key,   // explicit --key (disables acme)
    const char *cert,  // explicit --cert (disables acme)
    dstr_t acme_dirurl,
    char *acme_verify_name,  // may be "pebble" in some test scenarios
    dstr_t sm_baseurl,
    SSL_CTX *client_ctx,
    string_builder_t sm_dir,
    // function pointers, mainly for instrumenting tests:
    void (*indicate_ready)(void*, uv_citm_t*),
    void (*user_async_hook)(void*, uv_citm_t*),
    void *user_data
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
    PROP(&e, FMT(&buf, "%x", FSB(sm_dir)) );
    EXPECT_S(&e,
        "sm_dir", strend(buf.data, citm_args->sm_dir), citm_args->sm_dir
    );
    EXPECT_B(&e, "citm_called", citm_called, false);

    // hardcoded args; never configured by the cli
    if(indicate_ready != NULL){
        ORIG(&e, E_VALUE, "expected indicate_ready == NULL");
    }
    if(user_async_hook != NULL){
        ORIG(&e, E_VALUE, "expected user_async_hook == NULL");
    }
    EXPECT_NULL(&e, "user_data", user_data);
    EXPECT_D(&e, "acme_dirurl", acme_dirurl, DSTR_LIT(LETSENCRYPT));
    EXPECT_NULL(&e, "acme_verify_name", acme_verify_name);
    EXPECT_D(&e,
        "sm_baseurl", sm_baseurl, DSTR_LIT("https://splintermail.com")
    );
    EXPECT_NULL(&e, "client_ctx", client_ctx);

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
    PROP(&e, sb_expand(sb, &stack, &heap, &path) );

    PROP_GO(&e, fake_access(*path, mode, ret), cu);

cu:
    dstr_free(&heap);
    return e;
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

// networking.h
static derr_t fake_ssl_library_init(void){
    derr_t e = E_OK;
    return e;
}

static void fake_ssl_library_close(void){
    return;
}

// api_client.h

api_token_t* token_to_read;
derr_t read_token_error = {0};
bool read_token_notok = false;
static derr_t fake_api_token_read_increment_write_path(
    const string_builder_t *sb, api_token_t *token, bool *ok
){
    (void)sb;
    derr_t e = E_OK;
    *ok = !read_token_notok;
    if(read_token_error.type != E_NONE){
        return read_token_error;
    }
    if(token_to_read == NULL){
        UH_OH("unexpected call to api_token_read\n");
        ORIG(&e, E_INTERNAL, "unexpected call to api_token_read");
    }
    *token = (api_token_t){
        .key = token_to_read->key,
        .secret = token_to_read->secret,
        .nonce = token_to_read->nonce,
    };
    return e;
}
static void fake_api_token_free0(api_token_t *token){
    *token = (api_token_t){0};
}

struct register_token_args_t* register_token_args;
bool register_token_called;
static derr_t fake_register_api_token_sync(
    http_sync_t *sync,
    const dstr_t baseurl,
    const dstr_t user,
    const dstr_t pass,
    const char* creds_path
){
    derr_t e = E_OK;
    if(register_token_args == NULL){
        UH_OH("register_api_token called but nothing is prepared\n");
        ORIG(&e, E_INTERNAL, "bad register_api_token");
    }
    (void)sync;
    struct register_token_args_t* RTA = register_token_args;
    if(!dstr_eq(baseurl, dstr_from_cstr(RTA->baseurl)))
        UH_OH("RTA baseurl exp '%x' but got '%x'\n", FS(RTA->baseurl), FD(baseurl));
    if(!dstr_eq(user, dstr_from_cstr(RTA->user)))
        UH_OH("RTA user exp '%x' but got '%x'\n", FS(RTA->user), FD(user));
    if(!dstr_eq(pass, dstr_from_cstr(RTA->pass)))
        UH_OH("RTA pass exp '%x' but got '%x'\n", FS(RTA->pass), FD(pass));
    if(RTA->creds_path && strcmp(RTA->creds_path, creds_path) != 0)
        UH_OH("RTA creds_path exp '%x' but got '%x'\n", FS(RTA->creds_path), FS(creds_path));
    register_token_called = true;
    return RTA->to_return;
}

static derr_t fake_register_api_token_path_sync(
    http_sync_t *sync,
    const dstr_t baseurl,
    const dstr_t user,
    const dstr_t pass,
    const string_builder_t *sb
){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &stack, &heap, &path) );

    PROP_GO(&e,
        fake_register_api_token_sync(sync, baseurl, user, pass, path->data),
    cu);

cu:
    dstr_free(&heap);
    return e;
}

struct api_password_args_t* api_password_args;
bool api_password_called;
static derr_t fake_api_pass_sync(
    http_sync_t *sync,
    const dstr_t baseurl,
    const dstr_t path,
    const dstr_t arg,
    const dstr_t user,
    const dstr_t pass,
    json_t *json
){
    derr_t e = E_OK;
    if(api_password_args == NULL){
        UH_OH("api_password_call called but nothing is prepared\n");
        ORIG(&e, E_INTERNAL, "bad api_password_call");
    }
    (void)sync;
    struct api_password_args_t* APA = api_password_args;
    if(!dstr_eq(baseurl, dstr_from_cstr(APA->baseurl)))
        UH_OH("APA baseurl exp '%x' but got '%x'\n", FS(APA->baseurl), FD(baseurl));
    if(APA->path && !dstr_eq(path, dstr_from_cstr(APA->path)))
        UH_OH("APA command exp '%x' but got '%x'\n", FS(APA->path), FD(path));
    char *exp_arg = APA->arg ? APA->arg : "";
    if(!dstr_eq(arg, dstr_from_cstr(exp_arg)))
        UH_OH("APA arg exp '%x' but got '%x'\n", FS(APA->arg), FD(arg));
    if(!dstr_eq(user, dstr_from_cstr(APA->user)))
        UH_OH("APA user exp '%x' but got '%x'\n", FS(APA->user), FD(user));
    if(!dstr_eq(pass, dstr_from_cstr(APA->pass)))
        UH_OH("APA pass exp '%x' but got '%x'\n", FS(APA->pass), FD(pass));
    // return the specified response
    dstr_t text = dstr_from_cstr(APA->json);
    PROP(&e, json_parse(text, json) );
    api_password_called = true;
    return APA->to_return;
}

struct api_token_args_t* api_token_args;
bool api_token_called;
static derr_t fake_api_token_sync(
    http_sync_t *sync,
    const dstr_t baseurl,
    const dstr_t path,
    const dstr_t arg,
    api_token_t token,
    json_t *json
){
    derr_t e = E_OK;
    if(api_token_args == NULL){
        UH_OH("api_token_call called but nothing is prepared\n");
        ORIG(&e, E_INTERNAL, "bad api_token call");
    }
    (void)sync;
    struct api_token_args_t* ATA = api_token_args;
    if(!dstr_eq(baseurl, dstr_from_cstr(ATA->baseurl)))
        UH_OH("ATA baseurl exp '%x' but got '%x'\n", FS(ATA->baseurl), FD(baseurl));
    if(ATA->path && !dstr_eq(path, dstr_from_cstr(ATA->path)))
        UH_OH("ATA command exp '%x' but got '%x'\n", FS(ATA->path), FD(path));
    char *exp_arg = ATA->arg ? ATA->arg : "";
    if(!dstr_eq(arg, dstr_from_cstr(exp_arg)))
        UH_OH("ATA arg exp '%x' but got '%x'\n", FS(ATA->arg), FD(arg));
    if(ATA->token.key != token.key)
        UH_OH("ATA tkn->key exp '%x' but got '%x'\n", FU(ATA->token.key), FU(token.key));
    if(dstr_cmp(&ATA->token.secret, &token.secret) != 0)
        UH_OH("ATA tkn->secret exp '%x' but got '%x'\n", FD(ATA->token.secret), FD(token.secret));
    if(ATA->token.nonce != token.nonce)
        UH_OH("ATA tkn->nonce exp '%x' but got '%x'\n", FU(ATA->token.nonce), FU(token.nonce));
    // return the specified response
    dstr_t text = dstr_from_cstr(ATA->json);
    PROP(&e, json_parse(text, json) );
    api_token_called = true;
    return ATA->to_return;
}


// console_input.h
char** passwords;
static derr_t fake_get_password(dstr_t* password){
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
static derr_t fake_get_string(dstr_t* input){
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

ui_i dummy_ui_harness(void){
    return (ui_i){
        .dir_w_access_path = fake_dir_w_access_path,
        .dir_rw_access_path = fake_dir_rw_access_path,
        .file_r_access_path = fake_file_r_access_path,
        .file_w_access_path = fake_file_w_access_path,
        .file_rw_access_path = fake_file_rw_access_path,
        .exists_path = fake_exists_path,
        .for_each_file_in_dir = fake_for_each_file_in_dir,
        .ssl_library_init = fake_ssl_library_init,
        .ssl_library_close = fake_ssl_library_close,
        .api_token_read_increment_write_path =
            fake_api_token_read_increment_write_path,
        .api_token_free0 = fake_api_token_free0,
        .register_api_token_path_sync = fake_register_api_token_path_sync,
        .api_pass_sync = fake_api_pass_sync,
        .api_token_sync = fake_api_token_sync,
        .get_password = fake_get_password,
        .get_string = fake_get_string,
        .uv_citm = fake_uv_citm,
    };
}
