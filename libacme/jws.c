#include "libacme/libacme.h"

#include <openssl/bn.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/ec.h>
#include <openssl/evp.h>

// ECDSA signature is 2x key size
#define ES256_SIG_SIZE 64

#if OPENSSL_VERSION_NUMBER >= 0x30000000L // openssl-3.0 or greater
static void to_bigendian(dstr_t *buf){
    // check endianness
    int i = 1;
    void *p = (void*)&i;
    char *c = p;

    if(!*c) return;
    // little endian; reverse the buffer

    size_t len = buf->len;
    char *data = buf->data;
    for(size_t i = 0; i < len/2; i++){
        char a = data[i];
        char b = data[len - 1 -i];
        data[len - 1 -i] = a;
        data[i] = b;
    }
}

static void from_bigendian(dstr_t *buf){
    // the operation is symmetric
    to_bigendian(buf);
}
#endif

DEF_CONTAINER_OF(_jdump_jwk_t, iface, jdump_i)

derr_type_t _jdump_jwkpvt(jdump_i *iface, writer_i *out, int indent, int pos){
    key_i *k = CONTAINER_OF(iface, _jdump_jwk_t, iface)->k;
    return k->to_jwk_pvt(k, out, indent, pos);
}

derr_type_t _jdump_jwkpub(jdump_i *iface, writer_i *out, int indent, int pos){
    key_i *k = CONTAINER_OF(iface, _jdump_jwk_t, iface)->k;
    return k->to_jwk_pub(k, out, indent, pos);
}

typedef struct {
    key_i iface;
    EVP_PKEY *pkey;
    EVP_MD_CTX *mdctx;
} ed25519_t;
DEF_CONTAINER_OF(ed25519_t, iface, key_i)

static derr_type_t ed25519_to_jwk(
    ed25519_t *k, bool pvt, writer_i *out, int indent, int pos
){

    DSTR_VAR(buf, 32);
    DSTR_VAR(pvtbuf, 32);
    buf.len = buf.size;
    int ret = EVP_PKEY_get_raw_public_key(
        k->pkey, (unsigned char*)buf.data, &buf.len
    );
    if(ret != 1) return E_SSL;

    if(pvt){
        pvtbuf.len = pvtbuf.size;
        ret = EVP_PKEY_get_raw_private_key(
            k->pkey, (unsigned char*)pvtbuf.data, &pvtbuf.len
        );
        if(ret != 1) return E_SSL;
    }

    jdump_i *obj = DOBJ(
        // public key elements in sorted order for thumbprint
        DKEY("crv", DS("Ed25519")),
        DKEY("kty", DS("OKP")),
        DKEY("x", DB64URL(buf)),
        // private key may be excluded
        DKEY("d", pvt ? DB64URL(buf) : NULL),
    );

    derr_type_t etype = obj->jdump(obj, out, indent, pos);

    // always zeroize private key buffer
    if(pvt) dstr_zeroize(&pvtbuf);

    return etype;
}

static derr_type_t ed25519_to_jwk_pvt(
    key_i *iface, writer_i *out, int indent, int pos
){
    ed25519_t *k = CONTAINER_OF(iface, ed25519_t, iface);
    return ed25519_to_jwk(k, true, out, indent, pos);
}

static derr_type_t ed25519_to_jwk_pub(
    key_i *iface, writer_i *out, int indent, int pos
){
    ed25519_t *k = CONTAINER_OF(iface, ed25519_t, iface);
    return ed25519_to_jwk(k, false, out, indent, pos);
}

static derr_t ed25519_to_pem_pub(key_i *iface, dstr_t *out){
    derr_t e = E_OK;
    ed25519_t *k = CONTAINER_OF(iface, ed25519_t, iface);
    PROP(&e, get_public_pem(k->pkey, out));
    return e;
}

static derr_t ed25519_sign(key_i *iface, const dstr_t in, dstr_t *out){
    derr_t e = E_OK;

    ed25519_t *k = CONTAINER_OF(iface, ed25519_t, iface);

    PROP(&e, dstr_grow(out, out->len + SHA512_DIGEST_LENGTH) );

    int ret = EVP_DigestSignInit(k->mdctx, NULL, NULL, NULL, k->pkey);
    if(ret != 1){
        ORIG(&e, E_SSL, "EVP_DigestSignInit failed: %x", FSSL);
    }

    size_t siglen = out->size - out->len;
    ret = EVP_DigestSign(k->mdctx,
        (unsigned char*)out->data + out->len, &siglen,
        (const unsigned char*)in.data, in.len
    );
    if(ret != 1){
        ORIG(&e, E_SSL, "EVP_DigestSign failed: %x", FSSL);
    }
    out->len += siglen;

    return e;
}

