#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>

#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/x509v3.h>
#include <openssl/err.h>
#include <openssl/opensslv.h>

#include "networking.h"
#include "logger.h"
#include "fileops.h"

#include "win_compat.h"

static void log_errors(void){
    unsigned long e;
    while( (e = ERR_get_error()) ){
        char buffer[256];
        ERR_error_string_n(e, buffer, sizeof(buffer));
        LOG_ERROR("OpenSSL error: %x\n", FS(buffer));
    }
}

derr_t ssl_library_init(void){
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    // SSL_library_init depricated in OpenSSL 1.1.0
    SSL_library_init();
    // load_error_strings depricated as well
    SSL_load_error_strings();
#endif
    // calling the new OPENSSL_init_ssl() explicitly not strictly necessary
    return E_OK;
}

void ssl_library_close(void){
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    ERR_free_strings();
#endif
}

derr_t connection_new(connection_t* conn,
                        const char* addr, unsigned int port){
    derr_t error;
    // combine address and port
    DSTR_VAR(addr_port, 256);
    error = FMT(&addr_port, "%x:%x", FS(addr), FU(port));
    CATCH(E_FIXEDSIZE){
        LOG_ERROR("address too long\n");
        RETHROW(E_PARAM);
    }else PROP(error);

    // make connection
    conn->bio = BIO_new_connect(addr_port.data);
    if(!conn->bio){
        log_errors();
        ORIG(E_NOMEM, "error creating bio");
    }

    // connect BIO
    // BIO_do_connect is a macro which includes a -Wconversion-unsafe cast
    // so we will use the low-level function, taken from openssl/bio.h
    //int ret = BIO_do_connect(conn->bio);
    // a future conversion of the networking to use getaddrinfo will fix this
    long ret = BIO_ctrl(conn->bio, BIO_C_DO_STATE_MACHINE, 0, NULL);
    if(ret != 1){
        log_errors();
        ORIG_GO(E_CONN, "error making connection", fail);
    }

    return E_OK;

fail:
    BIO_free_all(conn->bio);
    conn->bio = NULL;
    return error;
}

derr_t listener_new(listener_t* l, const char* addr, unsigned int port){
    derr_t error = E_OK;
    // combine address and port
    DSTR_VAR(addr_port, 256);
    error = FMT(&addr_port, "%x:%x", FS(addr), FU(port));
    CATCH(E_FIXEDSIZE){
        LOG_ERROR("address too long\n");
        RETHROW(E_PARAM);
    }else PROP(error);

    // open a listener
    l->bio = BIO_new_accept(addr_port.data);
    if(!l->bio){
        log_errors();
        ORIG(E_SOCK, "unable to create listener bio");
    }

    // set SO_REUSEADDR
    // BIO_set_bind_mode is a macro which includes a -Wconversion-unsafe cast
    // so we will use the low-level function, taken from openssl/bio.h
    // int ret = BIO_set_bind_mode(l->bio, BIO_BIND_REUSEADDR);
    // a future conversion of the networking to use getaddrinfo will fix this
    long ret = BIO_ctrl(l->bio, BIO_C_SET_BIND_MODE, BIO_BIND_REUSEADDR, NULL);
    if(ret != 1){
        log_errors();
        ORIG_GO(E_SSL, "failed to set REUSEADDR", fail);
    }

    // the first time this is called it binds to the port
    // ret = BIO_do_accept(l->bio);
    ret = BIO_ctrl(l->bio, BIO_C_DO_STATE_MACHINE, 0, NULL);
    if(ret != 1){
        log_errors();
        ORIG_GO(E_SSL, "failed to bind to port", fail);
    }

    return error;
fail:
    BIO_free_all(l->bio);
    l->bio = NULL;
    return error;
}

