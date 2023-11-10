#include <openssl/bio.h>

#ifndef PREFERRED_CIPHERS
#define PREFERRED_CIPHERS "HIGH:MED:!aNULL:!kRSA:!PSK:!SRP:!MD5:!RC4"
#endif

// OpenSSL versions
typedef struct ssl_context_t {
    SSL_CTX* ctx;
} ssl_context_t;

derr_t ssl_context_new_client_ex(
    ssl_context_t* ctx, bool include_os, const char **cafiles, size_t ncafiles
);
// equivalent to ssl_context_new_client_ex(ctx, true, NULL, 0)
derr_t ssl_context_new_client(ssl_context_t* ctx);

// server side
derr_t ssl_context_new_server_pem(
    ssl_context_t* ctx, dstr_t fullchain, dstr_t key
);
derr_t ssl_context_new_server(
    ssl_context_t* ctx, const char* fullchainfile, const char* keyfile
);
derr_t ssl_context_new_server_path(
    ssl_context_t* ctx, string_builder_t fullchain, string_builder_t key
);

derr_t ssl_library_init(void);
void ssl_library_close(void);
void ssl_context_free(ssl_context_t* ctx);

// helpers, exposed for testing
derr_t ssl_ctx_read_cert_chain(SSL_CTX *ctx, dstr_t chain);
derr_t ssl_ctx_read_private_key(SSL_CTX *ctx, dstr_t key);