static void ed25519_free(key_i *iface){
    if(!iface) return;
    ed25519_t *k = CONTAINER_OF(iface, ed25519_t, iface);
    EVP_PKEY_free(k->pkey);
    EVP_MD_CTX_free(k->mdctx);
    free(k);
}

static void ed25519_prep(ed25519_t *k, EVP_PKEY **pkey, EVP_MD_CTX **mdctx){
    DSTR_STATIC(params, "\"alg\":\"EdDSA\",\"crv\":\"Ed25519\"");
    *k = (ed25519_t){
        .iface = {
            .protected_params = params,
            .to_jwk_pvt = ed25519_to_jwk_pvt,
            .to_jwk_pub = ed25519_to_jwk_pub,
            .to_pem_pub = ed25519_to_pem_pub,
            .sign = ed25519_sign,
            .free = ed25519_free,
        },
        .pkey = *pkey,
        .mdctx = *mdctx,
    };
    *pkey = NULL;
    *mdctx = NULL;
}

static derr_t ed25519_from_jwk(const dstr_t d, key_i **out){
    derr_t e = E_OK;

    *out = NULL;

    DSTR_VAR(buf, 32);
    PROP(&e, b64url2bin(d, &buf) );

    PROP(&e, ed25519_from_bytes(buf, out) );

    return e;
}

derr_t gen_ed25519(key_i **out){
    derr_t e = E_OK;
    *out = NULL;

    EVP_PKEY *pkey = NULL;
    EVP_PKEY_CTX *pctx = NULL;
    EVP_MD_CTX *mdctx = NULL;

    // first create a key
    pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
    if(!pctx){
        ORIG_GO(&e, E_SSL, "EVP_PKEY_CTX_new_id failed: %x", cu, FSSL);
    }

    int ret = EVP_PKEY_keygen_init(pctx);
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "EVP_PKEY_keygen_init failed: %x", cu, FSSL);
    }

    ret = EVP_PKEY_keygen(pctx, &pkey);
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "EVP_PKEY_keygen failed: %x", cu, FSSL);
    }

    mdctx = EVP_MD_CTX_new();
    if(!mdctx){
        ORIG_GO(&e, E_SSL, "EVP_MD_CTX_new failed: %x", cu, FSSL);
    }

    ed25519_t *k = DMALLOC_STRUCT_PTR(&e, k);
    CHECK_GO(&e, cu);

    ed25519_prep(k, &pkey, &mdctx);
    *out = &k->iface;

cu:
    if(pkey) EVP_PKEY_free(pkey);
    if(pctx) EVP_PKEY_CTX_free(pctx);
    if(mdctx) EVP_MD_CTX_free(mdctx);

    return e;
}

derr_t ed25519_from_bytes(const dstr_t bytes, key_i **out){
    derr_t e = E_OK;

    *out = NULL;

    EVP_PKEY *pkey = NULL;
    EVP_MD_CTX *mdctx = NULL;

    pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, NULL, (unsigned char*)bytes.data, bytes.len
    );
    if(!pkey){
        ORIG_GO(&e,
            E_SSL, "EVP_PKEY_new_raw_private_key failed: %x", cu, FSSL
        );
    }

    mdctx = EVP_MD_CTX_new();
    if(!mdctx){
        ORIG_GO(&e, E_SSL, "EVP_MD_CTX_new failed: %x", cu, FSSL);
    }

    ed25519_t *k = DMALLOC_STRUCT_PTR(&e, k);
    CHECK_GO(&e, cu);
    ed25519_prep(k, &pkey, &mdctx);

    *out = &k->iface;

cu:
    if(pkey) EVP_PKEY_free(pkey);
    if(mdctx) EVP_MD_CTX_free(mdctx);

    return e;
}

//

typedef struct {
    key_i iface;
    EVP_PKEY *pkey;
    EVP_MD_CTX *mdctx;
} es256_t;
DEF_CONTAINER_OF(es256_t, iface, key_i)

