extern dstr_t ca_good_key;
extern dstr_t ca_good_cert;

extern dstr_t good_127_0_0_1_key;
extern dstr_t good_127_0_0_1_cert;
extern dstr_t good_127_0_0_1_chain;

extern dstr_t good_nobody_key;
extern dstr_t good_nobody_cert;
extern dstr_t good_nobody_chain;

extern dstr_t good_expired_key;
extern dstr_t good_expired_cert;
extern dstr_t good_expired_chain;

extern dstr_t ca_bad_cert;
extern dstr_t ca_bad_key;

extern dstr_t bad_127_0_0_1_key;
extern dstr_t bad_127_0_0_1_cert;
extern dstr_t bad_127_0_0_1_chain;

derr_t trust_ca(SSL_CTX *ctx, dstr_t ca_cert);
derr_t trust_good(SSL_CTX *ctx);
derr_t trust_bad(SSL_CTX *ctx);

derr_t good_127_0_0_1_server(SSL_CTX **out);
derr_t good_nobody_server(SSL_CTX **out);
derr_t good_expired_server(SSL_CTX **out);
derr_t bad_127_0_0_1_server(SSL_CTX **out);

// for pebble, the letsencrypt in-memory ACME implementation

extern dstr_t ca_pebble_cert;

derr_t trust_pebble(SSL_CTX *ctx);
