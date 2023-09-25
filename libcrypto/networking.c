#include "libcrypto.h"


#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/x509v3.h>
#include <openssl/err.h>
#include <openssl/opensslv.h>

static derr_t set_safe_protocol(SSL_CTX *ctx){
    derr_t e = E_OK;

#if OPENSSL_VERSION_NUMBER < 0x10100000L
    // pre-1.1.0
    long opts = SSL_OP_NO_SSLv2
              | SSL_OP_NO_SSLv3
              | SSL_OP_NO_TLSv1
              | SSL_OP_NO_TLSv1_1;
    long lret = SSL_CTX_set_options(ctx->ctx, opts);
    if(!(lret & opts)){
        ORIG(&e, E_SSL, "failed to limit SSL protocols: %x", FSSL);
    }
#else
    // post-1.1.0
    long lret = SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if(lret != 1){
        ORIG(&e, E_SSL, "failed to limit SSL protocols: %x", FSSL);
    }
#endif

    return e;
}


// forward declaration of function only exposed to tests
derr_t ssl_context_load_from_os(ssl_context_t* ctx);

derr_t ssl_library_init(void){
    derr_t e = E_OK;
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    // SSL_library_init depricated in OpenSSL 1.1.0
    SSL_library_init();
    // load_error_strings depricated as well
    SSL_load_error_strings();
#else
    // calling the new OPENSSL_init_ssl() explicitly not strictly necessary
#endif
    return e;
}

void ssl_library_close(void){
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    ERR_free_strings();
#else
    OPENSSL_cleanup();
#endif
}


derr_t ssl_context_new_client_ex(
    ssl_context_t* ctx, bool include_os, const char **cafiles, size_t ncafiles
){
    derr_t e = E_OK;
    long lret;
    int ret;
    // pick method
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    // openssl pre-1.1.0 API
    // allow client to start talking to anybody
    const SSL_METHOD* meth = SSLv23_client_method();
    ctx->ctx = NULL;
    ctx->ctx = SSL_CTX_new(meth);
    if(!ctx->ctx){
        ORIG(&e, E_NOMEM, "failed to create SSL context: %x", FSSL);
    }
#else
    // openssl 1.1.0 API
    const SSL_METHOD* meth = TLS_client_method();
    ctx->ctx = NULL;
    ctx->ctx = SSL_CTX_new(meth);
    if(!ctx->ctx){
        ORIG(&e, E_NOMEM, "failed to create SSL context: %x", FSSL);
    }
#endif
    PROP_GO(&e, set_safe_protocol(ctx->ctx), cleanup);

    // load SSL certificate location first (may overwrite X509_STORE)
    if(include_os){
        PROP_GO(&e, ssl_context_load_from_os(ctx), cleanup);
    }

    X509_STORE *store = SSL_CTX_get_cert_store(ctx->ctx);
    for(size_t i = 0; i < ncafiles; i++){
        // put the cert in the store
        ret = X509_STORE_load_locations(store, cafiles[i], NULL);
        if(!ret){
            ORIG_GO(&e,
                E_SSL,
                "X509_STORE_load_file(%x) failed: %x",
                cleanup,
                FS(cafiles[i]),
                FSSL
            );
        }
    }

    /* no reason to accept weak ciphers with splintermail.com.  Note that the
       server is set to choose the cipher and the server is easier to keep
       up-to-date, but this is a good precaution. */
    ret = SSL_CTX_set_cipher_list(ctx->ctx, PREFERRED_CIPHERS);
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "could not set ciphers: %x", cleanup, FSSL);
    }

    // read/write operations should only return after handshake completed
    lret = SSL_CTX_set_mode(ctx->ctx, SSL_MODE_AUTO_RETRY);
    if(!(lret & SSL_MODE_AUTO_RETRY)){
        ORIG_GO(&e, E_SSL, "error setting SSL mode: %x", cleanup, FSSL);
    }

    return e;
cleanup:
    SSL_CTX_free(ctx->ctx);
    ctx->ctx = NULL;
    return e;
}

derr_t ssl_context_new_client(ssl_context_t* ctx){
    derr_t e = E_OK;
    PROP(&e, ssl_context_new_client_ex(ctx, true, NULL, 0) );
    return e;
}