static derr_type_t es256_to_jwk(
    es256_t *k, bool pvt, writer_i *out, int indent, int pos
);

static derr_type_t es256_to_jwk_pvt(
    key_i *iface, writer_i *out, int indent, int pos
){
    es256_t *k = CONTAINER_OF(iface, es256_t, iface);
    return es256_to_jwk(k, true, out, indent, pos);
}

static derr_type_t es256_to_jwk_pub(
    key_i *iface, writer_i *out, int indent, int pos
){
    es256_t *k = CONTAINER_OF(iface, es256_t, iface);
    return es256_to_jwk(k, false, out, indent, pos);
}

static derr_t es256_to_pem_pub(key_i *iface, dstr_t *out){
    derr_t e = E_OK;
    es256_t *k = CONTAINER_OF(iface, es256_t, iface);
    PROP(&e, get_public_pem(k->pkey, out));
    return e;
}

static derr_t es256_sign(key_i *iface, const dstr_t in, dstr_t *out){
    derr_t e = E_OK;
    es256_t *k = CONTAINER_OF(iface, es256_t, iface);

    int ret = EVP_DigestSignInit(k->mdctx, NULL, EVP_sha256(), NULL, k->pkey);
    if(ret != 1){
        ORIG(&e, E_SSL, "EVP_DigestSignInit failed: %x", FSSL);
    }

    // use EVP_DigestSign to get a der-encoded ECDSA signature
    DSTR_VAR(der, 72);
    der.len = der.size;
    ret = EVP_DigestSign(k->mdctx,
        (unsigned char*)der.data, &der.len,
        (const unsigned char*)in.data, in.len
    );
    if(ret != 1){
        ORIG(&e, E_SSL, "EVP_DigestSign failed: %x", FSSL);
    }


    // extract r and s, and concatenate them into a jws ECDSA signature
    const unsigned char *p = (unsigned char*)der.data;
    ECDSA_SIG *sig = d2i_ECDSA_SIG(NULL, &p, (long)der.len);
    if(!sig){
        ORIG(&e, E_SSL, "d2i_ECDSA_SIG failed: %x", FSSL);
    }

    // errors must go through cu label

    DSTR_VAR(r, 32);
    DSTR_VAR(s, 32);
    ret = BN_bn2binpad(
        ECDSA_SIG_get0_r(sig), (unsigned char*)r.data, (int)r.size
    );
    if(ret != (int)r.size){
        ORIG_GO(&e, E_SSL, "BN_bn2binpad(r) failed: %x", cu, FSSL);
    }
    ret = BN_bn2binpad(
        ECDSA_SIG_get0_s(sig), (unsigned char*)s.data, (int)s.size
    );
    if(ret != (int)s.size){
        ORIG_GO(&e, E_SSL, "BN_bn2binpad(s) failed: %x", cu, FSSL);
    }
    r.len = r.size;
    s.len = s.size;

    PROP_GO(&e, dstr_append(out, &r), cu);
    PROP_GO(&e, dstr_append(out, &s), cu);

cu:
    ECDSA_SIG_free(sig);

    return e;
}

static void es256_free(key_i *iface){
    if(!iface) return;
    es256_t *k = CONTAINER_OF(iface, es256_t, iface);
    EVP_PKEY_free(k->pkey);
    EVP_MD_CTX_free(k->mdctx);
    free(k);
}

// the remaining es256 implementation varies wildly between openssl versions

#if OPENSSL_VERSION_NUMBER < 0x30000000L // pre-3.0

