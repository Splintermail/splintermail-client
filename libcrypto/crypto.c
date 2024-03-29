#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

#include <openssl/ssl.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/bn.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/hmac.h>
#include <openssl/opensslv.h>

#include "libcrypto.h"


#define FORMAT_VERSON 1
#define B64_WIDTH 64
#define B64_CHUNK ((B64_WIDTH / 4) * 3)

// handle API uncompatibility between OpenSSL 1.1.0 API and older versions
#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define EVP_CIPHER_CTX_reset EVP_CIPHER_CTX_cleanup
#endif // old OpenSSL api

REGISTER_ERROR_TYPE(E_NOT4ME, "NOT4ME", "message encrypted but not to me");

DSTR_STATIC(pem_header, "-----BEGIN SPLINTERMAIL MESSAGE-----");
DSTR_STATIC(pem_footer, "-----END SPLINTERMAIL MESSAGE-----");

derr_t crypto_library_init(void){
    derr_t e = E_OK;
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    // SSL_library_init depricated in OpenSSL 1.1.0
    SSL_library_init();
    // load_error_strings depricated as well
    SSL_load_error_strings();
#endif
    // calling the new OPENSSL_init_crypto() explicitly not strictly necessary
    return e;
}

void crypto_library_close(void){
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    ERR_free_strings();
#endif
}

derr_t gen_key(int bits, const char* keyfile){
#if OPENSSL_VERSION_NUMBER < 0x30000000L // pre-3.0
    derr_t e = E_OK;
    // make sure the PRNG is seeded
    int ret = RAND_status();
    if(ret != 1){
        ORIG(&e, E_SSL, "not enough randomness to gen key: %x", FSSL);
    }

    BIGNUM* exp = BN_new();
    if(!exp){
        ORIG(&e, E_NOMEM, "failed to allocate bignum: %x", FSSL);
    }

    // set the exponent argument to a safe value
    ret = BN_set_word(exp, RSA_F4);
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "failed to set key exponent: %x", cleanup_1, FSSL);
    }

    // generate the key
    RSA* rsa = RSA_new();
    if(!rsa){
        ORIG_GO(&e, E_NOMEM, "failed to allocate rsa: %x", cleanup_1, FSSL);
    }
    ret = RSA_generate_key_ex(rsa, bits, exp, NULL);
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "failed to generate key: %x", cleanup_2, FSSL);
    }

    // open the file for the private key
    FILE *f;
    PROP_GO(&e, dfopen(keyfile, "w", &f), cleanup_2);

    // write the private key to the file (no password protection)
    ret = PEM_write_RSAPrivateKey(f, rsa, NULL, NULL, 0, NULL, NULL);
    if(!ret){
        ORIG_GO(&e, E_SSL, "failed to write private key: %x", cleanup_3, FSSL);
    }

    PROP_GO(&e, dfclose(f), cleanup_2);
    f = NULL;

cleanup_3:
    if(f) fclose(f);
cleanup_2:
    RSA_free(rsa);
cleanup_1:
    BN_free(exp);
    return e;
#else // 3.0 or greater
    derr_t e = E_OK;

    EVP_PKEY *pkey = NULL;
    FILE *f = NULL;
    BIO *bio = NULL;

    // make sure the PRNG is seeded
    int ret = RAND_status();
    if(ret != 1){
        ORIG(&e, E_SSL, "not enough randomness to gen key: %x", FSSL);
    }

    // generate the key
    pkey = EVP_RSA_gen(bits);
    if(!pkey){
        ORIG_GO(&e, E_SSL, "failed to generate key: %x", cu, FSSL);
    }

    // open the file ourselves, for better error typing
    PROP_GO(&e, dfopen(keyfile, "w", &f), cu);

    // wrap the f in a bio
    bio = BIO_new_fp(f, BIO_NOCLOSE);
    if(!bio){
        ORIG_GO(&e, E_SSL, "failed to create bio: %x", cu, FSSL);
    }

    // write the private key to the file (no password protection)
    ret = PEM_write_bio_PrivateKey(bio, pkey, NULL, NULL, 0, NULL, NULL);
    if(!ret){
        ORIG_GO(&e, E_SSL, "failed to write private key: %x", cu, FSSL);
    }

    ret = BIO_flush(bio);
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "failed to flush private key bio: %x", cu, FSSL);
    }

    // done with bio
    BIO_free(bio);
    bio = NULL;

    // done with f
    PROP_GO(&e, dfclose(f), cu);
    f = NULL;

cu:
    if(bio) BIO_free(bio);
    if(f) fclose(f);
    if(pkey) EVP_PKEY_free(pkey);
    return e;
#endif
}

derr_t gen_key_path(int bits, const string_builder_t *keypath){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(keypath, &stack, &heap, &path) );

    PROP_GO(&e, gen_key(bits, path->data), cu);

cu:
    dstr_free(&heap);
    return e;
}

// backing memory for a keypair_t
typedef struct {
    EVP_PKEY *pair;
    dstr_t fingerprint;
    refs_t refs;
} _keypair_t;
DEF_CONTAINER_OF(_keypair_t, fingerprint, dstr_t)
DEF_CONTAINER_OF(_keypair_t, refs, refs_t)

static void keypair_finalizer(refs_t *refs){
    _keypair_t *_kp = CONTAINER_OF(refs, _keypair_t, refs);
    dstr_free(&_kp->fingerprint);
    EVP_PKEY_free(_kp->pair);
    free(_kp);
}

derr_t get_fingerprint(EVP_PKEY* pkey, dstr_t *out){
    derr_t e = E_OK;

    X509* x = X509_new();
    if(!x){
        ORIG(&e, E_NOMEM, "X509_new failed: %x", FSSL);
    }

    int ret = X509_set_pubkey(x, pkey);
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "X509_set_pubkey failed: %x", fail_x509, FSSL);
    }

    // X509_pubkey_digest has a max output length and doesn't check at runtime
    DSTR_VAR(fpr, EVP_MAX_MD_SIZE);

    // get the fingerprint
    unsigned int fpr_len;
    const EVP_MD* type = EVP_sha256();
    ret = X509_pubkey_digest(
        x, type, (unsigned char*)fpr.data, &fpr_len
    );
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "X509_pubkey_digest failed: %x", fail_x509, FSSL);
    }
    fpr.len = fpr_len;

    X509_free(x);

    // copy to output
    PROP(&e, dstr_append(out, &fpr) );
    return e;

fail_x509:
    X509_free(x);
    return e;
}

