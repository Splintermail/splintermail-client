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
        trace_ssl_errors(&e);
        ORIG(&e, E_SSL, "failed to limit SSL protocols");
    }
#else
    // post-1.1.0
    long lret = SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if(lret != 1){
        trace_ssl_errors(&e);
        ORIG(&e, E_SSL, "failed to limit SSL protocols");
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
        trace_ssl_errors(&e);
        ORIG(&e, E_NOMEM, "failed to create SSL context");
    }
#else
    // openssl 1.1.0 API
    const SSL_METHOD* meth = TLS_client_method();
    ctx->ctx = NULL;
    ctx->ctx = SSL_CTX_new(meth);
    if(!ctx->ctx){
        trace_ssl_errors(&e);
        ORIG(&e, E_NOMEM, "failed to create SSL context");
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
            trace_ssl_errors(&e);
            ORIG_GO(&e, E_SSL, "X509_STORE_load_file failed", cleanup);
        }
    }

    /* no reason to accept weak ciphers with splintermail.com.  Note that the
       server is set to choose the cipher and the server is easier to keep
       up-to-date, but this is a good precaution. */
    ret = SSL_CTX_set_cipher_list(ctx->ctx, PREFERRED_CIPHERS);
    if(ret != 1){
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_SSL, "could not set ciphers", cleanup);
    }

    // read/write operations should only return after handshake completed
    lret = SSL_CTX_set_mode(ctx->ctx, SSL_MODE_AUTO_RETRY);
    if(!(lret & SSL_MODE_AUTO_RETRY)){
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_SSL, "error setting SSL mode", cleanup);
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
        trace_ssl_errors(&e);
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
        trace_ssl_errors(&e);
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
            trace_ssl_errors(&e);
            ORIG_GO(&e, E_SSL, "failed to convert system cert to SSL cert", cu_store);
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

static void sec_perror(derr_t *e, OSStatus err){
    CFStringRef str = SecCopyErrorMessageString(err, NULL);
    if(!str){
        TRACE(e, "failed to get error message\n");
    }
    const char* buf;
    buf = CFStringGetCStringPtr(str, kCFStringEncodingUTF8);
    TRACE(e, "%x\n", FS(buf));
    CFRelease(str);
}