// openssl-1.1 variant
static derr_type_t es256_to_jwk(
    es256_t *k, bool pvt, writer_i *out, int indent, int pos
){
    derr_type_t etype = E_NONE;

    BIGNUM *x = NULL;
    BIGNUM *y = NULL;

    DSTR_VAR(xbuf, 256);
    DSTR_VAR(ybuf, 256);
    DSTR_VAR(dbuf, 256);

    // get the elliptic curve key from the EVP_PKEY wrapper
    const EC_KEY *eckey = EVP_PKEY_get0_EC_KEY(k->pkey);
    if(!eckey) LOG_FATAL("did not get EC_KEY from es256_t\n");

    // get the public key as an EC_POINT
    const EC_POINT *ecpoint =  EC_KEY_get0_public_key(eckey);

    // get the group, in order to get x and y values
    const EC_GROUP *group = EC_KEY_get0_group(eckey);

    // get x and y as well
    x = BN_new();
    if(!x){
        etype = E_SSL;
        goto cu;
    }
    y = BN_new();
    if(!y){
        etype = E_SSL;
        goto cu;
    }
    int ret = EC_POINT_get_affine_coordinates(group, ecpoint, x, y, NULL);
    if(ret != 1){
        etype = E_SSL;
        goto cu;
    }

    if(BN_num_bytes(x) > (int)xbuf.size){
        LOG_ERROR("xbuf too small\n");
        etype = E_INTERNAL;
        goto cu;
    }
    if(BN_num_bytes(y) > (int)ybuf.size){
        LOG_ERROR("ybuf too small\n");
        etype = E_INTERNAL;
        goto cu;
    }

    int xlen = BN_bn2bin(x, (unsigned char*)xbuf.data);
    if(xlen < 0){
        LOG_ERROR("BN_bn2bin returned negative length\n");
        etype = E_INTERNAL;
        goto cu;
    }
    xbuf.len = (size_t)xlen;

    int ylen = BN_bn2bin(y, (unsigned char*)ybuf.data);
    if(ylen < 0){
        LOG_ERROR("BN_bn2bin returned negative length\n");
        etype = E_INTERNAL;
        goto cu;
    }
    ybuf.len = (size_t)ylen;

    if(pvt){
        // get the private key as a bignum
        const BIGNUM *d = EC_KEY_get0_private_key(eckey);
        if(BN_num_bytes(d) > (int)dbuf.size){
            LOG_ERROR("dbuf too small\n");
            etype = E_INTERNAL;
            goto cu;
        }
        int dlen = BN_bn2bin(d, (unsigned char*)dbuf.data);
        if(dlen < 0){
            LOG_ERROR("BN_bn2bin returned negative length\n");
            etype = E_INTERNAL;
            goto cu;
        }
        dbuf.len = (size_t)dlen;
    }

    jdump_i *obj = DOBJ(
        // public key elements in sorted order for thumbprint
        DKEY("crv", DS("P-256")),
        DKEY("kty", DS("EC")),
        DKEY("x", DB64URL(xbuf)),
        DKEY("y", DB64URL(ybuf)),
        // private element may be skipped
        DKEY("d", pvt ? DB64URL(dbuf) : NULL),
    );
    etype = obj->jdump(obj, out, indent, pos);
    if(etype) goto cu;

cu:
    if(pvt) dstr_zeroize(&dbuf);
    if(x) BN_free(x);
    if(y) BN_free(y);

    return etype;
}

// openssl-1.1 variant
static derr_t es256_from_eckey(EC_KEY **eckey, key_i **out){
    derr_t e = E_OK;

    *out = NULL;

    EVP_PKEY *pkey = NULL;
    EVP_MD_CTX *mdctx = NULL;

    pkey = EVP_PKEY_new();
    if(!pkey){
        ORIG_GO(&e, E_SSL, "EVP_PKEY_new failed: %x", cu, FSSL);
    }

    int ret = EVP_PKEY_assign_EC_KEY(pkey, *eckey);
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "EVP_PKEY_assign_EC_KEY failed: %x", cu, FSSL);
    }
    // eckey now owned by pkey
    *eckey = NULL;

    mdctx = EVP_MD_CTX_new();
    if(!mdctx){
        ORIG_GO(&e, E_SSL, "EVP_MD_CTX_new failed: %x", cu, FSSL);
    }

    es256_t *k = DMALLOC_STRUCT_PTR(&e, k);
    CHECK_GO(&e, cu);

    DSTR_STATIC(params, "\"alg\":\"ES256\"");
    *k = (es256_t){
        .iface = {
            .protected_params = params,
            .to_jwk_pvt = es256_to_jwk_pvt,
            .to_jwk_pub = es256_to_jwk_pub,
            .to_pem_pub = es256_to_pem_pub,
            .sign = es256_sign,
            .free = es256_free,
        },
        .pkey = pkey,
        .mdctx = mdctx,
    };
    pkey = NULL;
    mdctx = NULL;

    *out = &k->iface;