static derr_t _read_pem_encoded_key(
    dstr_t pem,
    EVP_PKEY **out,
    EVP_PKEY *(*read_fn)(BIO*, EVP_PKEY**, pem_password_cb*, void*),
    const char *kind
){
    derr_t e = E_OK;
    *out = NULL;

    EVP_PKEY *pkey = EVP_PKEY_new();
    if(!pkey){
        ORIG(&e, E_NOMEM, "EVP_PKEY_new failed: %x", FSSL);
    }

    // make sure pem isn't too long for OpenSSL
    if(pem.len > INT_MAX) ORIG(&e, E_PARAM, "pem is way too long");
    int pemlen = (int)pem.len;

    // wrap the pem-encoded key in an SSL memory BIO
    BIO* pembio = BIO_new_mem_buf((void*)pem.data, pemlen);
    if(!pembio){
        ORIG_GO(&e, E_NOMEM, "unable to create BIO: %x", fail_pkey, FSSL);
    }

    // read the public key from the BIO (no password protection)
    EVP_PKEY* temp;
    temp = read_fn(pembio, &pkey, NULL, NULL);
    BIO_free(pembio);
    if(!temp){
        ORIG_GO(&e,
            E_PARAM, "failed to read %x key: %x", fail_pkey, FS(kind), FSSL
        );
    }

    *out = pkey;

    return e;

fail_pkey:
    EVP_PKEY_free(pkey);
    return e;
}

derr_t read_pem_encoded_pubkey(dstr_t pem, EVP_PKEY **out){
    derr_t e = E_OK;
    PROP(&e, _read_pem_encoded_key(pem, out, PEM_read_bio_PUBKEY, "public") );
    return e;
}

derr_t read_pem_encoded_privkey(dstr_t pem, EVP_PKEY **out){
    derr_t e = E_OK;
    PROP(&e,
        _read_pem_encoded_key(pem, out, PEM_read_bio_PrivateKey, "private")
    );
    return e;
}

// owns pkey and frees it on failure
static derr_t keypair_new(keypair_t **out, EVP_PKEY* pkey){
    derr_t e = E_OK;

    *out = NULL;

    // allocate backing memory
    _keypair_t *_kp = malloc(sizeof(*_kp));
    if(!_kp) ORIG_GO(&e, E_NOMEM, "nomem", fail);
    *_kp = (_keypair_t){ .pair = pkey };

    // start with 1 ref for the keypair_t we will return
    PROP_GO(&e, refs_init(&_kp->refs, 1, keypair_finalizer), fail_back_mem);

    // allocate reference memory
    keypair_t *kp = malloc(sizeof(*kp));
    if(!kp) ORIG_GO(&e, E_NOMEM, "nomem", fail_refs);
    *kp = (keypair_t){ .pair = _kp->pair, .fingerprint = &_kp->fingerprint };
    link_init(&kp->link);

    // initialize fingerprint
    PROP_GO(&e, dstr_new(&_kp->fingerprint, FL_FINGERPRINT), fail_ref_mem);

    PROP_GO(&e, get_fingerprint(pkey, &_kp->fingerprint), fail_fpr);

    *out = kp;
    return e;

fail_fpr:
    dstr_free(&_kp->fingerprint);
fail_ref_mem:
    free(kp);
fail_refs:
    refs_free(&_kp->refs);
fail_back_mem:
    free(_kp);
fail:
    EVP_PKEY_free(pkey);
    return e;
}

derr_t keypair_load_private(keypair_t **out, const char *keyfile){
    derr_t e = E_OK;

    *out = NULL;

    DSTR_VAR(pem, 8192);
    derr_t e2 = dstr_read_file(keyfile, &pem);
    CATCH(&e2, E_FIXEDSIZE){
        DROP_VAR(&e2);
        ORIG(&e, E_PARAM, "keyfile (%x) too long", FS(keyfile));
    }else PROP_VAR(&e, &e2);

    EVP_PKEY *pkey = NULL;
    PROP(&e, read_pem_encoded_privkey(pem, &pkey) );

    PROP(&e, keypair_new(out, pkey) );

    return e;
}

derr_t keypair_load_public(keypair_t **out, const char *keyfile){
    derr_t e = E_OK;

    *out = NULL;
    EVP_PKEY *pkey = NULL;
    EVP_PKEY *temp = NULL;

    DSTR_VAR(pem, 8192);
    derr_t e2 = dstr_read_file(keyfile, &pem);
    CATCH(&e2, E_FIXEDSIZE){
        DROP_VAR(&e2);
        ORIG_GO(&e, E_PARAM, "keyfile (%x) too long", cu, FS(keyfile));
    }else PROP_VAR_GO(&e, &e2, cu);

    e2 = read_pem_encoded_pubkey(pem, &pkey);
    if(e2.type != E_PARAM){
        // check for non-E_PARAM errors
        PROP_VAR_GO(&e, &e2, cu);
        // successfully read public key
        goto have_pubkey;
    }

    // E_PARAM error: failure may have been that it was a private key
    derr_t e3 = read_pem_encoded_privkey(pem, &pkey);
    if(is_error(e3)){
        // drop e3, just throw e2
        DROP_VAR(&e3);
        PROP_VAR_GO(&e, &e2, cu);
    }

    // successfully read a private key, get just the public key
    DROP_VAR(&e2);
    DROP_VAR(&e3);

    pem.len = 0;
    PROP_GO(&e, get_public_pem(pkey, &pem), cu);
    EVP_PKEY_free(pkey);
    pkey = NULL;
    PROP_GO(&e, read_pem_encoded_pubkey(pem, &pkey), cu);

have_pubkey:
    // keypair_new always owns pkey
    temp = pkey;
    pkey = NULL;
    PROP_GO(&e, keypair_new(out, temp), cu);

cu:
    if(pkey) EVP_PKEY_free(pkey);
    // erase any possible key material
    memset(pem.data, 0, pem.size);
    return e;
}

derr_t keypair_load_private_path(
    keypair_t **out, const string_builder_t *keypath
){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(keypath, &stack, &heap, &path) );

    PROP_GO(&e, keypair_load_private(out, path->data), cu);

cu:
    dstr_free(&heap);
    return e;
}

derr_t keypair_load_public_path(
    keypair_t **out, const string_builder_t *keypath
){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(keypath, &stack, &heap, &path) );

    PROP_GO(&e, keypair_load_public(out, path->data), cu);

cu:
    dstr_free(&heap);
    return e;
}