// loosely based on openssl's ssl/ssl_rsa.c::use_certificate_chain_file()
static derr_t ssl_ctx_read_cert_chain(SSL_CTX *ctx, dstr_t chain){
    derr_t e = E_OK;

    BIO *bio = NULL;
    X509 *x = NULL;

    if(chain.len > INT_MAX) ORIG(&e, E_PARAM, "chain is way too long");
    int chainlen = (int)chain.len;

    bio = BIO_new_mem_buf((void*)chain.data, chainlen);
    if(!bio){
        ORIG_GO(&e, E_NOMEM, "unable to create BIO", cu);
    }

    x = X509_new();
    if(!x){
        ORIG_GO(&e, E_NOMEM, "nomem", cu);
    }

    // the first certificate may have trust information
    X509 *xret = PEM_read_bio_X509_AUX(bio, &x, NULL, NULL);
    if(!xret){
        ORIG_GO(&e, E_SSL, "unable to read cert: %x", cu, FSSL);
    }

    int ret = SSL_CTX_use_certificate(ctx, x);
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "unable to use cert: %x", cu, FSSL);
    }

    X509_free(x);
    x = NULL;

    long lret = SSL_CTX_clear_chain_certs(ctx);
    if(lret != 1){
        ORIG_GO(&e, E_SSL, "unable to clear chain certs: %x", cu, FSSL);
    }

    // read chain certs until the file runs out
    ERR_clear_error();
    while(true){
        // prepare memory
        x = X509_new();
        if(!x){
            ORIG_GO(&e, E_NOMEM, "nomem", cu);
        }
        // read a chain cert, no trust info
        X509 *xret = PEM_read_bio_X509(bio, &x, NULL, NULL);
        if(!xret){
            unsigned long err = ERR_peek_last_error();
            if(ERR_GET_LIB(err) == ERR_LIB_PEM
            && ERR_GET_REASON(err) == PEM_R_NO_START_LINE){
                // just an EOF error
                ERR_clear_error();
                break;
            }
            // non-EOF error
            ORIG_GO(&e, E_SSL, "unable to read chain cert: %x", cu, FSSL);
        }
        lret = SSL_CTX_add0_chain_cert(ctx, x);
        if(lret != 1){
            ORIG_GO(&e, E_SSL, "unable to add chain certs: %x", cu, FSSL);
        }
        // don't free successfully add0'd cert
        x = NULL;
    }

cu:
    X509_free(x);
    BIO_free(bio);

    return e;
}

static derr_t ssl_ctx_read_private_key(SSL_CTX *ctx, dstr_t key){
    derr_t e = E_OK;

    BIO *bio = NULL;
    EVP_PKEY *pkey = NULL;

    if(key.len > INT_MAX) ORIG(&e, E_PARAM, "key is way too long");
    int keylen = (int)key.len;

    bio = BIO_new_mem_buf((void*)key.data, keylen);
    if(!bio){
        ORIG_GO(&e, E_NOMEM, "unable to create BIO", cu);
    }

    pkey = EVP_PKEY_new();
    if(!pkey){
        ORIG_GO(&e, E_NOMEM, "nomem", cu);
    }

    EVP_PKEY *pkret = PEM_read_bio_PrivateKey(bio, &pkey, NULL, NULL);
    if(!pkret){
        ORIG_GO(&e, E_SSL, "unable to read private key: %x\n", cu, FSSL);
    }

    int ret = SSL_CTX_use_PrivateKey(ctx, pkey);
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "unable to use private key: %x\n", cu, FSSL);
    }

cu:
    EVP_PKEY_free(pkey);
    BIO_free(bio);

    return e;
}

