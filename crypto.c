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

#include "crypto.h"
#include "libdstr/libdstr.h"
#include "ssl_errors.h"
#include "refs.h"


#define FORMAT_VERSON 1
#define B64_WIDTH 64
#define B64_CHUNK ((B64_WIDTH / 4) * 3)

// handle API uncompatibility between OpenSSL 1.1.0 API and older versions
#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define EVP_CIPHER_CTX_reset EVP_CIPHER_CTX_cleanup
#endif // old OpenSSL api

REGISTER_ERROR_TYPE(E_NOT4ME, "NOT4ME");

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
    derr_t e = E_OK;
    // make sure the PRNG is seeded
    int ret = RAND_status();
    if(ret != 1){
        trace_ssl_errors(&e);
        ORIG(&e, E_SSL, "not enough randomness to gen key");
    }

    BIGNUM* exp = BN_new();
    if(!exp){
        trace_ssl_errors(&e);
        ORIG(&e, E_NOMEM, "failed to allocate bignum");
    }

    // set the exponent argument to a safe value
    ret = BN_set_word(exp, RSA_F4);
    if(ret != 1){
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_SSL, "failed to set key exponent", cleanup_1);
    }

    // generate the key
    RSA* rsa = RSA_new();
    if(!rsa){
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_NOMEM, "failed to allocate rsa", cleanup_1);
    }
    ret = RSA_generate_key_ex(rsa, bits, exp, NULL);
    if(ret != 1){
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_SSL, "failed to generate key", cleanup_2);
    }

    // open the file for the private key
    FILE* f = compat_fopen(keyfile, "w");
    if(!f){
        TRACE(&e, "%x: %x\n", FS(keyfile), FE(&errno));
        ORIG_GO(&e, errno == ENOMEM ? E_NOMEM : E_OPEN, "failed to open file for writing", cleanup_2);
    }

    // write the private key to the file (no password protection)
    ret = PEM_write_RSAPrivateKey(f, rsa, NULL, NULL, 0, NULL, NULL);
    fclose(f);
    if(!ret){
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_SSL, "failed to write private key", cleanup_2);
    }

cleanup_2:
    RSA_free(rsa);
cleanup_1:
    BN_free(exp);
    return e;
}

// backing memory for a keypair_t
typedef struct {
    EVP_PKEY *pair;
    dstr_t fingerprint;
    refs_t refs;
} _keypair_t;
DEF_CONTAINER_OF(_keypair_t, fingerprint, dstr_t);
DEF_CONTAINER_OF(_keypair_t, refs, refs_t);

static void keypair_finalizer(refs_t *refs){
    _keypair_t *_kp = CONTAINER_OF(refs, _keypair_t, refs);
    dstr_free(&_kp->fingerprint);
    EVP_PKEY_free(_kp->pair);
    free(_kp);
}

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

    // now get ready to get the fingerprint of the key
    X509* x = X509_new();
    if(!x){
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_NOMEM, "X509_new failed", fail_fpr);
    }

    int ret = X509_set_pubkey(x, pkey);
    if(ret != 1){
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_SSL, "X509_set_pubkey failed", fail_x509);
    }

    // get the fingerprint
    unsigned int fpr_len;
    const EVP_MD* type = EVP_sha256();
    ret = X509_pubkey_digest(
        x, type, (unsigned char*)_kp->fingerprint.data, &fpr_len
    );
    if(ret != 1){
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_SSL, "X509_pubkey_digest failed", fail_x509);
    }
    _kp->fingerprint.len = fpr_len;

    X509_free(x);

    *out = kp;
    return e;

fail_x509:
    X509_free(x);
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

derr_t keypair_load(keypair_t **out, const char *keyfile){
    derr_t e = E_OK;

    *out = NULL;

    // try to allocate for the EVP_PKEY
    EVP_PKEY *pkey = EVP_PKEY_new();
    if(!pkey){
        trace_ssl_errors(&e);
        ORIG(&e, E_NOMEM, "EVP_PKEY_new failed");
    }

    FILE* f;
    EVP_PKEY* temp;

    // open the file for the private key
    f = compat_fopen(keyfile, "r");
    if(!f){
        TRACE(&e, "%x: %x\n", FS(keyfile), FE(&errno));
        derr_type_t err_type = errno == ENOMEM ? E_NOMEM : E_OPEN;
        ORIG_GO(&e, err_type, "failed to open file", fail_pkey);
    }

    // read the private key from the file (no password)
    temp = PEM_read_PrivateKey(f, &pkey, NULL, NULL);
    fclose(f);
    if(!temp){
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_SSL, "failed to read private key", fail_pkey);
    }

    PROP(&e, keypair_new(out, pkey) );

    return e;