derr_t keypair_from_pubkey_pem(keypair_t **out, dstr_t pem){
    derr_t e = E_OK;

    *out = NULL;

    EVP_PKEY *pkey;
    PROP(&e, read_pem_encoded_pubkey(pem, &pkey) );

    PROP(&e, keypair_new(out, pkey) );

    return e;
}

derr_t keypair_from_private_pem(keypair_t **out, dstr_t pem){
    derr_t e = E_OK;

    *out = NULL;

    EVP_PKEY *pkey;
    PROP(&e, read_pem_encoded_privkey(pem, &pkey) );

    PROP(&e, keypair_new(out, pkey) );

    return e;
}

derr_t keypair_copy(const keypair_t *old, keypair_t **out){
    derr_t e = E_OK;

    *out = NULL;

    // dereference backing memory
    _keypair_t *_kp = CONTAINER_OF(old->fingerprint, _keypair_t, fingerprint);

    // allocate reference memory
    keypair_t *kp = malloc(sizeof(*kp));
    if(!kp) ORIG(&e, E_NOMEM, "nomem");
    *kp = (keypair_t){ .pair = _kp->pair, .fingerprint = &_kp->fingerprint };
    link_init(&kp->link);

    // upref the backing memory
    ref_up(&_kp->refs);

    *out = kp;
    return e;
}

void keypair_free(keypair_t **old){
    keypair_t *kp = *old;
    if(!kp) return;
    // downref the backing memory
    _keypair_t *_kp = CONTAINER_OF(kp->fingerprint, _keypair_t, fingerprint);
    ref_dn(&_kp->refs);
    // free the reference memory
    free(kp);
    *old = NULL;
}

derr_t get_private_pem(EVP_PKEY *pkey, dstr_t *out){
    derr_t e = E_OK;

    // first create a memory BIO for writing the key to
    BIO* bio = BIO_new(BIO_s_mem());
    if(!bio){
        ORIG(&e, E_NOMEM, "unable to create memory BIO: %x", FSSL);
    }

    // now write the private key to memory
    int ret = PEM_write_bio_PKCS8PrivateKey(
        bio, pkey, NULL, NULL, 0, NULL, NULL
    );
    if(!ret){
        ORIG_GO(&e, E_NOMEM, "failed to write private key: %x", cleanup, FSSL);
    }

    // now get a pointer to what was written
    char* ptr;
    long bio_len = BIO_get_mem_data(bio, &ptr);
    // I don't see any indication on how to check for errors, so here's a guess
    if(bio_len < 1){
        ORIG_GO(&e,
            E_INTERNAL,
            "failed to read private key from memory: %x",
            cleanup,
            FSSL
        );
    }

    // now wrap that pointer in a dstr_t for a dstr_copy operation
    dstr_t dptr;
    DSTR_WRAP(dptr, ptr, (size_t)bio_len, false);
    PROP_GO(&e, dstr_copy(&dptr, out), cleanup);

cleanup:
    BIO_free(bio);
    return e;
}

derr_t get_public_pem(EVP_PKEY *pkey, dstr_t *out){
    derr_t e = E_OK;

    // first create a memory BIO for writing the key to
    BIO* bio = BIO_new(BIO_s_mem());
    if(!bio){
        ORIG(&e, E_NOMEM, "unable to create memory BIO: %x", FSSL);
    }

    // now write the public key to memory
    int ret = PEM_write_bio_PUBKEY(bio, pkey);
    if(!ret){
        ORIG_GO(&e, E_NOMEM, "failed to write public key: %x", cleanup, FSSL);
    }

    // now get a pointer to what was written
    char* ptr;
    long bio_len = BIO_get_mem_data(bio, &ptr);
    // I don't see any indication on how to check for errors, so here's a guess
    if(bio_len < 1){
        ORIG_GO(&e,
            E_INTERNAL,
            "failed to read public key from memory: %x",
            cleanup,
            FSSL
        );
    }

    // now wrap that pointer in a dstr_t for a dstr_copy operation
    dstr_t dptr;
    DSTR_WRAP(dptr, ptr, (size_t)bio_len, false);
    PROP_GO(&e, dstr_copy(&dptr, out), cleanup);

cleanup:
    BIO_free(bio);
    return e;
}

derr_t keypair_get_public_pem(const keypair_t* kp, dstr_t* out){
    derr_t e = E_OK;
    PROP(&e, get_public_pem(kp->pair, out) );
    return e;
}

derr_t encrypter_new(encrypter_t* ec){
    derr_t e = E_OK;
    // allocate the context
    ec->ctx = EVP_CIPHER_CTX_new();
    if(!ec->ctx){
        ORIG(&e, E_SSL, "EVP_CIPHER_CTX_new failed: %x", FSSL);
    }

    DSTR_WRAP_ARRAY(ec->pre64, ec->pre64_buffer);

    link_init(&ec->keys);

    return e;
}

void encrypter_free(encrypter_t* ec){
    if(ec->ctx){
        EVP_CIPHER_CTX_free(ec->ctx);
    }
    ec->ctx = NULL;
    // free key references
    link_t *link;
    while((link = link_list_pop_first(&ec->keys))){
        keypair_t *kp = CONTAINER_OF(link, keypair_t, link);
        keypair_free(&kp);
    }
}

static void encrypter_reset(encrypter_t *ec){
    // reset (not free) the cipher context
    EVP_CIPHER_CTX_reset(ec->ctx);
    // free key references
    link_t *link;
    while((link = link_list_pop_first(&ec->keys))){
        keypair_t *kp = CONTAINER_OF(link, keypair_t, link);
        keypair_free(&kp);
    }
}

/* this will initialize the EVP_CIPHER_CTX, generate the random symmetrical
   key, encrypt that key to every public key given, and output the header of
   the message */