derr_t ssl_context_new_server_pem(
    ssl_context_t* ctx, dstr_t fullchain, dstr_t key
){
    derr_t e = E_OK;
    long lret;

    *ctx = (ssl_context_t){0};
    SSL_CTX *out = NULL;

    // pick method
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    // openssl pre-1.1.0 API
    // allow server to start talking to anybody
    const SSL_METHOD* meth = SSLv23_server_method();
    out = SSL_CTX_new(meth);
    if(!out){
        ORIG(&e, E_NOMEM, "failed to create SSL context: %x", FSSL);
    }
    long ulret;
#else
    // openssl 1.1.0 API
    // allow server to start talking to anybody
    const SSL_METHOD* meth = TLS_server_method();
    out = SSL_CTX_new(meth);
    if(!out){
        ORIG(&e, E_NOMEM, "failed to create SSL context: %x", FSSL);
    }
    uint64_t ulret;
#endif
    PROP_GO(&e, set_safe_protocol(out), cu);

    // set key and cert
    PROP_GO(&e, ssl_ctx_read_cert_chain(out, fullchain), cu);
    PROP_GO(&e, ssl_ctx_read_private_key(out, key), cu);

    // make sure the key matches the certificate
    int ret = SSL_CTX_check_private_key(out);
    if(ret != 1){
        ORIG_GO(&e,
            E_SSL, "private key does not match certificate: %x", cu, FSSL
        );
    }

    // make sure server sets cipher preference
    ulret = SSL_CTX_set_options(out, SSL_OP_CIPHER_SERVER_PREFERENCE);
    if( !(ulret & SSL_OP_CIPHER_SERVER_PREFERENCE) ){
        ORIG_GO(&e,
            E_SSL, "failed to set server cipher preference: %x", cu, FSSL
        );
    }

    ret = SSL_CTX_set_cipher_list(out, PREFERRED_CIPHERS);
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "could not set ciphers: %x", cu, FSSL);
    }

    // read/write operations should only return after handshake completed
    lret = SSL_CTX_set_mode(out, SSL_MODE_AUTO_RETRY);
    if(!(lret & SSL_MODE_AUTO_RETRY)){
        ORIG_GO(&e, E_SSL, "error setting SSL mode: %x", cu, FSSL);
    }

    ctx->ctx = out;
    out = NULL;

cu:
    SSL_CTX_free(out);
    return e;
}

derr_t ssl_context_new_server(
    ssl_context_t* ctx, const char* fullchainfile, const char* keyfile
){
    derr_t e = E_OK;
    *ctx = (ssl_context_t){0};

    dstr_t chain = {0};
    dstr_t key = {0};

    // read the chainfile and keyfile
    PROP_GO(&e, dstr_read_file(fullchainfile, &chain), cu);
    PROP_GO(&e, dstr_read_file(keyfile, &key), cu);

    PROP_GO(&e, ssl_context_new_server_pem(ctx, chain, key), cu);

cu:
    dstr_free0(&chain);
    dstr_free0(&key);
    return e;
}

derr_t ssl_context_new_server_path(
    ssl_context_t* ctx, string_builder_t fullchain, string_builder_t key
){
    DSTR_VAR(stack_cert, 256);
    dstr_t heap_cert = {0};
    dstr_t* dchain = NULL;
    DSTR_VAR(stack_key, 256);
    dstr_t heap_key = {0};
    dstr_t* dkey = NULL;

    derr_t e = E_OK;

    *ctx = (ssl_context_t){0};

    PROP_GO(&e, sb_expand(&fullchain, &stack_cert, &heap_cert, &dchain), cu);
    PROP_GO(&e, sb_expand(&key, &stack_key, &heap_key, &dkey), cu);
    PROP_GO(&e, ssl_context_new_server(ctx, dchain->data, dkey->data), cu);

cu:
    dstr_free(&heap_key);
    dstr_free(&heap_cert);
    return e;
}

void ssl_context_free(ssl_context_t* ctx){
    if(ctx->ctx){
        SSL_CTX_free(ctx->ctx);
    }
    ctx->ctx = NULL;
}

#ifdef _WIN32
#include <wincrypt.h>
// code for loading OS CA certificates in windows
derr_t ssl_context_load_from_os(ssl_context_t* ctx){
    derr_t e = E_OK;
    // https://msdn.microsoft.com/en-us/library/windows/desktop/aa376560.aspx
    HCERTSTORE hStore = CertOpenSystemStoreA((HCRYPTPROV_LEGACY)NULL, "ROOT");
    if(hStore == NULL){
        win_perror();
        ORIG(&e, E_OS, "failed to open system certificate store");
    }

    PCCERT_CONTEXT wincert = NULL;

    // start building a certificate store
    X509_STORE* store = X509_STORE_new();
    if(!store){
        ORIG_GO(&e, E_NOMEM, "X509_STORE_new failed", cu_winstore);
    }

    // https://msdn.microsoft.com/en-us/library/windows/desktop/aa376560.aspx
    while((wincert = CertEnumCertificatesInStore(hStore, wincert)) != NULL){
        // create OpenSSL key from der-encoding
        // (we need to use a separate pointer because d2i_X509 changes it)
        const unsigned char* ptr = wincert->pbCertEncoded;
        long len = wincert->cbCertEncoded;
        X509* x = d2i_X509(NULL, &ptr, len);
        if(x == NULL){
            ORIG_GO(&e,
                E_SSL,
                "failed to convert system cert to SSL cert: %x",
                cu_store,
                FSSL
            );
        }

        // add cert to the store
        int ret = X509_STORE_add_cert(store, x);
        if(ret != 1){
            ORIG_GO(&e, E_SSL, "Unable to add certificate to store", cu_x);
        }

    cu_x:
        X509_free(x);
        if(is_error(e)) goto cu_store;
    }

    // now add store to the context
    //SSL_CTX_set0_verify_cert_store(ctx->ctx, store);
    SSL_CTX_set_cert_store(ctx->ctx, store);
    // no errors possible with this function
    // but don't cleanup cu_store, it will be cleaned up automatically later
    goto cu_winstore;

cu_store:
    X509_STORE_free(store);
cu_winstore:
    CertCloseStore(hStore, CERT_CLOSE_STORE_FORCE_FLAG);
    return e;
}

