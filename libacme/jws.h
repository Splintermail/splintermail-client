struct key_i;
typedef struct key_i key_i;

struct key_i {
    dstr_t protected_params;
    derr_type_t (*to_jwk_pvt)(key_i*, writer_i *out, int indent, int pos);
    // to_jwk_pub emits sorted keys for thumbprint purposes
    derr_type_t (*to_jwk_pub)(key_i*, writer_i *out, int indent, int pos);
    derr_t (*to_pem_pub)(key_i*, dstr_t *out);
    derr_t (*sign)(key_i*, const dstr_t in, dstr_t *out);
    void (*free)(key_i*);
};

typedef struct {
    jdump_i iface;
    key_i *k;
} _jdump_jwk_t;

derr_type_t _jdump_jwkpvt(jdump_i *iface, writer_i *out, int indent, int pos);
derr_type_t _jdump_jwkpub(jdump_i *iface, writer_i *out, int indent, int pos);

#define DJWKPVT(k) (&(_jdump_jwk_t){ { _jdump_jwkpvt }, k }.iface)
#define DJWKPUB(k) (&(_jdump_jwk_t){ { _jdump_jwkpub }, k }.iface)

// I'd prefer using Ed25519 over ES256, but letsencrypt doesn't support it
derr_t gen_ed25519(key_i **out);

derr_t ed25519_from_bytes(const dstr_t bytes, key_i **out);

derr_t gen_es256(key_i **out);

derr_t jwk_to_key(json_ptr_t jwk, key_i **out);

derr_t json_to_key(const dstr_t text, key_i **out);

// not base64url-encoded
derr_t jwk_thumbprint(key_i *k, dstr_t *out);

// for dns01 challenges: sha256(token + "." + b64url(thumbprint(key)))
derr_t dns01_key_authz_string(const dstr_t token, key_i *k, dstr_t *out);

// signatures

derr_t _sign_key(void *data, const dstr_t in, dstr_t *out);
#define SIGN_KEY(key) _sign_key, (void*)(key)

derr_t sign_hs256(const dstr_t hmac_key, const dstr_t in, dstr_t *out);
derr_t _sign_hs256(void *data, const dstr_t in, dstr_t *out);
#define SIGN_HS256(hmac_key) _sign_hs256, (void*)(hmac_key)

// b64url-encoding is applied to protected, payload, and output of sign()
derr_t jws(
    const dstr_t protected,
    const dstr_t payload,
    derr_t (*sign)(void *sign_data, const dstr_t in, dstr_t *out),
    void *sign_data,
    dstr_t *out
);

// sufficient for most acme endpoints
derr_t acme_jws(
    key_i *k,
    const dstr_t payload,
    const dstr_t nonce,
    const dstr_t url,
    const dstr_t kid,
    dstr_t *out
);

// for auto-parsing out of json
typedef struct {
    jspec_t jspec;
    key_i **k;
} jspec_jwk_t;
DEF_CONTAINER_OF(jspec_jwk_t, jspec, jspec_t)

derr_t jspec_jwk_read(jspec_t *jspec, jctx_t *ctx);

#define JJWK(_k) \
    &((jspec_jwk_t){ \
        .jspec = { .read = jspec_jwk_read }, .k = _k, \
    }.jspec)