// derr_t encrypter_start(encrypter_t* ec, EVP_PKEY** pkeys, size_t npkeys,
//                        LIST(dstr_t)* fingerprints, dstr_t* out){
derr_t encrypter_start(encrypter_t* ec, link_t *keys, dstr_t* out){
    derr_t e = E_OK;

    // count and copy the keys
    ec->nkeys = 0;
    keypair_t *kp;
    LINK_FOR_EACH(kp, keys, keypair_t, link){
        if(ec->nkeys == MAX_ENCRYPTER_PUBKEYS){
            ORIG(&e, E_FIXEDSIZE, "too many pubkeys to encrypt to");
        }

        ec->pkeys[ec->nkeys++] = kp->pair;

        keypair_t *copy;
        PROP(&e, keypair_copy(kp, &copy) );
        link_list_append(&ec->keys, &copy->link);
    }

    // get ready to recieve all of the encrypted keys
    unsigned char* eks[MAX_ENCRYPTER_PUBKEYS];
    int ek_len[MAX_ENCRYPTER_PUBKEYS];

    // get max length of encrypted keys
    int max_ek_len = 0;
    for(size_t i = 0; i < ec->nkeys; i++){
        max_ek_len = MAX(max_ek_len, EVP_PKEY_size(ec->pkeys[i]));
    }

    // allocate a block of space for eks[i] to point into
    dstr_t eks_block;
    PROP_GO(&e, dstr_new(&eks_block, ec->nkeys * (size_t)max_ek_len), cu);

    // set eks pointers to point into eks_block
    for(size_t i = 0; i < ec->nkeys; i++){
        eks[i] = (unsigned char*)&(eks_block.data[i * (size_t)max_ek_len]);
    }

    // set type
    const EVP_CIPHER* type = CIPHER_TYPE;
    // store block size
    ec->block_size = (size_t)EVP_CIPHER_block_size(type);

    // get ready to recieve IV, should be more than big enough
    unsigned char iv[CIPHER_IV_LEN];
    // we are choosing not to use a VLA, so we do a check here
    int iv_len = EVP_CIPHER_iv_length(type);
    if((size_t)iv_len > sizeof(iv)){
        ORIG_GO(&e, E_FIXEDSIZE, "short iv buffer", cu);
    }

    // make sure npkeys isn't outrageous before the cast
    if(ec->nkeys > INT_MAX)
        ORIG_GO(&e, E_VALUE, "way too many pkeys", cu);
    int npkeys = (int)ec->nkeys;
    int ret = EVP_SealInit(ec->ctx, type, eks, ek_len, iv, ec->pkeys, npkeys);
    if(ret != npkeys){
        ORIG_GO(&e, E_SSL, "EVP_SealInit failed: %x", cu, FSSL);
    }

    // append PEM-like header to *out in plain text
    PROP_GO(&e, dstr_append(out, &pem_header), fail_1);
    DSTR_STATIC(line_break, "\n");
    PROP_GO(&e, dstr_append(out, &line_break), fail_1);

    // start with a clean pre64 buffer
    ec->pre64.len = 0;

    // append the version in base64
    // example output: "V:1\n" (version: 1)
    PROP_GO(&e, FMT(&ec->pre64, "V:%x\n", FI(FORMAT_VERSON)), fail_1);
    PROP_GO(&e, bin2b64_stream(&ec->pre64, out, B64_WIDTH, false), fail_1);

    // append each recipient and their encrypted key in base64
    // example output: "R:v-64:<sha256 hash>:256:<pubkey-encrypted msg key>
    size_t i = 0;
    LINK_FOR_EACH(kp, keys, keypair_t, link){
        // wrap the encrypted key in a dstr_t for ease of printing
        dstr_t ek_wrapper;
        DSTR_WRAP(ek_wrapper, (char*)eks[i], (size_t)ek_len[i], false);

        // format line
        PROP_GO(&e, FMT(&ec->pre64, "R:%x:%x:%x:%x\n",
                                 FU(kp->fingerprint->len),
                                 FD(*kp->fingerprint),
                                 FI(ek_len[i]),
                                 FD(ek_wrapper)), fail_1);
        // dump line
        PROP_GO(&e, bin2b64_stream(&ec->pre64, out, B64_WIDTH, false), fail_1);

        i++;
    }

    // append the IV
    // example ouput: "IV:16:<initialization vector>"
    dstr_t iv_wrapper;
    DSTR_WRAP(iv_wrapper, (char*)iv, (size_t)iv_len, false);
    // note we are also appending the M: to start the message
    PROP_GO(&e, FMT(&ec->pre64, "IV:%x:%x\nM:", FI(iv_len), FD(iv_wrapper)), fail_1);
    PROP_GO(&e, bin2b64_stream(&ec->pre64, out, B64_WIDTH, false), fail_1);

cu:
    dstr_free(&eks_block);
    return e;

fail_1:
    dstr_free(&eks_block);
    encrypter_reset(ec);
    return e;
}

derr_t encrypter_update(encrypter_t* ec, const dstr_t *in, dstr_t* out){
    derr_t e = E_OK;

    /* *pre64 should never have more that B64_CHUNK bytes leftover, because
       every B64_CHUNK bytes get flushed to *out, and EVP_SealUpdate will
       output at most (1 block - 1) bytes more than what we put in, so we can
       put in at most (pre64->size - B64_CHUNK - blocksize + 1) bytes.
       Technically pre64 could just grow but I'd like it to be allocated on the
       stack with fixed sized, since this will run on the server and I want to
       have more control over the memory size there */

    size_t chunk_size = ec->pre64.size - B64_CHUNK - ec->block_size + 1;

    for(size_t i = 0; i < in->len; i += chunk_size){
        // get a pointer into the next chunk of *in
        unsigned char* inptr = (unsigned char*)in->data + i;
        // this should be a safe cast since pre64 and block_size are fixed-size
        int inlen = (int)MIN(in->len - i, chunk_size);
        // get a pointer to the free data in *pre64
        unsigned char* outptr = (unsigned char*)ec->pre64.data + ec->pre64.len;
        int outlen;

        // encrypt this chunk
        int ret = EVP_SealUpdate(ec->ctx, outptr, &outlen, inptr, inlen);
        if(ret != 1){
            ORIG_GO(&e, E_SSL, "EVP_SealUpdate failed: %x", fail, FSSL);
        }
        ec->pre64.len += (size_t)outlen;

        // flush pre64 to out
        PROP_GO(&e, bin2b64_stream(&ec->pre64, out, B64_WIDTH, false), fail);
    }

    return e;

fail:
    encrypter_reset(ec);
    return e;
}


derr_t encrypter_update_stream(encrypter_t* ec, dstr_t* in, dstr_t* out){
    derr_t e = E_OK;
    PROP(&e, encrypter_update(ec, in, out) );

    // we always use all of dstr_t *in
    in->len = 0;

    return e;
}