derr_t ssl_context_new_client(ssl_context_t* ctx){
    derr_t error;
    long lret;
    // pick method
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    // openssl pre-1.1.0 API
    // allow client to start talking to anybody
    const SSL_METHOD* meth = SSLv23_client_method();
    ctx->ctx = NULL;
    ctx->ctx = SSL_CTX_new(meth);
    if(!ctx->ctx){
        log_errors();
        ORIG(E_NOMEM, "failed to create SSL context");
    }
    // restrict client to only settle on TLS
    lret = SSL_CTX_set_options(ctx->ctx, SSL_OP_NO_SSLv2
                                              | SSL_OP_NO_SSLv3);
    if(!(lret & (SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3))){
        log_errors();
        ORIG_GO(E_SSL, "failed to limit SSL methods", cleanup);
    }
#else
    // openssl 1.1.0 API
    const SSL_METHOD* meth = TLS_client_method();
    ctx->ctx = NULL;
    ctx->ctx = SSL_CTX_new(meth);
    if(!ctx->ctx){
        log_errors();
        ORIG(E_NOMEM, "failed to create SSL context");
    }
#endif


    // load SSL certificate location
    PROP_GO( ssl_context_load_from_os(ctx), cleanup);

    /* no reason to accept weak ciphers with splintermail.com.  Note that the
       server is set to choose the cipher and the server is easier to keep
       up-to-date, but this is a good precaution. */
    int ret = SSL_CTX_set_cipher_list(ctx->ctx, PREFERRED_CIPHERS);
    if(ret != 1){
        log_errors();
        ORIG_GO(E_SSL, "could not set ciphers", cleanup);
    }

    // read/write operations should only return after handshake completed
    lret = SSL_CTX_set_mode(ctx->ctx, SSL_MODE_AUTO_RETRY);
    if(!(lret & SSL_MODE_AUTO_RETRY)){
        log_errors();
        ORIG_GO(E_SSL, "error setting SSL mode", cleanup);
    }

    return E_OK;
cleanup:
    SSL_CTX_free(ctx->ctx);
    ctx->ctx = NULL;
    return error;
}