cu:
    if(pkey) EVP_PKEY_free(pkey);
    if(mdctx) EVP_MD_CTX_free(mdctx);

    return e;
}

// openssl-1.1 variant
static derr_t es256_from_jwk(
    const dstr_t x, const dstr_t y, const dstr_t d, key_i **out
){
    derr_t e = E_OK;

    *out = NULL;

    BIGNUM *xbn = NULL;
    BIGNUM *ybn = NULL;
    BIGNUM *dbn = NULL;

    EC_KEY *eckey = NULL;

    DSTR_VAR(xbuf, 256);
    DSTR_VAR(ybuf, 256);
    DSTR_VAR(dbuf, 256);

    PROP_GO(&e, b64url2bin(x, &xbuf), cu);
    PROP_GO(&e, b64url2bin(y, &ybuf), cu);
    PROP_GO(&e, b64url2bin(d, &dbuf), cu);

    xbn = BN_bin2bn((const unsigned char*)xbuf.data, (int)xbuf.len, NULL);
    if(!xbn){
        ORIG_GO(&e, E_SSL, "BN_bin2bn failed: %x", cu, FSSL);
    }

    ybn = BN_bin2bn((const unsigned char*)ybuf.data, (int)ybuf.len, NULL);
    if(!ybn){
        ORIG_GO(&e, E_SSL, "BN_bin2bn failed: %x", cu, FSSL);
    }

    dbn = BN_bin2bn((const unsigned char*)dbuf.data, (int)dbuf.len, NULL);
    if(!dbn){
        ORIG_GO(&e, E_SSL, "BN_bin2bn failed: %x", cu, FSSL);
    }

    eckey = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if(!eckey){
        ORIG_GO(&e, E_SSL, "EC_KEY_new_by_curve_name failed: %x", cu, FSSL);
    }

    int ret = EC_KEY_set_public_key_affine_coordinates(eckey, xbn, ybn);
    if(ret != 1){
        ORIG_GO(&e,
            E_SSL, "EC_KEY_set_public_key_affine_coordinates failed: %x", FSSL
        cu);
    }

    ret = EC_KEY_set_private_key(eckey, dbn);
    if(ret != 1){
        ORIG_GO(&e,
            E_SSL, "EC_KEY_set_public_key_affine_coordinates failed: %x", FSSL
        cu);
    }

    PROP_GO(&e, es256_from_eckey(&eckey, out), cu);

cu:
    dstr_zeroize(&dbuf);
    if(xbn) BN_free(xbn);
    if(ybn) BN_free(ybn);
    if(dbn) BN_clear_free(dbn);
    if(eckey) EC_KEY_free(eckey);

    return e;
}

// openssl-1.1 variant
derr_t gen_es256(key_i **out){
    derr_t e = E_OK;

    EC_KEY *eckey = NULL;

    eckey = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if(!eckey){
        ORIG_GO(&e, E_SSL, "EC_KEY_new_by_curve_name failed: %x", cu, FSSL);
    }

    int ret = EC_KEY_generate_key(eckey);
    if(!ret){
        ORIG_GO(&e, E_SSL, "EC_KEY_generate_key failed: %x", cu, FSSL);
    }

    PROP_GO(&e, es256_from_eckey(&eckey, out), cu);

cu:
    if(eckey) EC_KEY_free(eckey);

    return e;
}

#else //

// openssl-3.0 variant
static derr_type_t es256_to_jwk(
    es256_t *k, bool pvt, writer_i *out, int indent, int pos
){
    derr_type_t etype = E_NONE;

    DSTR_VAR(d, 32);
    DSTR_VAR(x, 32);
    DSTR_VAR(y, 32);
    OSSL_PARAM params[] = {
        {
            .key="priv",
            .data_type=OSSL_PARAM_UNSIGNED_INTEGER,
            .data=d.data,
            .data_size=d.size,
        },
        {
            .key="qx",
            .data_type=OSSL_PARAM_UNSIGNED_INTEGER,
            .data=x.data,
            .data_size=x.size,
        },
        {
            .key="qy",
            .data_type=OSSL_PARAM_UNSIGNED_INTEGER,
            .data=y.data,
            .data_size=y.size,
        },
        {0},
    };
    int ret = EVP_PKEY_get_params(k->pkey, &params[pvt ? 0 : 1]);
    if(ret != 1) return E_SSL;
    if(pvt) d.len = params[0].return_size;
    x.len = params[1].return_size;
    y.len = params[2].return_size;
    if(pvt) to_bigendian(&d);
    to_bigendian(&x);
    to_bigendian(&y);

    jdump_i *obj = DOBJ(
        // public key elements in sorted order for thumbprint
        DKEY("crv", DS("P-256")),
        DKEY("kty", DS("EC")),
        DKEY("x", DB64URL(x)),
        DKEY("y", DB64URL(y)),
        // private element may be skipped
        DKEY("d", pvt ? DB64URL(d) : NULL),
    );
    etype = obj->jdump(obj, out, indent, pos);

    // always zeroize
    if(pvt) dstr_zeroize(&d);

    return etype;
}

