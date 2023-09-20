#ifndef API_CLIENT_C
#define API_CLIENT_C

#include "libdstr/libdstr.h"
#include "libweb/libweb.h"
#include "libhttp/libhttp.h"

extern derr_type_t E_TOKEN;     // invalid token
extern derr_type_t E_PASSWORD;  // invalid password

typedef struct {
    unsigned int key;
    dstr_t secret;
    uint64_t nonce;
} api_token_t;

typedef void (*api_client_cb)(void *data, derr_t err, json_t *json);

typedef struct {
    duv_http_t *http;
    schedulable_t schedulable;
    dstr_t baseurl;
    // state
    json_t *json;
    api_client_cb cb;
    void *cb_data;
    derr_t e;
    http_pairs_t h1;
    http_pairs_t h2;
    dstr_t d1; // for h1
    dstr_t d2; // for h2
    bool using_password;
    duv_http_req_t req;
    stream_reader_t reader;
    dstr_t url;
    dstr_t reqbody;
    dstr_t respbody;
} api_client_t;

// zeroizes and frees secret
void api_token_free0(api_token_t *token);

// note that api_token should be zeroized or freed before calling this
derr_t api_token_read(const char *path, api_token_t *token);
derr_t api_token_read_path(const string_builder_t *sb, api_token_t *token);

derr_t api_token_write(api_token_t token, const char *path);
derr_t api_token_write_path(api_token_t token, const string_builder_t *sb);

/* helper to consistently solving tricky error handling:
   - read api token.  If it's unreadable, delete it and return ok=false.
     If it fails for other reasons, just raise that error.
   - increment nonce
   - write api token back to file.  If it fails, throw an error but leave the
     file alone.

   Note that api_token should be zeroized or freed before calling this. */
derr_t api_token_read_increment_write(
    const char *path, api_token_t *token, bool *ok
);
derr_t api_token_read_increment_write_path(
    const string_builder_t *sb, api_token_t *token, bool *ok
);

typedef struct {
    jspec_t jspec;
    jspec_t *content;
} jspec_api_t;

/* JAPI auto-dereferences the outer layer of json:
        {"status": "...", "content": ...} */
derr_t jspec_api_read(jspec_t *jspec, jctx_t *ctx);
#define JAPI(content) &((jspec_api_t){{jspec_api_read}, (content) }.jspec)

derr_t api_password_call(const char* host, unsigned int port, dstr_t* command,
                         dstr_t* arg, const dstr_t* username,
                         const dstr_t* password, int* code, dstr_t* reason,
                         dstr_t* recv, json_t *json);
/* throws: E_NOMEM (creating BIO, or adding to *reason)
           E_FIXEDSIZE (adding to *reason or to *recv)
           E_PARAM (username, password, arg, host, or command too long)
           E_INTERNAL
           E_CONN (failed or broken connection with host)
           E_SSL (server SSL certificate invalid)
           E_RESPONSE (bad response from server) */

/* before calling this file, the nonce of an existing API token should be
   incremented and written to a file, so that no matter what happens in this
   call the next api_token_call() gets a new nonce */
derr_t api_token_call(const char* host, unsigned int port, dstr_t* command,
                      dstr_t* arg, api_token_t* token, int* code,
                      dstr_t* reason, dstr_t* recv, json_t *json);
/* throws: E_NOMEM (creating BIO, or adding to *reason)
           E_FIXEDSIZE (adding to *reason or to *recv)
           E_PARAM (host, arg, or command too long)
           E_INTERNAL
           E_CONN (failed or broken connection with host)
           E_SSL (server SSL certificate invalid)
           E_RESPONSE (bad response from server) */

// copies baseurl
derr_t api_client_init(
    api_client_t *apic, duv_http_t *http, const dstr_t baseurl
);
// must have no request in flight
void api_client_free(api_client_t *apic);

// always succeeds; returns true if an err=E_CANCELED will be coming
bool api_client_cancel(api_client_t *apic);

// password-authenticated, event-based API call
void apic_pass(
    api_client_t *apic,
    dstr_t path,
    dstr_t arg,
    dstr_t user,
    dstr_t pass,
    json_t *json,
    api_client_cb cb,
    void *cb_data
);

// token-authenticated, event-based API call
void apic_token(
    api_client_t *apic,
    dstr_t path,
    dstr_t arg,
    api_token_t token,
    json_t *json,
    api_client_cb cb,
    void *cb_data
);

// password-authenticated, synchronous API call
derr_t api_pass_sync(
    http_sync_t *sync,
    const dstr_t baseurl,
    const dstr_t path,
    const dstr_t arg,
    const dstr_t user,
    const dstr_t pass,
    json_t *json
);

// token-authenticated, synchronous API call
derr_t api_token_sync(
    http_sync_t *sync,
    const dstr_t baseurl,
    const dstr_t path,
    const dstr_t arg,
    api_token_t token,
    json_t *json
);

derr_t register_api_token_sync(
    http_sync_t *sync,
    const dstr_t baseurl,
    const dstr_t user,
    const dstr_t pass,
    const char* creds_path
);

derr_t register_api_token_path_sync(
    http_sync_t *sync,
    const dstr_t baseurl,
    const dstr_t user,
    const dstr_t pass,
    const string_builder_t *sb
);

#endif // API_CLIENT_C