#else // not _WIN32

#ifdef __APPLE__
// code for loading OS CA certificates in osx
#include <Security/SecItem.h>
#include <Security/SecImportExport.h>
#include <Security/SecCertificate.h>
#include <Security/SecPolicy.h>
#include <Security/SecTrust.h>

#define MAX_DEPTH 100

// macos error string fmt_i's

static derr_type_t put_cfstring(writer_i *out, CFStringRef str, bool *ok){
    // try to get a pointer (if CFStringRef is contiguous)
    const char *s = CFStringGetCStringPtr(str, kCFStringEncodingUTF8);
    if(s){
        *ok = true;
        return out->w->puts(out, s, strlen(s));
    }

    // otherwise grab the first 1k bytes
    char buf[1024];
    CFRange range = { .location = 0, .length = sizeof(buf) };
    CFIndex used = 0;
    CFIndex len = CFStringGetBytes(
        str,
        range,
        kCFStringEncodingUTF8,
        '?', // UInt8 lossByte, for undecodable characters
        false, // isExternalRepresentation; if true, would include unicode BOM
        (unsigned char*)buf,
        sizeof(buf),
        &used // usedBufLen
    );

    if(len < 1){
        *ok = false;
        return E_NONE;
    }

    *ok = true;
    return out->w->puts(out, buf, (size_t)len);
}

typedef struct {
    fmt_i iface;
    OSStatus osstatus;
} _fmt_osstatus_t;

DEF_CONTAINER_OF(_fmt_osstatus_t, iface, fmt_i)

static derr_type_t _fmt_osstatus(const fmt_i *iface, writer_i *out){
    OSStatus osstatus = CONTAINER_OF(iface, _fmt_osstatus_t, iface)->osstatus;
    CFStringRef str = SecCopyErrorMessageString(osstatus, NULL);
    if(!str){
        static char msg[] = "<failed to read macos OSStatus>";
        return out->w->puts(out, msg, sizeof(msg)-1);
    }
    bool ok;
    derr_type_t etype = put_cfstring(out, str, &ok);
    CFRelease(str);
    if(!ok){
        static char msg[] = "<failed to read macos OSStatus>";
        return out->w->puts(out, msg, sizeof(msg)-1);
    }
    return etype;
}

#define FMACOS(osstatus) (&(_fmt_osstatus_t){ {_fmt_osstatus}, osstatus }.iface)

typedef struct {
    fmt_i iface;
    CFErrorRef cferror;
} _fmt_cferror_t;

DEF_CONTAINER_OF(_fmt_cferror_t, iface, fmt_i)

static derr_type_t _fmt_cferror(const fmt_i *iface, writer_i *out){
    CFErrorRef cferror = CONTAINER_OF(iface, _fmt_cferror_t, iface)->cferror;

    CFStringRef str = CFErrorCopyDescription(cferror);
    if(!str){
        static char msg[] = "<failed to read CFErrorRef>";
        return out->w->puts(out, msg, sizeof(msg)-1);
    }
    bool ok;
    derr_type_t etype = put_cfstring(out, str, &ok);
    CFRelease(str);
    if(!ok){
        static char msg[] = "<failed to read CFErrorRef>";
        return out->w->puts(out, msg, sizeof(msg)-1);
    }
    return etype;
}

#define FCFERROR(cferror) (&(_fmt_cferror_t){ {_fmt_cferror}, cferror }.iface)