// openssl-3.0 variant
static derr_t es256_from_pkey(EVP_PKEY **pkey, key_i **out){
    derr_t e = E_OK;

    *out = NULL;
    EVP_MD_CTX *mdctx = NULL;

    mdctx = EVP_MD_CTX_new();
    if(!mdctx){
        ORIG_GO(&e, E_SSL, "EVP_MD_CTX_new failed: %x", cu, FSSL);
    }

    es256_t *k = DMALLOC_STRUCT_PTR(&e, k);
    CHECK_GO(&e, cu);

    DSTR_STATIC(params, "\"alg\":\"ES256\"");
    *k = (es256_t){
        .iface = {
            .protected_params = params,
            .to_jwk_pvt = es256_to_jwk_pvt,
            .to_jwk_pub = es256_to_jwk_pub,
            .to_pem_pub = es256_to_pem_pub,
            .sign = es256_sign,
            .free = es256_free,
        },
        .pkey = *pkey,
        .mdctx = mdctx,
    };
    *pkey = NULL;
    mdctx = NULL;

    *out = &k->iface;

cu:
    if(mdctx) EVP_MD_CTX_free(mdctx);

    return e;
}

// openssl-3.0 variant
static derr_t es256_from_jwk(
    const dstr_t x, const dstr_t y, const dstr_t d, key_i **out
){
    derr_t e = E_OK;
    *out = NULL;

    EVP_PKEY *pkey = NULL;
    EVP_PKEY_CTX *ctx = NULL;

    /* pub format is the byte 'POINT_CONVERSION_UNCOMPRESSED' followed by
       x, then y, each in big-endian format */
    DSTR_VAR(pub, 65);
    pub.data[pub.len++] = POINT_CONVERSION_UNCOMPRESSED;
    PROP_GO(&e, b64url2bin(x, &pub), cu);
    PROP_GO(&e, b64url2bin(y, &pub), cu);

    DSTR_VAR(dbin, 32);
    PROP_GO(&e, b64url2bin(d, &dbin), cu);
    // jwk is big-endian, but OSSL_PARAM_UNSIGNED_INTEGER is host-endian
    from_bigendian(&dbin);

    OSSL_PARAM params[] = {
        {
            .key="group",
            .data_type=OSSL_PARAM_UTF8_STRING,
            .data="prime256v1",
            .data_size=10,
        },
        {
            .key="pub",
            .data_type=OSSL_PARAM_OCTET_STRING,
            .data=pub.data,
            .data_size=pub.len,
        },
        {
            .key="priv",
            .data_type=OSSL_PARAM_UNSIGNED_INTEGER,
            .data=dbin.data,
            .data_size=dbin.len,
        },
        {0},
    };

    ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    if(!ctx){
        ORIG_GO(&e, E_SSL, "EVP_PKEY_CTX_new_from_name failed: %x", cu, FSSL);
    }

    int ret = EVP_PKEY_fromdata_init(ctx);
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "EVP_PKEY_fromdata_init failed: %x", cu, FSSL);
    }

    ret = EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_KEYPAIR, params);
    if(ret != 1 || !pkey){
        ORIG_GO(&e, E_SSL, "EVP_PKEY_fromdata failed: %x", cu, FSSL);
    }

    PROP_GO(&e, es256_from_pkey(&pkey, out), cu);

cu:
    dstr_zeroize(&dbin);
    if(pkey) EVP_PKEY_free(pkey);
    if(ctx) EVP_PKEY_CTX_free(ctx);

    return e;
}