fail_pkey:
    EVP_PKEY_free(pkey);
    return e;
}

derr_t keypair_from_pem(keypair_t **out, const dstr_t *pem){
    derr_t e = E_OK;

    *out = NULL;

    // try to allocate for the EVP_PKEY
    EVP_PKEY *pkey = EVP_PKEY_new();
    if(!pkey){
        trace_ssl_errors(&e);
        ORIG(&e, E_SSL, "EVP_PKEY_new failed");
    }

    // make sure pem isn't too long for OpenSSL
    if(pem->len > INT_MAX)
        ORIG(&e, E_VALUE, "pem is way too long");
    int pemlen = (int)pem->len;

    // wrap the pem-encoded key in an SSL memory BIO
    BIO* pembio = BIO_new_mem_buf((void*)pem->data, pemlen);
    if(!pembio){
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_SSL, "unable to create BIO", fail_pkey);
    }

    // read the public key from the BIO (no password protection)
    EVP_PKEY* temp;
    temp = PEM_read_bio_PUBKEY(pembio, &pkey, NULL, NULL);
    BIO_free(pembio);
    if(!temp){
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_SSL, "failed to read public key", fail_pkey);
    }

    PROP(&e, keypair_new(out, pkey) );

    return e;

fail_pkey:
    EVP_PKEY_free(pkey);
    return e;
}

derr_t keypair_copy(keypair_t *old, keypair_t **out){
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
    if(!old) return;
    keypair_t *kp = *old;
    // downref the backing memory
    _keypair_t *_kp = CONTAINER_OF(kp->fingerprint, _keypair_t, fingerprint);
    ref_dn(&_kp->refs);
    // free the reference memory
    free(kp);
    *old = NULL;
}

derr_t keypair_get_public_pem(keypair_t* kp, dstr_t* out){
    derr_t e = E_OK;
    // first create a memory BIO for writing the key to
    BIO* bio = BIO_new(BIO_s_mem());
    if(!bio){
        trace_ssl_errors(&e);
        ORIG(&e, E_NOMEM, "unable to create memory BIO");
    }

    // now write the public key to memory
    int ret = PEM_write_bio_PUBKEY(bio, kp->pair);
    if(!ret){
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_NOMEM, "failed to write public key", cleanup);
    }

    // now get a pointer to what was written
    char* ptr;
    long bio_len = BIO_get_mem_data(bio, &ptr);
    // I don't see any indication on how to check for errors, so here's a guess
    if(bio_len < 1){
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_INTERNAL, "failed to read public key from memory", cleanup);
    }

    // now wrap that pointer in a dstr_t for a dstr_copy operation
    dstr_t dptr;
    DSTR_WRAP(dptr, ptr, (size_t)bio_len, false);
    PROP_GO(&e, dstr_copy(&dptr, out), cleanup);

cleanup:
    BIO_free(bio);
    return e;
}

derr_t encrypter_new(encrypter_t* ec){
    derr_t e = E_OK;
    // allocate the context
    ec->ctx = EVP_CIPHER_CTX_new();
    if(!ec->ctx){
        trace_ssl_errors(&e);
        ORIG(&e, E_SSL, "EVP_CIPHER_CTX_new failed");
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
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_SSL, "EVP_SealInit failed", cu);
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
    PROP_GO(&e, bin2b64(&ec->pre64, out, B64_WIDTH, false), fail_1);

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
                                 FD(kp->fingerprint),
                                 FI(ek_len[i]),
                                 FD(&ek_wrapper)), fail_1);
        // dump line
        PROP_GO(&e, bin2b64(&ec->pre64, out, B64_WIDTH, false), fail_1);

        i++;
    }

    // append the IV
    // example ouput: "IV:16:<initialization vector>"
    dstr_t iv_wrapper;
    DSTR_WRAP(iv_wrapper, (char*)iv, (size_t)iv_len, false);
    // note we are also appending the M: to start the message
    PROP_GO(&e, FMT(&ec->pre64, "IV:%x:%x\nM:", FI(iv_len), FD(&iv_wrapper)), fail_1);
    PROP_GO(&e, bin2b64(&ec->pre64, out, B64_WIDTH, false), fail_1);

cu:
    dstr_free(&eks_block);
    return e;

fail_1:
    dstr_free(&eks_block);
    encrypter_reset(ec);
    return e;
}