/* macos strategy:  I don't see a non-deprecated way to get a list of all
   system certs (the old strategy) but I do see ways to evaluate the trust
   of a single certificate chain, so we'll just do exactly that in a
   verify_callback.

   We still rely on OpenSSL for the backbone of the verification.  This makes
   it easy for test code with custom verify_name or explicitly trusted certs
   to succeed without configuring the surrounding operating system.

   The sequence is basically:

    - We configure a verify_callback with SSL_CTX_set_verify().  There's three
      layers of customizable callbacks in OpenSSL[1], but this seems to be the
      most recommended, and also the least invasive.

    - In our verify_callback, we ignore certificates with preverify_ok=1.  This
      indicates that OpenSSL already trusts the cert.  In our case, that means
      the certificate has been explicitly trusted by the application, since
      no certificate store is available in macos.  Thanks, macos.

    - If we see a preverify_ok=0, we check the whole TLS cert chain against
      macos's trusted stores, and if the OS says it's trustworthy we override
      the failure.

    - Typically, that happens once while OpenSSL is evaluating the chain of
      certs.  OpenSSL starts at the root certificate and says, "hey I don't
      know this guy at all!", then we check the whole chain, say "no it's good
      I trust it" and OpenSSL says, "ah ok whatever" and proceeds as if the
      root is trusted.  Then, unless the cert chain is broken, the remaining
      certs will be validated again by OpenSSL and our verify_callback skips
      all callbacks (which have preverify_ok).

    - After chain is verified, OpenSSL does the hostname check.  We
      intentionally configure the macos lookup to not consider the hostname
      verification since we have that covered.

    - Note that hostname verification failures do not result in a call to
      our verify_callback.

   So in the end, we observe the following properties:

    - hostname verification always belongs to OpenSSL.

    - OpenSSL can declare a certificate valid, and macos will not be consulted.

    - OpenSSL cannot declare a certificate invalid, it can only defer to macos.

   If OpenSSL were at risk of declaring invalid certs valid, this would be a
   problem.  But if that were true we'd be toast anyway.


   [1] Three levels of callback overrides in OpenSSL:

    - ssl_verify_cert_chain() will set up an X509_STORE_CTX, which is a
      horrible name for an object used in a single certificate verification
      check.  Then it will pass that X509_STORE_CTX to X509_verify_cert(),
      unless somebody called SSL_CTX_set_cert_verify_callback(), in which case
      the entire certificate validation process is handed off to the "app
      callback".

      If we hooked in at that level, we would have to reimplement explicit
      certificate trust and verify_hostname overrides with the macos library,
      but we wouldn't have the duplicate checks anymore.

    - X509_verify_cert() does some basic validity checks on the X509_STORE_CTX
      object itself and then calls verify_chain().  verify_chain() does a bunch
      of validity checks on the cert (but not checking signatures or trust),
      then it calls internal_verify() unless somebody called
      X509_STORE_set_verify(), in which case the verify function (not the
      "verify callback") is used instead of the internal_verify().

    - internal_verify() walks through the chain of certs and tries to verify
      their trust and the chain of signatures.  At each failure, it calls
      the verify callback in case the user needs to override the failure.

      Note that the verify_callback is used in other places than just
      internal_verify(); it's not right to assume that the verify function and
      the verify callback are mutually exclusive.
*/
static int macos_verify_callback(int preverify_ok, X509_STORE_CTX *ctx){
    /* Don't interfere with successful checks.  Since we don't have access to
       the list of system root certs, this could happen with a root cert if
       we explicitly trusted the CA in the application.  It could also happen
       if we're looking at an intermediate or leaf certificate that is valid,
       given a root certificate that we've overridden to be trusted. */
    fprintf(stderr, "preverify_ok: %d\n", preverify_ok);
    if(preverify_ok) return preverify_ok;

    int ok = 0;

    int err = X509_STORE_CTX_get_error(ctx);
    int depth = X509_STORE_CTX_get_error_depth(ctx);
    fprintf(stderr, "err = %d, depth = %d, ntrusted = %d, ntotal = %d\n",
            err, depth,
            (int)sk_X509_num(X509_STORE_CTX_get0_chain(ctx)),
            (int)sk_X509_num(X509_STORE_CTX_get0_untrusted(ctx))
    );

    /* prepare to pass a CFArrayRef of SecCertificateRef objects, starting
       with the leaf-most certificate, to SecTrustCreateWithCertificates */

    unsigned char *udata[MAX_DEPTH] = {0};
    const void *certs[MAX_DEPTH] = {0};
    CFIndex ncerts = 0;
    CFDataRef certdata = NULL;
    CFArrayRef certs_array = NULL;
    SecPolicyRef policy = NULL;
    SecTrustRef trust = NULL;
    CFErrorRef cferror = NULL;

    STACK_OF(X509) *sk = X509_STORE_CTX_get0_chain(ctx);

    for(int i = 0; i < sk_X509_num(sk); i++){
        // get the X509 at this depth
        X509 *x509 = sk_X509_value(sk, 0);

        // get der-encoded data
        int ret = i2d_X509(x509, &udata[i]);
        if(ret < 0){
            LOG_ERROR("in macos_verify_callback: i2d_X509(): %x\n", FSSL);
            goto cu;
        }

        /* pass kCFAllocatorNull because I don't know who should free it in
           error situations if we let the core foundation free udata */
        certdata = CFDataCreateWithBytesNoCopy(
            kCFAllocatorDefault, // CFAllocatorRef allocator
            udata[i],            // const UInt8 *bytes
            ret,                 // CFIndex length
            kCFAllocatorNull     // CFAllocatorRef bytesDeallocator
        );
        if(!certdata){
            LOG_ERROR("CFDataCreateWithBytesNoCopy failed\n");
            goto cu;
        }

        SecCertificateRef cert = SecCertificateCreateWithData(NULL, certdata);
        if(!cert){
            goto cu;
        }
        // certdata now owned by cert
        certdata = NULL;

        certs[ncerts++] = (void*)cert;
    }

    // create the certs array
    certs_array = CFArrayCreate(NULL, certs, ncerts, NULL);
    if(!certs_array){
        LOG_ERROR("CFArrayCreate failed\n");
        goto cu;
    }

    // create the SSL evaluation policy for our SecTrust request
    policy = SecPolicyCreateSSL(
        false, // server
        NULL   // hostname (we rely on openssl for that)
    );

    // create the SecTrust request object
    OSStatus osret = SecTrustCreateWithCertificates(
        certs_array, // CFTypeRef certificates
        policy,      // CFTypeRef policies
        &trust       // SecTrustRef *trust
    );
    if(osret != errSecSuccess){
        LOG_ERROR("SecTrustCreateWithCertificates(): %x\n", FMACOS(osret));
        goto cu;
    }

    // now actually check the certificate chain
    bool trusted = SecTrustEvaluateWithError(trust, &cferror);
    if(cferror){
        // we failed to evalue trust
        LOG_ERROR("SecTrustEvaluateWithError(): %x\n", FCFERROR(cferror));
        goto cu;
    }

    if(trusted){
        LOG_ERROR("yo actually we do trust this cert\n");
        ok = 1;
        // erase the error entirely
        X509_STORE_CTX_set_error(ctx, X509_V_OK);
    }

cu:
    if(cferror) CFRelease(cferror);
    if(trust) CFRelease(trust);
    if(policy) CFRelease(policy);
    if(certdata) CFRelease(certdata);
    if(certs_array){
        // free the array, let the array free the elements
        CFRelease(certs_array);
    }else{
        // free the elements we created before failing
        for(CFIndex i = 0; i < ncerts; i++){
            if(certs[i] == NULL) break;
            CFRelease(certs[i]);
        }
    }
    return ok;
}