derr_t encrypter_finish(encrypter_t* ec, dstr_t* out){
    derr_t e = E_OK;

    // at most (blocksize) bytes will be written by SealFinish

    // get a pointer to the free data in *pre64
    unsigned char* outptr = (unsigned char*)ec->pre64.data + ec->pre64.len;
    int outlen;

    // encrypt final chunk
    int ret = EVP_SealFinal(ec->ctx, outptr, &outlen);
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "EVP_SealFinal failed: %x", cleanup, FSSL);
    }
    ec->pre64.len += (size_t)outlen;

    // flush pre64 to out, completely
    PROP_GO(&e, bin2b64_stream(&ec->pre64, out, B64_WIDTH, true), cleanup);

    // get the GCM tag
    DSTR_VAR(tag, CIPHER_TAG_LEN);
    memset(tag.data, 0, tag.size);
    ret = EVP_CIPHER_CTX_ctrl(ec->ctx, EVP_CTRL_GCM_GET_TAG, CIPHER_TAG_LEN,
                              (unsigned char*)tag.data);
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "failed to get GCM tag: %x", cleanup, FSSL);
    }
    tag.len = tag.size;

    // base64-encode the tag
    DSTR_VAR(b64tag, CIPHER_TAG_LEN * 2);
    // e = PFMT("%x\n", FD_DBG(tag)); DROP_VAR(&e);
    PROP_GO(&e, bin2b64_stream(&tag, &b64tag, 0, true), cleanup);

    // append the encoded tag on its own line, with '=' as a prefix
    PROP_GO(&e, FMT(out, "=%x\n", FD(b64tag)), cleanup);

    // append the PEM-like footer
    PROP_GO(&e, dstr_append(out, &pem_footer), cleanup);
    DSTR_STATIC(line_break, "\n");
    PROP_GO(&e, dstr_append(out, &line_break), cleanup);

cleanup:
    encrypter_reset(ec);
    return e;
}

derr_t decrypter_new(decrypter_t* dc){
    derr_t e = E_OK;
    // we can't allocate this until we recieve the key pair
    memset(&dc->enc_key, 0, sizeof(dc->enc_key));

    DSTR_WRAP_ARRAY(dc->iv, dc->iv_buffer);
    // make sure our iv buffer is actually long enough
    const EVP_CIPHER* type = CIPHER_TYPE;
    if((size_t)EVP_CIPHER_iv_length(type) > dc->iv.size){
        ORIG(&e, E_INTERNAL, "iv buffer too short");
    }

    // for stuff that hasn't been decoded yet
    DSTR_WRAP_ARRAY(dc->base64, dc->base64_buffer);

    /* for stuff that has been decoded but not parsed yet.  This should be big
       enough that it can always hold *base64 decoded.  Of course, since
       *base64 will shrink we'll just make it the same size */
    DSTR_WRAP_ARRAY(dc->buffer, dc->buffer_buffer);

    // a place for the decoded tag
    DSTR_WRAP_ARRAY(dc->tag, dc->tag_buffer);

    // allocate the context
    dc->ctx = EVP_CIPHER_CTX_new();
    if(!dc->ctx){
        ORIG(&e, E_NOMEM, "EVP_CIPHER_CTX_new failed: %x", FSSL);
    }

    // set some initial state (makes error handling easier)
    dc->message_started = false;

    return e;
}

void decrypter_free(decrypter_t* dc){
    if(dc->ctx){
        EVP_CIPHER_CTX_free(dc->ctx);
    }
    dc->ctx = NULL;
    dstr_free(&dc->enc_key);
}

derr_t decrypter_start(decrypter_t* dc, const keypair_t* kp,
        LIST(dstr_t)* recips, dstr_t* recips_block){
    derr_t e = E_OK;
    // reset decrypter context in case a previous decryption was interrupted
    if(dc->message_started){
        EVP_CIPHER_CTX_reset(dc->ctx);
    }
    // record the pointers for the recipient list
    dc->recips = recips;
    dc->recips_block = recips_block;
    // set the state
    dc->header_found = false;
    dc->footer_found = false;
    dc->version_found = false;
    dc->key_found = false;
    dc->iv_found = false;
    dc->message_started = false;
    dc->tag_found = false;
    dc->tag.len = 0;
    // remember the key
    dc->kp = kp;
    // make sure our key buffer is actually long enough
    if(dc->enc_key.data == NULL){
        PROP(&e, dstr_new(&dc->enc_key, (size_t)EVP_PKEY_size(kp->pair)) );
    }else{
        PROP(&e, dstr_grow(&dc->enc_key, (size_t)EVP_PKEY_size(kp->pair)) );
    }
    // start with clean buffers
    dc->buffer.len = 0;
    dc->base64.len = 0;
    return e;
}

/* this function should parse all of the full lines of metadata in *buffer and
   return when either more data is needed or after the "M:" tag, meaning that
   everything remaining is part of the encrypted message. */
/* throws: E_PARAM (message was not parsable)
           E_SSL (message could not be decrypted)
           E_INTERNAL
           E_NOT4ME */