derr_t encrypter_update(encrypter_t* ec, dstr_t* in, dstr_t* out){
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
            trace_ssl_errors(&e);
            ORIG_GO(&e, E_SSL, "EVP_SealUpdate failed", fail);
        }
        ec->pre64.len += (size_t)outlen;

        // flush pre64 to out
        PROP_GO(&e, bin2b64(&ec->pre64, out, B64_WIDTH, false), fail);
    }

    /* we always use all of dstr_t *in, but to be consistent with other api
       functions we will "consume" the buffer */
    in->len = 0;

    return e;

fail:
    encrypter_reset(ec);
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
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_SSL, "EVP_SealFinal failed", cleanup);
    }
    ec->pre64.len += (size_t)outlen;

    // flush pre64 to out, completely
    PROP_GO(&e, bin2b64(&ec->pre64, out, B64_WIDTH, true), cleanup);

    // get the GCM tag
    DSTR_VAR(tag, CIPHER_TAG_LEN);
    memset(tag.data, 0, tag.size);
    ret = EVP_CIPHER_CTX_ctrl(ec->ctx, EVP_CTRL_GCM_GET_TAG, CIPHER_TAG_LEN,
                              (unsigned char*)tag.data);
    if(ret != 1){
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_SSL, "failed to get GCM tag", cleanup);
    }
    tag.len = tag.size;

    // base64-encode the tag
    DSTR_VAR(b64tag, CIPHER_TAG_LEN * 2);
    // e = PFMT("%x\n", FD_DBG(&tag)); DROP_VAR(&e);
    PROP_GO(&e, bin2b64(&tag, &b64tag, 0, true), cleanup);

    // append the encoded tag on its own line, with '=' as a prefix
    PROP_GO(&e, FMT(out, "=%x\n", FD(&b64tag)), cleanup);

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
        trace_ssl_errors(&e);
        ORIG(&e, E_NOMEM, "EVP_CIPHER_CTX_new failed");
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
                        return e;;
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
                        return e;;
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
                    return e;;
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
                        return e;;
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
                int result;
                result = dstr_cmp(&hash, dc->kp->fingerprint);
                if(result == 0){
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
                        return e;;
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
                    return e;;
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
                NOFAIL(&e, E_ANY, dstr_copy(&iv, &dc->iv) );
                // remove this line from the buffer
                line_len = (uintptr_t)(leftover.data - dc->buffer.data) + iv_len + 1;
                dstr_leftshift(&dc->buffer, line_len);
                break;

            case 3: // M = message begins
                if(!dc->key_found){
                    // this is a speical error that ditm needs to catch
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
                    trace_ssl_errors(&e);
                    ORIG(&e, E_SSL, "EVP_OpenInit failed");
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
            e2 = b642bin(&sub, &dc->tag);
            // that should never error
            CATCH(e2, E_FIXEDSIZE){
                RETHROW_GO(&e, &e2, E_PARAM, fail);
            }else PROP(&e, e2);
            // that's all for the whole encryption message;
            read = in->len;
            break;
        }

        // read from *in to *base64
        NOFAIL_GO(&e, E_ANY, dstr_append(&dc->base64, &sub), fail);
        read += sub.len;

        // now push *base64 through the decoder
        NOFAIL_GO(&e, E_FIXEDSIZE, b642bin(&dc->base64, &dc->buffer), fail);

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
                trace_ssl_errors(&e);
                ORIG_GO(&e, E_SSL, "EVP_OpenUpdate failed", fail);
            }
            // make sure no buffer overrun happened
            if((size_t)outl > bytes_max){
                ORIG_GO(&e, E_INTERNAL, "more data decrypted than expected", fail);
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
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_SSL, "Failed to set GCM tag", cleanup);
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
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_SSL, "EVP_OpenFinal failed", cleanup);
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

derr_t hmac(const dstr_t* secret, const dstr_t* payload, dstr_t* hmac){
    derr_t e = E_OK;
    // set minimum length
    PROP(&e, dstr_grow(hmac, EVP_MAX_MD_SIZE) );

    if(secret->len > INT_MAX){
        ORIG(&e, E_PARAM, "secret too long");
    }
    int seclen = (int)secret->len;

    unsigned int hmaclen;
    const EVP_MD* type = EVP_sha512();
    unsigned char* ret = HMAC(type, (unsigned char*)secret->data, seclen,
                              (unsigned char*)payload->data, payload->len,
                              (unsigned char*)hmac->data, &hmaclen);
    if(!ret){
        trace_ssl_errors(&e);
        ORIG(&e, E_INTERNAL, "HMAC() failed");
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
        trace_ssl_errors(&e);
        ORIG(&e, E_SSL, "RAND_bytes() failed");
    }

    // extend the length of the dstr
    out->len = nbytes;
    return e;
}