derr_t ssl_context_load_from_os(ssl_context_t *ctx){
    derr_t e = E_OK;

    SSL_CTX_set_verify_depth(ctx->ctx, MAX_DEPTH);
    SSL_CTX_set_verify(ctx->ctx, SSL_VERIFY_PEER, &macos_verify_callback);

    return e;
}

#else // not __APPLE__

// code for loading OS CA certificates in Linux
derr_t ssl_context_load_from_os(ssl_context_t* ctx){
    derr_t e = E_OK;

    bool ok;
    // archlinux and debian location
    char* location = "/etc/ssl/certs/ca-certificates.crt";
    PROP(&e, dexists(location, &ok) );
    if(!ok){
        // fedora location
        location = "/etc/pki/tls/certs/ca-bundle.crt";
        PROP(&e, dexists(location, &ok) );
        if(!ok){
            ORIG(&e, E_SSL, "could not find any verify locations");
        }
    }

    LOG_DEBUG("chose verify_location: %x\n", FS(location));

    int ret = SSL_CTX_load_verify_locations(ctx->ctx, location, NULL);
    if(ret != 1){
        ORIG(&e,
            E_SSL, "failed to load verify_location(%x): %x", FS(location), FSSL
        );
    }
    return e;
}

#endif // __APPLE__

#endif // _WIN32