derr_t ssl_context_new_server(ssl_context_t* ctx, const char* certfile,
                              const char* keyfile, const char* dhfile){
    derr_t error;

    // make sure certfile is a real file and that we have access
    if(!file_r_access(certfile)){
        ORIG(E_FS, "unable to access certfile");
    }
    // make sure keyfile is a real file and that we have access
    if(!file_r_access(keyfile)){
        ORIG(E_FS, "unable to access keyfile");
    }
    // make sure dhfile is a real file and that we have access
    if(dhfile && !file_r_access(dhfile)){
        ORIG(E_FS, "unable to access dhfile");
    }

    long lret;

    // pick method
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    // openssl pre-1.1.0 API
    // allow server to start talking to anybody
    const SSL_METHOD* meth = SSLv23_server_method();
    ctx->ctx = NULL;
    ctx->ctx = SSL_CTX_new(meth);
    if(!ctx->ctx){
        log_errors();
        ORIG(E_NOMEM, "failed to create SSL context");
    }
    // restrict server to only settle on TLS
    lret = SSL_CTX_set_options(ctx->ctx, SSL_OP_NO_SSLv2
                                              | SSL_OP_NO_SSLv3);
    if(!(lret & (SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3))){
        log_errors();
        ORIG_GO(E_SSL, "failed to limit SSL methods", cleanup);
    }
    long ulret;
#else
    // openssl 1.1.0 API
    // allow server to start talking to anybody
    const SSL_METHOD* meth = TLS_server_method();
    ctx->ctx = NULL;
    ctx->ctx = SSL_CTX_new(meth);
    if(!ctx->ctx){
        log_errors();
        ORIG(E_NOMEM, "failed to create SSL context");
    }
    unsigned long ulret;
#endif

    // set key and cert
    int ret = SSL_CTX_use_certificate_chain_file(ctx->ctx, certfile);
    if(ret != 1){
        log_errors();
        ORIG_GO(E_SSL, "could not set certificate", cleanup);
    }
    ret = SSL_CTX_use_PrivateKey_file(ctx->ctx, keyfile, SSL_FILETYPE_PEM);
    if(ret != 1){
        log_errors();
        ORIG_GO(E_SSL, "could not set private key", cleanup);
    }
    // make sure the key matches the certificate
    ret = SSL_CTX_check_private_key(ctx->ctx);
    if(ret != 1){
        log_errors();
        ORIG_GO(E_SSL, "private key does not match certificate", cleanup);
    }

    // set diffie-helman perameters
    if(dhfile){
        DH* dh = NULL;
        FILE* fp = fopen(dhfile, "r");
        if(!fp){
            // already checked read access above, so highly likely E_NOMEM
            ORIG_GO(E_NOMEM, "unable to open dhfile", cleanup);
        }
        dh = PEM_read_DHparams(fp, NULL, NULL, NULL);
        fclose(fp);
        if(!dh){
            log_errors();
            ORIG_GO(E_SSL, "failed to read dh params", cleanup);
        }
        lret = SSL_CTX_set_tmp_dh(ctx->ctx, dh);
        if(lret != 1){
            DH_free(dh);
            log_errors();
            ORIG_GO(E_SSL, "failed to set dh params", cleanup);
        }
        DH_free(dh);
    }

    // make sure server sets cipher preference
    ulret = SSL_CTX_set_options(ctx->ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
    if( !(ulret & SSL_OP_CIPHER_SERVER_PREFERENCE) ){
        log_errors();
        ORIG_GO(E_SSL, "failed to set server cipher preference ", cleanup);
    }

    ret = SSL_CTX_set_cipher_list(ctx->ctx, PREFERRED_CIPHERS);
    if(ret != 1){
        log_errors();
        ORIG_GO(E_SSL, "could not set ciphers", cleanup);
    }

    // read/write operations should only return after handshake completed
    lret = SSL_CTX_set_mode(ctx->ctx, SSL_MODE_AUTO_RETRY);
    if(!(lret & SSL_MODE_AUTO_RETRY)){
        log_errors();
        ORIG_GO(E_SSL, "error setting SSL mode", cleanup);
    }

    return E_OK;
cleanup:
    SSL_CTX_free(ctx->ctx);
    ctx->ctx = NULL;
    return error;
}

void ssl_context_free(ssl_context_t* ctx){
    if(ctx->ctx){
        SSL_CTX_free(ctx->ctx);
    }
    ctx->ctx = NULL;
}

derr_t connection_new_ssl(connection_t* conn, ssl_context_t* ctx,
                          const char* addr, unsigned int port){
    derr_t error;
    // combine address and port
    DSTR_VAR(addr_port, 256);
    error = FMT(&addr_port, "%x:%x", FS(addr), FU(port));
    CATCH(E_FIXEDSIZE){
        LOG_ERROR("address too long\n");
        RETHROW(E_PARAM);
    }else PROP(error);

    // make encrypted connection
    conn->bio = BIO_new_ssl_connect(ctx->ctx);
    if(!conn->bio){
        log_errors();
        ORIG(E_NOMEM, "error creating bio");
    }

    // get ssl pointer from bio
    SSL* ssl = NULL;
    BIO_get_ssl(conn->bio, &ssl);
    if(!ssl){
        log_errors();
        ORIG_GO(E_INTERNAL, "error getting ssl from bio", fail_1);
    }

    // set hostname and port, always returns 1
    BIO_set_conn_hostname(conn->bio, addr_port.data);

    // connect BIO
    // BIO_do_connect is a macro which includes a -Wconversion-unsafe cast
    // so we will use the low-level function, taken from openssl/bio.h
    //int ret = BIO_do_connect(conn->bio);
    // a future conversion of the networking to use getaddrinfo will fix this
    long ret = BIO_ctrl(conn->bio, BIO_C_DO_STATE_MACHINE, 0, NULL);
    if(ret != 1){
        log_errors();
        ORIG_GO(E_CONN, "error making connection", fail_1);
    }

    // is SSL certificate verified (cryptographically sound)?
    long lret = SSL_get_verify_result(ssl);
    if(lret != X509_V_OK){
        log_errors();
        ORIG_GO(E_SSL, "unverified certificate", fail_1);
    }

    // check name on certificate
    X509* peer = SSL_get_peer_certificate(ssl);
    if(!peer){
        log_errors();
        /* according to man page, a client-side call to SSL_get_peer_certificate
           can only return NULL if we are using anonymous ciphers or we are not
           connected */
        ORIG_GO(E_INTERNAL, "error getting peer certificate", fail_1);
    }
    // X509_NAME* name_on_cert = X509_get_subject_name(peer);
    // char namebuf[1024];
    // X509_NAME_oneline(name_on_cert, namebuf, 1024 - 1);
    // LOG_DEBUG("ssl connected to %x\n", FS(namebuf));
    ret = X509_check_host(peer, addr, strlen(addr), 0, NULL);
    if(ret == 0){
        log_errors();
        ORIG_GO(E_SSL, "server name does not match certificate", fail_2);
    }else if(ret != 1){
        log_errors();
        ORIG_GO(E_INTERNAL, "library error checking server name", fail_2);
    }

    X509_free(peer);
    return E_OK;

fail_2:
    X509_free(peer);
fail_1:
    BIO_free_all(conn->bio);
    conn->bio = NULL;
    return error;
}
derr_t connection_read(connection_t* conn, dstr_t* out, size_t *amount_read){
    int ret;
    if(out->fixed_size){
        // try to fill buffer
        int bytes_to_read = (int)MIN(out->size - out->len, INT_MAX);
        if(bytes_to_read == 0) ORIG(E_FIXEDSIZE, "can't read to full buffer");
        ret = BIO_read(conn->bio, out->data + out->len, bytes_to_read);
        if(ret > 0) out->len += (size_t)ret;
    }else{
        // just read an arbitrary chunk
        DSTR_VAR(temp, 4096);
        int bytes_to_read = (int)MIN(temp.size, INT_MAX);
        ret = BIO_read(conn->bio, temp.data, bytes_to_read);
        if(ret > 0){
            temp.len = (size_t)ret;
            PROP( dstr_append(out, &temp) );
        }
    }
    // amount_read == NULL means "please throw error on broken connection"
    if(!amount_read && ret <= 0){
        log_errors();
        ORIG(E_CONN, "broken connection");
    }
    if(amount_read) *amount_read = (size_t)ret;
    return E_OK;
}

derr_t connection_write(connection_t* conn, const dstr_t* in){
    // repeatedly try to write until we write the whole buffer
    size_t total = 0;
    while(total < in->len){
        int bytes_to_write = (int)MIN(in->len - total, INT_MAX);
        int written = BIO_write(conn->bio, in->data + total, bytes_to_write);
        // writing zero or fewer bytes is a failure mode
        if(written <= 0){
            log_errors();
            ORIG(E_CONN, "broken connection");
        }
        total += (size_t)written;
    }

    return E_OK;
}
void connection_close(connection_t* conn){
    if(conn->bio){
        BIO_free_all(conn->bio);
        conn->bio = NULL;
    }
}

derr_t listener_new_ssl(listener_t* l, ssl_context_t* ctx,
                          const char* addr, unsigned int port){
    derr_t error = E_OK;
    // combine address and port
    DSTR_VAR(addr_port, 256);
    error = FMT(&addr_port, "%x:%x", FS(addr), FU(port));
    CATCH(E_FIXEDSIZE){
        LOG_ERROR("address too long\n");
        RETHROW(E_PARAM);
    }else PROP(error);

    // open a listener
    l->bio = BIO_new_accept(addr_port.data);
    if(!l->bio){
        log_errors();
        ORIG(E_SOCK, "unable to create listener bio");
    }

    // set SO_REUSEADDR
    // BIO_set_bind_mode is a macro which includes a -Wconversion-unsafe cast
    // so we will use the low-level function, taken from openssl/bio.h
    // int ret = BIO_set_bind_mode(l->bio, BIO_BIND_REUSEADDR);
    // a future conversion of the networking to use getaddrinfo will fix this
    long ret = BIO_ctrl(l->bio, BIO_C_SET_BIND_MODE, BIO_BIND_REUSEADDR, NULL);
    if(ret != 1){
        log_errors();
        ORIG_GO(E_SSL, "failed to set REUSEADDR", fail);
    }

    // build the SSL bio chain
    BIO* ssl_bio = BIO_new_ssl(ctx->ctx, 0);
    if(!ssl_bio){
        log_errors();
        ORIG_GO(E_SSL, "unable to create ssl_bio", fail);
    }

    // attach the SSL bio chain
    long lret = BIO_set_accept_bios(l->bio, ssl_bio);
    if(lret != 1){
        BIO_free_all(ssl_bio);
        ssl_bio = NULL;
        log_errors();
        ORIG_GO(E_SSL, "unable to attach ssl_bio", fail);
    }

    // the first time this is called it binds to the port
    // ret = BIO_do_accept(l->bio);
    ret = BIO_ctrl(l->bio, BIO_C_DO_STATE_MACHINE, 0, NULL);
    if(ret != 1){
        log_errors();
        ORIG_GO(E_SSL, "failed to bind to port", fail);
    }

    return error;
fail:
    BIO_free_all(l->bio);
    l->bio = NULL;
    return error;
}

void listener_close(listener_t* l){
    if(l->bio){
        BIO_free_all(l->bio);
        l->bio = NULL;
    }
}

derr_t listener_accept(listener_t* l, connection_t* conn){
    // after the first time, BIO_do_accept() waits for an incoming connection
    derr_t error;
    // int ret = BIO_do_accept(l->bio);
    long ret = BIO_ctrl(l->bio, BIO_C_DO_STATE_MACHINE, 0, NULL);
    if(ret != 1){
        log_errors();
        ORIG(E_SSL, "failed to accept connection");
    }

    conn->bio = BIO_pop(l->bio);
    if(!conn->bio){
        log_errors();
        ORIG_GO(E_SSL, "failed to pop accept connection", fail);
    }

    return E_OK;

fail:
    /* TODO: there should probably be a cleanup step here after
       BIO_do_accept() but I am not sure what it would be */
    return error;
}

#ifdef _WIN32
#include <wincrypt.h>
// code for loading OS CA certificates in windows
derr_t ssl_context_load_from_os(ssl_context_t* ctx){
    derr_t error = E_OK;
    // https://msdn.microsoft.com/en-us/library/windows/desktop/aa376560.aspx
    HCERTSTORE hStore = CertOpenSystemStoreA((HCRYPTPROV_LEGACY)NULL, "ROOT");
    if(hStore == NULL){
        win_perror();
        ORIG(E_OS, "failed to open system certificate store");
    }

    PCCERT_CONTEXT wincert = NULL;

    // start building a certificate store
    X509_STORE* store = X509_STORE_new();
    if(!store){
        log_errors();
        ORIG_GO(E_NOMEM, "X509_STORE_new failed", cu_winstore);
    }

    // https://msdn.microsoft.com/en-us/library/windows/desktop/aa376560.aspx
    while((wincert = CertEnumCertificatesInStore(hStore, wincert)) != NULL){
        // create OpenSSL key from der-encoding
        // (we need to use a separate pointer because d2i_X509 changes it)
        const unsigned char* ptr = wincert->pbCertEncoded;
        long len = wincert->cbCertEncoded;
        X509* x = d2i_X509(NULL, &ptr, len);
        if(x == NULL){
            log_errors();
            ORIG_GO(E_SSL, "failed to convert system cert to SSL cert", cu_store);
        }

        // add cert to the store
        int ret = X509_STORE_add_cert(store, x);
        if(ret != 1){
            ORIG_GO(E_SSL, "Unable to add certificate to store", cu_x);
        }

    cu_x:
        X509_free(x);
        if(error) goto cu_store;
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
    return error;
}

#else // not _WIN32

#ifdef __APPLE__
// code for loading OS CA certificates in osx
#include <Security/SecItem.h>
#include <Security/SecImportExport.h>
#include <Security/SecCertificate.h>

static void sec_perror(OSStatus err){
    CFStringRef str = SecCopyErrorMessageString(err, NULL);
    if(!str){
        LOG_ERROR("failed to get error message\n");
    }
    const char* buf;
    buf = CFStringGetCStringPtr(str, kCFStringEncodingUTF8);
    LOG_ERROR("%x\n", FS(buf));
    CFRelease(str);
}


derr_t ssl_context_load_from_os(ssl_context_t* ctx){
    derr_t error = E_OK;
    // start building a certificate store
    X509_STORE* store = X509_STORE_new();
    if(!store){
        log_errors();
        ORIG(E_NOMEM, "X509_STORE_new failed");
    }

    // open SystemRoot keychain
    SecKeychainRef kc_sysroot;
    char* srpath = "/System/Library/Keychains/SystemRootCertificates.keychain";
    OSStatus osret = SecKeychainOpen(srpath, &kc_sysroot);
    if(osret != errSecSuccess){
        sec_perror(osret);
        LOG_ERROR("keychain: %x\n", FS(srpath));
        ORIG_GO(E_OS, "unable to open keychain", cu_store);
    }

    // open System keychain
    SecKeychainRef kc_sys;
    char* spath = "/Library/Keychains/System.keychain";
    osret = SecKeychainOpen(spath, &kc_sys);
    if(osret != errSecSuccess){
        sec_perror(osret);
        LOG_ERROR("keychain: %x\n", FS(spath));
        ORIG_GO(E_OS, "unable to open keychain", cu_kc_sysroot);
    }

    // create an array of all the keychains we want to search
    const void* kcs[] = {kc_sysroot, kc_sys};
    CFArrayRef keychains = CFArrayCreate(NULL, kcs,
                                         sizeof(kcs)/sizeof(*kcs),
                                         NULL);
    if(!keychains){
        ORIG_GO(E_NOMEM, "unable to create array of keychains", cu_kc_sys);
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
        ORIG_GO(E_NOMEM, "unable to create search query", cu_keychains);
    }

    // do the search
    CFArrayRef results;
    osret = SecItemCopyMatching(query, (CFTypeRef*)&results);
    if(osret != errSecSuccess){
        sec_perror(osret);
        ORIG_GO(E_OS, "failure executing search", cu_dict);
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
        //     ORIG_GO(E_OS, "failure copying subject summary", cu_results);
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
            sec_perror(osret);
            ORIG_GO(E_OS, "failure in export", cu_results);
        }

        // get pointer and len
        // (we need to use a separate pointer because d2i_X509 changes it)
        const unsigned char* ptr = CFDataGetBytePtr(exported);
        long len = CFDataGetLength(exported);

        // create OpenSSL key from der-encoding
        X509* x = d2i_X509(NULL, &ptr, len);
        if(x == NULL){
            log_errors();
            ORIG_GO(E_SSL, "failed to convert system cert to SSL cert", cu_results);
        }

        // add cert to the store
        int ret = X509_STORE_add_cert(store, x);
        if(ret != 1){
            ORIG_GO(E_SSL, "Unable to add certificate to store", cu_x);
        }
        // LOG_DEBUG("Added to store!\n");

    cu_x:
        X509_free(x);
        if(error) goto cu_results;
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
    if(error) X509_STORE_free(store);

    return error;
}

#else // not __APPLE__

static void log_errors_as(log_level_t lvl){
    unsigned long e;
    while( (e = ERR_get_error()) ){
        char buffer[256];
        ERR_error_string_n(e, buffer, sizeof(buffer));
        LOG_AS(lvl, "OpenSSL error: %x\n", FS(buffer));
    }
}

// code for loading OS CA certificates in Linux
derr_t ssl_context_load_from_os(ssl_context_t* ctx){
    // archlinux and debian location
    char* location = "/etc/ssl/certs/ca-certificates.crt";
    int ret = SSL_CTX_load_verify_locations(ctx->ctx, location, NULL);
    if(ret != 1){
        log_errors_as(LOG_LVL_DEBUG);
        LOG_DEBUG("failed to load verify_location: %x\n", FS(location));
    }else{
        return E_OK;
    }
    // fedora location
    location = "/etc/pki/tls/certs/ca-bundle.crt";
    ret = SSL_CTX_load_verify_locations(ctx->ctx, location, NULL);
    if(ret != 1){
        log_errors_as(LOG_LVL_DEBUG);
        LOG_DEBUG("failed to load verify_location: %x\n", FS(location));
    }else{
        return E_OK;
    }
    // if we are here we didn't load anything
    ORIG(E_SSL, "could not load any verify locations");
}

#endif // __APPLE__

#endif // _WIN32