static derr_t decrypter_parse_metadata(decrypter_t* dc){
    derr_t e = E_OK;
    // define some patterns
    LIST_PRESET(dstr_t, colon, DSTR_LIT(":"));
    LIST_PRESET(dstr_t, line_end, DSTR_LIT("\n"));
    LIST_PRESET(dstr_t, tags, DSTR_LIT("V:"),
                              DSTR_LIT("R:"),
                              DSTR_LIT("IV:"),
                              DSTR_LIT("M:"));
    size_t longest_tag_len = 3;
    dstr_t sub;
    char* pos;
    // as long as there might be another tag, keep trying to parse
    while(dc->buffer.len >= longest_tag_len){
        // now try to parse a line
        size_t which_tag;
        pos = dstr_find(&dc->buffer, &tags, &which_tag, NULL);
        // must be a valid tag right at the beginning of the line
        if(!pos || pos > dc->buffer.data){
            ORIG(&e, E_PARAM, "failed to parse message");
        }
        // get a substring containing the rest of the line
        size_t start = (uintptr_t)(pos - dc->buffer.data) + tags.data[which_tag].len;
        dstr_t leftover = dstr_sub(&dc->buffer, start , 0);
        // the version tag must come first
        if(dc->version_found == false && which_tag != 0){
            ORIG(&e, E_PARAM, "failed to parse message");
        }
        // now do tag-sepcific parsing
        size_t line_len;
        switch(which_tag){
            case 0: // V = version
                // find end of line
                pos = dstr_find(&leftover, &line_end, NULL, NULL);
                // version number should be 3 characters or less
                if(!pos){
                    if(leftover.len <= 4){
                        // we might not have all of the line yet
                        return e;
                    }
                    ORIG(&e, E_PARAM, "failed to parse version");
                }
                // otherwise we should have a version
                sub = dstr_sub(&leftover, 0, (uintptr_t)(pos - leftover.data));
                unsigned int version;
                // this already returns E_PARAM on error:
                PROP(&e, dstr_tou(&sub, &version, 10) );
                if(version != 1){
                    ORIG(&e, E_PARAM, "unsupported message version");
                }
                dc->version_found = true;
                // remove this line from the buffer
                dstr_leftshift(&dc->buffer, (uintptr_t)(pos - dc->buffer.data) + 1);
                break;

            case 1: // R = recipient line
                // find the end of the hash length
                pos = dstr_find(&leftover, &colon, NULL, NULL);
                // hash length should be 4 charcaters or less
                if(!pos){
                    if(leftover.len <= 5){
                        // we might not have all of the line yet
                        return e;
                    }
                    ORIG(&e, E_PARAM, "failed to parse R line");
                }
                // get the hash length
                sub = dstr_sub(&leftover, 0, (uintptr_t)(pos - leftover.data));
                unsigned int hash_len;
                // this already returns E_PARAM on error:
                PROP(&e, dstr_tou(&sub, &hash_len, 10) );
                // update the leftover string
                leftover = dstr_sub(&leftover, (uintptr_t)(pos - leftover.data) + 1, 0);
                // make sure we have enough bytes left (including separator)
                if(leftover.len < hash_len + 1){
                    // we don't have the whole line
                    return e;
                }
                // otherwise read the hash of the key
                dstr_t hash;
                hash = dstr_sub(&leftover, 0, hash_len);
                // verify that after the hash we have a colon
                if(leftover.data[hash_len] != ':'){
                    ORIG(&e, E_PARAM, "failed to parse R line");
                }
                // update the leftover string
                leftover = dstr_sub(&leftover, hash_len + 1, 0);
                // find the end of the encrypted key length
                pos = dstr_find(&leftover, &colon, NULL, NULL);
                // encrypted key length should be 4 charcaters or less
                if(!pos){
                    if(leftover.len <= 5){
                        // we might not have all of the line yet
                        return e;
                    }
                    ORIG(&e, E_PARAM, "failed to parse R line");
                }
                // get the encrypted key length
                sub = dstr_sub(&leftover, 0, (uintptr_t)(pos - leftover.data));
                unsigned int key_len;
                // this already returns E_PARAM on error:
                PROP(&e, dstr_tou(&sub, &key_len, 10) );
                // update the leftover string
                leftover = dstr_sub(&leftover, (uintptr_t)(pos - leftover.data) + 1, 0);
                // make sure we have enough bytes left (including separator)
                if(leftover.len < key_len + 1){
                    // we don't have the whole line
                    return e;
                }
                // otherwise read the encrypted key
                dstr_t key;
                key = dstr_sub(&leftover, 0, key_len);
                // verify that after the key we have a new line
                if(leftover.data[key_len] != '\n'){
                    ORIG(&e, E_PARAM, "failed to parse R line");
                }
                // at last! we can check if this key was encrypted to us
                if(dstr_eq_consttime(&hash, dc->kp->fingerprint)){
                    dc->key_found = true;
                    PROP(&e, dstr_copy(&key, &dc->enc_key) );
                }
                // add to recipient list set up by decrypter_start
                if(dc->recips){
                    PROP(&e, list_append_with_mem(dc->recips, dc->recips_block,
                                               hash, false) );
                }
                // remove this line from the buffer
                line_len = (uintptr_t)(leftover.data - dc->buffer.data) + key_len + 1;
                dstr_leftshift(&dc->buffer, line_len);
                break;

            case 2: // IV = initialization vector
                // find the end of the iv length
                pos = dstr_find(&leftover, &colon, NULL, NULL);
                // iv length should be 4 charcaters or less
                if(!pos){
                    if(leftover.len <= 5){
                        // we might not have all of the line yet
                        return e;
                    }
                    ORIG(&e, E_PARAM, "failed to parse IV line");
                }
                // get the iv length
                sub = dstr_sub(&leftover, 0, (uintptr_t)(pos - leftover.data));
                unsigned int iv_len;
                // this already returns E_PARAM on error:
                PROP(&e, dstr_tou(&sub, &iv_len, 10) );
                // update the leftover string
                leftover = dstr_sub(&leftover, (uintptr_t)(pos - leftover.data) + 1, 0);
                // make sure we have enough bytes left (including separator)
                if(leftover.len < iv_len + 1){
                    // we don't have the whole line
                    return e;
                }
                // otherwise read the iv
                dstr_t iv;
                iv = dstr_sub(&leftover, 0, iv_len);
                // verify that after the iv we have a newline
                if(leftover.data[iv_len] != '\n'){
                    ORIG(&e, E_PARAM, "failed to parse IV line");
                }
                // make sure the iv is exactly as long as we need it to be
                if(iv_len != CIPHER_IV_LEN){
                    ORIG(&e, E_PARAM, "found invalid IV");
                }
                // store the iv
                dc->iv_found = true;
                dc->iv.len = 0;
                dstr_append_quiet(&dc->iv, &iv);
                // remove this line from the buffer
                line_len = (uintptr_t)(leftover.data - dc->buffer.data) + iv_len + 1;
                dstr_leftshift(&dc->buffer, line_len);
                break;

            case 3: // M = message begins
                if(!dc->key_found){
                    // this is a special error that citm needs to catch
                    ORIG(&e, E_NOT4ME, "our key not found");
                }
                if(!dc->iv_found){
                    ORIG(&e, E_PARAM, "no IV found");
                }
                // start the decryption
                const EVP_CIPHER* type = CIPHER_TYPE;
                unsigned char* bkey = (unsigned char*)dc->enc_key.data;
                unsigned char* biv = (unsigned char*)dc->iv.data;

                // this should be a very safe cast
                if(dc->enc_key.len > INT_MAX)
                    ORIG(&e, E_PARAM, "somehow encryption key is way too long");
                int ekeylen = (int)dc->enc_key.len;
                int ret = EVP_OpenInit(dc->ctx, type, bkey, ekeylen,
                                       biv, dc->kp->pair);
                if(ret != 1){
                    ORIG(&e, E_SSL, "EVP_OpenInit failed: %x", FSSL);
                }

                dc->message_started = true;
                // remove the tag "M:" from *buffer
                dstr_leftshift(&dc->buffer, 2);
                // exit the read-lines loop
                return e;
        }
    }

    return e;
}

