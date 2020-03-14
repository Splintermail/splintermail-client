#ifndef API_CLIENT_C
#define API_CLIENT_C

#include "libdstr/libdstr.h"

typedef struct{
    unsigned int key;
    char secret_buffer[256];
    dstr_t secret;
    unsigned long nonce;
} api_token_t;

/* it's called "init" not "new" because it allocates nothing, it just wraps
   fixed-size char arrays in dstr_t's */
void api_token_init(api_token_t* token);

derr_t api_token_read(const char* path, api_token_t* token);
/* throws: E_PARAM (bad file contents)
           E_FS  (file does not exist or we have no access)
           E_NOMEM (on fopen)
           E_OS (reading the opened file)
           E_INTERNAL */

derr_t api_token_write(const char* path, api_token_t* token);
/* throws: E_NOMEM (on fopen)
           E_FS
           E_OS (writing the opened file)
           E_INTERNAL */

derr_t register_api_token(const char* host,
                          unsigned int port,
                          const dstr_t* user,
                          const dstr_t* pass,
                          const char* creds_path);

derr_t api_password_call(const char* host, unsigned int port, dstr_t* command,
                         dstr_t* arg, const dstr_t* username,
                         const dstr_t* password, int* code, dstr_t* reason,
                         dstr_t* recv, LIST(json_t)* json);
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
                      dstr_t* reason, dstr_t* recv, LIST(json_t)* json);
/* throws: E_NOMEM (creating BIO, or adding to *reason)
           E_FIXEDSIZE (adding to *reason or to *recv)
           E_PARAM (host, arg, or command too long)
           E_INTERNAL
           E_CONN (failed or broken connection with host)
           E_SSL (server SSL certificate invalid)
           E_RESPONSE (bad response from server) */

#endif // API_CLIENT_C