// openssl-3.0 variant
derr_t gen_es256(key_i **out){
    derr_t e = E_OK;

    EVP_PKEY *pkey = NULL;

    pkey = EVP_EC_gen("P-256");
    if(!pkey){
        ORIG_GO(&e, E_SSL, "EC_KEY_new_by_curve_name failed: %x", cu, FSSL);
    }

    PROP_GO(&e, es256_from_pkey(&pkey, out), cu);

cu:
    if(pkey) EVP_PKEY_free(pkey);

    return e;
}

#endif // openssl 3.0 vs 1.1

//

derr_t jwk_to_key(json_ptr_t jwk, key_i **out){
    derr_t e = E_OK;

    *out = NULL;

    dstr_t kty, crv, x, y, d;
    bool have_crv, have_x, have_y, have_d;
    jspec_t *jspec = JOBJ(true,
        JKEYOPT("crv", &have_crv, JDREF(&crv)),
        JKEYOPT("d", &have_d, JDREF(&d)),
        JKEY("kty", JDREF(&kty)),
        JKEYOPT("x", &have_x, JDREF(&x)),
        JKEYOPT("y", &have_y, JDREF(&y)),
    );
    PROP(&e, jspec_read(jspec, jwk) );
    if(!dstr_eq(kty, DSTR_LIT("OKP")) && !dstr_eq(kty, DSTR_LIT("EC"))){
        ORIG(&e,
            E_PARAM,
            "only Ed25519 and ES256 keys are supported; got \"kty\": \"%x\"",
            FD_DBG(kty)
        );
    }
    if(!have_crv) ORIG(&e, E_PARAM, "missing crv");

    if(dstr_eq(kty, DSTR_LIT("OKP"))){
        // Ed25519 key
        if(!dstr_eq(crv, DSTR_LIT("Ed25519"))){
            ORIG(&e,
                E_PARAM,
                "only Ed25519 EdDSA curve is supported; got \"crv\": \"%x\"",
                FD_DBG(crv)
            );
        }
        if(!have_x || !have_d){
            ORIG(&e, E_PARAM, "Ed25519 keys require x and d parameters");
        }
        PROP(&e, ed25519_from_jwk(d, out) );
    }else{
        // ES256 key
        if(!dstr_eq(crv, DSTR_LIT("P-256"))){
            ORIG(&e,
                E_PARAM,
                "only P-256 ECDSA curve is supported; got \"crv\": \"%x\"",
                FD_DBG(crv)
            );
        }
        if(!have_x || !have_y || !have_d){
            ORIG(&e, E_PARAM, "ES256 keys require x, y, and d parameters");
        }
        PROP(&e, es256_from_jwk(x, y, d, out) );
    }

    return e;
}

derr_t json_to_key(const dstr_t text, key_i **out){
    derr_t e = E_OK;

    *out = NULL;

    json_t json;
    JSON_PREP_PREALLOCATED(json, 256, 32, true);

    PROP_GO(&e, json_parse(text, &json), cu);
    PROP_GO(&e, jwk_to_key(json.root, out), cu);

cu:
    // erase key content
    dstr_zeroize(&json_textbuf);

    return e;
}

// not base64url-encoded
derr_t jwk_thumbprint(key_i *k, dstr_t *out){
    derr_t e = E_OK;

    PROP(&e, dstr_grow(out, out->len + SHA256_DIGEST_LENGTH) );

    DSTR_VAR(json, 256);
    PROP(&e, jdump(DJWKPUB(k), WD(&json), 0));

    unsigned char *uret = SHA256(
        (const unsigned char*)json.data,
        json.len,
        (unsigned char*)out->data + out->len
    );
    if(!uret){
        ORIG(&e, E_SSL, "SHA256 failed: %x", FSSL);
    }
    out->len += SHA256_DIGEST_LENGTH;

    return e;
}