derr_t decrypter_update(decrypter_t* dc, dstr_t* in, dstr_t* out){
    derr_t e = E_OK;
    derr_t e2;
    int result;
    size_t read = 0;

    // if we are waiting on the header
    if(dc->header_found == false){
        // don't do anything if  we can't compare yet
        if(in->len < pem_header.len){
            return e;
        }else{
            // otherwise do the comparison
            dstr_t sub = dstr_sub(in, 0, pem_header.len);
            result = dstr_cmp(&sub, &pem_header);
            if(result != 0){
                ORIG_GO(&e, E_PARAM, "PEM header not found", fail);
            }
            // if we are here we found the header
            dc->header_found = true;
            read += sub.len;
        }
    }

    while(read < in->len){
        // if we already found the tag, ignore everything left
        if(dc->tag_found){
            read = in->len;
            break;
        }

        // figure out how much we should read from *in
        size_t bytes_to_read;

        // first, find out how much we could read to fill *base64
        size_t free_space = dc->base64.size - dc->base64.len;
        if(free_space == 0){
            ORIG_GO(&e, E_PARAM, "bad decryption, line too long", fail);
        }

        // now find out how much text we have in the current line
        LIST_PRESET(dstr_t, line_end, DSTR_LIT("\n"));
        dstr_t leftover = dstr_sub(in, read, 0);
        char* pos = dstr_find(&leftover, &line_end, NULL, NULL);
        if(pos){
            bytes_to_read = MIN(free_space, (uintptr_t)pos + 1 -
                                            ((uintptr_t)in->data + read));
        }else{
            // if there's not a whole line, we need more input
            break;
        }

        // get a substring of the line we are going to read
        dstr_t sub = dstr_sub(in, read, MIN(read + bytes_to_read, in->len));

        // check if this line is the tag
        if(sub.data[0] == '='){
            dc->tag_found = true;
            // there should be nothing worth saving in *base64
            dc->base64.len = 0;
            // get a substring of the part of this line we want to decode
            sub = dstr_sub(&sub, 1, 0);
            // base64 the tag
            e2 = b642bin_stream(&sub, &dc->tag);
            // that should never error
            CATCH(&e2, E_FIXEDSIZE){
                RETHROW_GO(&e, &e2, E_PARAM, fail);
            }else PROP(&e, e2);
            // that's all for the whole encryption message;
            read = in->len;
            break;
        }

        // read from *in to *base64
        dstr_append_quiet(&dc->base64, &sub);
        read += sub.len;

        // now push *base64 through the decoder
        NOFAIL_GO(&e, E_FIXEDSIZE, b642bin_stream(&dc->base64, &dc->buffer), fail);

        // are we still parsing metadata?
        if(dc->message_started == false){
            PROP_GO(&e, decrypter_parse_metadata(dc), fail);
        }

        // are we ready to push encrypted message bytes into the decrypter?
        if(dc->message_started == true){
            // dump *buffer into the EVP_CIPHER
            if(dc->buffer.len == 0) break;
            unsigned char* bin = (unsigned char*)dc->buffer.data;
            size_t inl = dc->buffer.len;
            /* the output written will be as much as (inl + block_size - 1),
               (see man 3 EVP_EncryptUpdate for explanation) so we need to
               make sure we have enough space before we start */
            size_t bytes_max = inl + CIPHER_BLOCK_SIZE - 1;
            PROP_GO(&e, dstr_grow(out, out->len + bytes_max), fail);
            // do the decryption
            unsigned char* bout = (unsigned char*)out->data + out->len;
            int outl;
            // dc->buffer is fixed size, so this cast should be a safe cast
            int ret = EVP_OpenUpdate(dc->ctx, bout, &outl, bin, (int)inl);
            if(ret != 1){
                ORIG_GO(&e, E_SSL, "EVP_OpenUpdate failed: %x", fail, FSSL);
            }
            // make sure no buffer overrun happened
            if((size_t)outl > bytes_max){
                ORIG_GO(&e,
                    E_INTERNAL, "more data decrypted than expected",
                fail);
            }
            out->len += (size_t)outl;
            dc->buffer.len = 0;
        }
    }
    dstr_leftshift(in, read);
    return e;

fail:
    if(dc->message_started){
        EVP_CIPHER_CTX_reset(dc->ctx);
        dc->message_started = false;
    }
    return e;
}

derr_t decrypter_finish(decrypter_t* dc, dstr_t* out){
    derr_t e = E_OK;
    // make sure that we actually even started
    if(!dc->message_started){
        ORIG(&e, E_PARAM, "tried to finish decryption before the message began");
    }

    // under no circumstances should the GCM tag be longer than this
    if(dc->tag.len > INT_MAX){
        ORIG(&e, E_INTERNAL, "gcm tag is way too long");
    }
    int taglen = (int)dc->tag.len;
    // set the GCM authentication tag
    int ret = EVP_CIPHER_CTX_ctrl(dc->ctx, EVP_CTRL_GCM_SET_TAG,
                                  taglen, (unsigned char*)dc->tag.data);
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "Failed to set GCM tag: %x", cleanup, FSSL);
    }

    /* the output written as much as (block_size), so we need to
       make sure we have enough space before we start */
    size_t bytes_max = CIPHER_BLOCK_SIZE;
    PROP_GO(&e, dstr_grow(out, out->len + bytes_max), cleanup);
    // do the decryption
    unsigned char* bout = (unsigned char*)out->data + out->len;
    int outl;
    ret = EVP_OpenFinal(dc->ctx, bout, &outl);
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "EVP_OpenFinal failed: %x", cleanup, FSSL);
    }
    // make sure no buffer overrun happened
    if((size_t)outl > bytes_max){
        ORIG_GO(&e, E_INTERNAL, "more data decrypted than expected", cleanup);
    }
    out->len += (size_t)outl;

cleanup:
    if(dc->message_started){
        EVP_CIPHER_CTX_reset(dc->ctx);
    }
    return e;
}

derr_t hmac(const dstr_t secret, const dstr_t payload, dstr_t* hmac){
    derr_t e = E_OK;
    // set minimum length
    PROP(&e, dstr_grow(hmac, EVP_MAX_MD_SIZE) );

    if(secret.len > INT_MAX){
        ORIG(&e, E_PARAM, "secret too long");
    }
    int seclen = (int)secret.len;

    unsigned int hmaclen;
    const EVP_MD* type = EVP_sha512();
    unsigned char* ret = HMAC(type, (unsigned char*)secret.data, seclen,
                              (unsigned char*)payload.data, payload.len,
                              (unsigned char*)hmac->data, &hmaclen);
    if(!ret){
        ORIG(&e, E_INTERNAL, "HMAC() failed: %x", FSSL);
    }
    hmac->len = (size_t)hmaclen;

    return e;
}