derr_t ssl_context_load_from_os(ssl_context_t* ctx){
    derr_t e = E_OK;
    // start building a certificate store
    X509_STORE* store = X509_STORE_new();
    if(!store){
        trace_ssl_errors(&e);
        ORIG(&e, E_NOMEM, "X509_STORE_new failed");
    }

    // open SystemRoot keychain
    SecKeychainRef kc_sysroot;
    char* srpath = "/System/Library/Keychains/SystemRootCertificates.keychain";
    OSStatus osret = SecKeychainOpen(srpath, &kc_sysroot);
    if(osret != errSecSuccess){
        sec_perror(&e, osret);
        TRACE(&e, "keychain: %x\n", FS(srpath));
        ORIG_GO(&e, E_OS, "unable to open keychain", cu_store);
    }

    // open System keychain
    SecKeychainRef kc_sys;
    char* spath = "/Library/Keychains/System.keychain";
    osret = SecKeychainOpen(spath, &kc_sys);
    if(osret != errSecSuccess){
        sec_perror(&e, osret);
        TRACE(&e, "keychain: %x\n", FS(spath));
        ORIG_GO(&e, E_OS, "unable to open keychain", cu_kc_sysroot);
    }

    // create an array of all the keychains we want to search
    const void* kcs[] = {kc_sysroot, kc_sys};
    CFArrayRef keychains = CFArrayCreate(NULL, kcs,
                                         sizeof(kcs)/sizeof(*kcs),
                                         NULL);
    if(!keychains){
        ORIG_GO(&e, E_NOMEM, "unable to create array of keychains", cu_kc_sys);
    }

    // prep the search
    /* we want:
        - only get certificates (kSecClass = Certificate)
        - return the actual SecCertificateRef (kSecReturnRef = True)
        - we want all the certs (kSecMatchLimit = All)
        - only trusted certificates (kSecMatchTrustedOnly = True)
        - no user authentication required (kSecUseAuthenticationUI = Skip)
        - search specified keychains (kSecMatchSearchList = CFArray)
    */
    CFDictionaryRef query;
    const void* keys[] = {
                          kSecClass,
                          kSecReturnRef,
                          kSecMatchLimit,
                          kSecMatchTrustedOnly,
                          kSecUseAuthenticationUI,
                          kSecMatchSearchList,
                         };
    const void* vals[] = {
                          kSecClassCertificate,
                          kCFBooleanTrue,
                          kSecMatchLimitAll,
                          kCFBooleanTrue,
                          kSecUseAuthenticationUISkip,
                          keychains,
                         };
    query = CFDictionaryCreate(NULL, (const void**)keys, (const void**)vals,
                               sizeof(keys)/sizeof(*keys), NULL, NULL);
    if(!query){
        ORIG_GO(&e, E_NOMEM, "unable to create search query", cu_keychains);
    }

    // do the search
    CFArrayRef results;
    osret = SecItemCopyMatching(query, (CFTypeRef*)&results);
    if(osret != errSecSuccess){
        sec_perror(&e, osret);
        ORIG_GO(&e, E_OS, "failure executing search", cu_dict);
    }

    // get count
    CFIndex count = CFArrayGetCount(results);
    // LOG_DEBUG("count %x\n", FI(count));

    // add each cert in keychain to certificate store
    for(long i = 0; i < count; i++){
        // get the cert
        // --- this line throws a compiler warning, but not sure how to fix it:
        // const SecCertificateRef cert = CFArrayGetValueAtIndex(results, i);
        // --- this fixes the compiler warning, using some non-public type:
        const struct OpaqueSecCertificateRef *cert =
                                         CFArrayGetValueAtIndex(results, i);

        // // get the subject summary
        // CFStringRef summary = SecCertificateCopySubjectSummary(cert);
        // if(!summary){
        //     ORIG_GO(&e, E_OS, "failure copying subject summary", cu_results);
        // }
        // // copy the summary to a buffer
        // const char* buffer = CFStringGetCStringPtr(summary, kCFStringEncodingUTF8);
        // printf("%s\n", buffer);
        // // free the summary
        // CFRelease(summary);

        // export this key to der-encoded X509 format
        CFDataRef exported;
        SecExternalFormat format = kSecFormatX509Cert;
        SecItemImportExportFlags flags = 0;
        // we are only exporting certificate items, not key items
        SecItemImportExportKeyParameters *keyparams = NULL;
        // do the export
        osret = SecItemExport(cert, format, flags, keyparams, &exported);
        if(osret != errSecSuccess){
            sec_perror(&e, osret);
            ORIG_GO(&e, E_OS, "failure in export", cu_results);
        }

        // get pointer and len
        // (we need to use a separate pointer because d2i_X509 changes it)
        const unsigned char* ptr = CFDataGetBytePtr(exported);
        long len = CFDataGetLength(exported);

        // create OpenSSL key from der-encoding
        X509* x = d2i_X509(NULL, &ptr, len);
        if(x == NULL){
            trace_ssl_errors(&e);
            ORIG_GO(&e, E_SSL, "failed to convert system cert to SSL cert", cu_results);
        }

        // add cert to the store
        int ret = X509_STORE_add_cert(store, x);
        if(ret != 1){
            ORIG_GO(&e, E_SSL, "Unable to add certificate to store", cu_x);
        }
        // LOG_DEBUG("Added to store!\n");

    cu_x:
        X509_free(x);
        if(is_error(e)) goto cu_results;
    }

    // now add store to the context
    //SSL_CTX_set0_verify_cert_store(ctx->ctx, store);
    SSL_CTX_set_cert_store(ctx->ctx, store);
    // no errors possible with this function

cu_results:
    // for(long i = 0; i < count; i++){
    //     CFRelease(CFArrayGetValueAtIndex(results, i));
    // }
    // This pukes if I try to free whats in the array and also free this:
    CFRelease(results);
cu_dict:
    CFRelease(query);
cu_keychains:
    CFRelease(keychains);
cu_kc_sys:
    CFRelease(kc_sys);
cu_kc_sysroot:
    CFRelease(kc_sysroot);

cu_store:
    // don't cleanup cu_store, it will be cleaned up automatically later
    if(is_error(e)) X509_STORE_free(store);

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
        TRACE(&e, "failed to load verify_location: %x\n", FS(location));
        trace_ssl_errors(&e);
        ORIG(&e, E_SSL, "failed to load verify_location");
    }
    return e;
}

#endif // __APPLE__

#endif // _WIN32