// for dns01 challenges: b64url(sha256(token + "." + b64url(thumbprint(key))))
derr_t dns01_key_authz_string(const dstr_t token, key_i *k, dstr_t *out){
    derr_t e = E_OK;

    DSTR_VAR(thumb_bin, SHA256_DIGEST_LENGTH);
    DSTR_VAR(thumb_b64, 2*SHA256_DIGEST_LENGTH);
    DSTR_VAR(prehash, SHA256_DIGEST_LENGTH + 2*SHA256_DIGEST_LENGTH);
    DSTR_VAR(binhash, SHA256_DIGEST_LENGTH + 2*SHA256_DIGEST_LENGTH);

    PROP(&e, jwk_thumbprint(k, &thumb_bin) );
    PROP(&e, bin2b64url(thumb_bin, &thumb_b64) );
    PROP(&e, FMT(&prehash, "%x.%x", FD(token), FD(thumb_b64)) );

    unsigned char *uret = SHA256(
        (const unsigned char*)prehash.data,
        prehash.len,
        (unsigned char*)binhash.data
    );
    if(!uret){
        ORIG(&e, E_SSL, "SHA256 failed: %x", FSSL);
    }
    binhash.len = SHA256_DIGEST_LENGTH;

    PROP(&e, bin2b64url(binhash, out) );

    return e;
}

// signatures

derr_t _sign_key(void *data, const dstr_t in, dstr_t *out){
    key_i *k = data;
    return k->sign(k, in, out);
}

derr_t sign_hs256(const dstr_t hmac_key, const dstr_t in, dstr_t *out){
    derr_t e = E_OK;

    PROP(&e, dstr_grow(out, out->len + SHA256_DIGEST_LENGTH) );

    if(hmac_key.len > INT_MAX){
        ORIG(&e, E_FIXEDSIZE, "hmac key is way too long!\n");
    }
    unsigned int siglen = (unsigned int)(MIN(out->len - out->size, UINT_MAX));
    unsigned char *ucret = HMAC(
        EVP_sha256(),
        (const void*)hmac_key.data, (int)hmac_key.len,
        (const unsigned char*)in.data, in.len,
        (unsigned char*)out->data, &siglen
    );
    if(!ucret){
        ORIG(&e, E_SSL, "HMAC failed: %x", FSSL);
    }
    out->len += siglen;

    return e;
}

derr_t _sign_hs256(void *data, const dstr_t in, dstr_t *out){
    dstr_t *hmac_key = data;
    return sign_hs256(*hmac_key, in, out);
}

//

derr_t jws(
    const dstr_t protected,
    const dstr_t payload,
    derr_t (*sign)(void *sign_data, const dstr_t in, dstr_t *out),
    void *sign_data,
    dstr_t *out
){
    derr_t e = E_OK;

    PROP(&e, dstr_append(out, &DSTR_LIT("{\"protected\":\"")) );

    // prepare the signing input
    size_t protected_start = out->len;
    PROP(&e, bin2b64url(protected, out) );
    size_t protected_end = out->len;
    PROP(&e, dstr_append(out, &DSTR_LIT(".")) );
    PROP(&e, bin2b64url(payload, out) );
    dstr_t signing_input = dstr_sub2(*out, protected_start, SIZE_MAX);

    // compute the signature
    DSTR_VAR(signature, MAX(SHA512_DIGEST_LENGTH, ES256_SIG_SIZE));
    PROP(&e, sign(sign_data, signing_input, &signature) );

    // continue with the jws from the end of the b64url-encoded payload
    out->len = protected_end;
    PROP(&e,
        FMT(out,
                /*protected*/"\","
                "\"payload\":\"%x\","
                "\"signature\":\"%x\""
            "}",
            FB64URL(payload),
            FB64URL(signature),
        )
    );

    return e;
}

derr_t acme_jws(
    key_i *k,
    const dstr_t payload,
    const dstr_t nonce,
    const dstr_t url,
    const dstr_t kid,
    dstr_t *out
){
    derr_t e = E_OK;

    // write protected headers
    DSTR_VAR(protected, 4096);
    jdump_i *obj = DOBJ(
        DOBJSNIPPET(k->protected_params),
        DKEY("nonce", DD(nonce)),
        DKEY("kid", DD(kid)),
        DKEY("url", DD(url)),
    );
    PROP(&e, jdump(obj, WD(&protected), 0) );

    PROP(&e, jws(protected, payload, SIGN_KEY(k), out) );

    return e;
}

derr_t jspec_jwk_read(jspec_t *jspec, jctx_t *ctx){
    derr_t e = E_OK;

    jspec_jwk_t *j = CONTAINER_OF(jspec, jspec_jwk_t, jspec);

    json_ptr_t jptr = { .node = ctx->node };

    PROP(&e, jwk_to_key(jptr, j->k) );

    return e;
}