derr_t random_bytes(dstr_t* out, size_t nbytes){
    derr_t e = E_OK;
    // set minimum length
    PROP(&e, dstr_grow(out, nbytes) );

    // It'd be nice if RAND_bytes accepted size_t argument
    if(nbytes > INT_MAX){
        ORIG(&e, E_VALUE, "too many bytes requested");
    }

    // get the random bytes
    int ret = RAND_bytes((unsigned char*)out->data, (int)nbytes);
    if(ret != 1){
        ORIG(&e, E_SSL, "RAND_bytes() failed: %x", FSSL);
    }

    // extend the length of the dstr
    out->len = nbytes;
    return e;
}

derr_t random_uint(uint32_t *out){
    derr_t e = E_OK;
    *out = 0;

    DSTR_VAR(buf, sizeof(*out));
    PROP(&e, random_bytes(&buf, buf.size) );

    memcpy(out, buf.data, sizeof(*out));

    return e;
}

derr_t random_uint_under(uint32_t end, uint32_t *out){
    derr_t e = E_OK;

    // integer division gives us the right answer:
    // divisor = 11 / 10 = 1
    // divisor = 19 / 10 = 1
    // divisor = 20 / 10 = 2
    // divisor = 29 / 10 = 2
    // for the last case:
    // for rand=29; 29 / 2 = 14, retry
    // for rand=21; 21 / 2 = 10, retry
    // for rand=20; 20 / 2 = 10, retry
    // for rand=19; 19 / 2 = 9, good
    // for rand=18; 18 / 2 = 9, good
    // for rand=17; 17 / 2 = 8, good
    uint32_t divisor = UINT32_MAX / end;

    for(size_t limit = 0; limit < 100000; limit++){
        DSTR_VAR(buf, sizeof(*out));
        PROP(&e, random_bytes(&buf, buf.size) );

        uint32_t temp;
        memcpy(&temp, buf.data, sizeof(temp));
        temp /= divisor;

        if(temp < end){
            *out = temp;
            return e;
        }
    }

    TRACE(&e, "failed to find random number less than %x\n", FU(end));
    ORIG(&e, E_INTERNAL, "10000 tries without success");
}

derr_t random_uint64(uint64_t *out){
    derr_t e = E_OK;
    *out = 0;

    DSTR_VAR(buf, sizeof(*out));
    PROP(&e, random_bytes(&buf, buf.size) );

    memcpy(out, buf.data, sizeof(*out));

    return e;
}

derr_t random_uint64_under(uint64_t end, uint64_t *out){
    derr_t e = E_OK;

    uint64_t divisor = UINT64_MAX / end;

    for(size_t limit = 0; limit < 100000; limit++){
        DSTR_VAR(buf, sizeof(*out));
        PROP(&e, random_bytes(&buf, buf.size) );

        uint64_t temp;
        memcpy(&temp, buf.data, sizeof(temp));
        temp /= divisor;

        if(temp < end){
            *out = temp;
            return e;
        }
    }

    TRACE(&e, "failed to find random number less than %x\n", FU(end));
    ORIG(&e, E_INTERNAL, "10000 tries without success");
}


derr_t keyshare_init(keyshare_t *keyshare){
    derr_t e = E_OK;

    link_init(&keyshare->keys);
    link_init(&keyshare->listeners);

    return e;
}

// all listeners must already be unregistered
void keyshare_free(keyshare_t *keyshare){
    link_t *link;
    while((link = link_list_pop_first(&keyshare->keys))){
        keypair_t *kp = CONTAINER_OF(link, keypair_t, link);
        keypair_free(&kp);
    }
}

derr_t keyshare_add_key(keyshare_t *keyshare, const keypair_t *kp){
    derr_t e = E_OK;

    link_t copies;
    link_init(&copies);
    link_t *link;

    // make a copy for ourselves
    keypair_t *ours;
    PROP(&e, keypair_copy(kp, &ours) );

    // make all the copies
    key_listener_i *key_listener;
    LINK_FOR_EACH(key_listener, &keyshare->listeners, key_listener_i, link){
        keypair_t *copy;
        PROP_GO(&e, keypair_copy(kp, &copy), fail);
        link_list_append(&copies, &copy->link);
    }

    // store our copy
    link_list_append(&keyshare->keys, &ours->link);

    // pass out all the copies
    LINK_FOR_EACH(key_listener, &keyshare->listeners, key_listener_i, link){
        link = link_list_pop_first(&copies);
        keypair_t *_kp = CONTAINER_OF(link, keypair_t, link);
        key_listener->add(key_listener, _kp);
    }

    return e;

fail:
    while((link = link_list_pop_first(&copies))){
        keypair_t *_kp = CONTAINER_OF(link, keypair_t, link);
        keypair_free(&_kp);
    }
    keypair_free(&ours);
    return e;
}

void keyshare_del_key(keyshare_t *keyshare, const dstr_t *fingerprint){
    // just spread the word
    key_listener_i *key_listener;
    LINK_FOR_EACH(key_listener, &keyshare->listeners, key_listener_i, link){
        key_listener->del(key_listener, fingerprint);
    }
}

derr_t keyshare_register(keyshare_t *keyshare, key_listener_i *key_listener,
        link_t *initial_keys){
    derr_t e = E_OK;

    link_t copies;
    link_init(&copies);
    link_t *link;

    // make all the copies
    keypair_t *kp;
    LINK_FOR_EACH(kp, &keyshare->keys, keypair_t, link){
        keypair_t *copy;
        PROP_GO(&e, keypair_copy(kp, &copy), fail);
        link_list_append(&copies, &copy->link);
    }

    // remember this key_listener
    link_list_append(&keyshare->listeners, &key_listener->link);

    // give the list of new copies to the new key_listener
    link_list_append_list(initial_keys, &copies);

    return e;

fail:
    while((link = link_list_pop_first(&copies))){
        keypair_t *kp = CONTAINER_OF(link, keypair_t, link);
        keypair_free(&kp);
    }
    return e;
}

void keyshare_unregister(keyshare_t *keyshare, key_listener_i *key_listener){
    (void)keyshare;
    // just forget the listener
    link_remove(&key_listener->link);
}
