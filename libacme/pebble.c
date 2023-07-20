#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "libdstr/libdstr.h"
#include "libcrypto/libcrypto.h"
#include "libacme/pebble.h"


inline derr_t trust_pebble(SSL_CTX *ctx){
    derr_t e = E_OK;

    // hard-coded copy of pebble cert, for testing against pebble
    DSTR_STATIC(pebble_ca,
        "-----BEGIN CERTIFICATE-----\n"
        "MIIDCTCCAfGgAwIBAgIIJOLbes8sTr4wDQYJKoZIhvcNAQELBQAwIDEeMBwGA1UE\n"
        "AxMVbWluaWNhIHJvb3QgY2EgMjRlMmRiMCAXDTE3MTIwNjE5NDIxMFoYDzIxMTcx\n"
        "MjA2MTk0MjEwWjAgMR4wHAYDVQQDExVtaW5pY2Egcm9vdCBjYSAyNGUyZGIwggEi\n"
        "MA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQC5WgZNoVJandj43kkLyU50vzCZ\n"
        "alozvdRo3OFiKoDtmqKPNWRNO2hC9AUNxTDJco51Yc42u/WV3fPbbhSznTiOOVtn\n"
        "Ajm6iq4I5nZYltGGZetGDOQWr78y2gWY+SG078MuOO2hyDIiKtVc3xiXYA+8Hluu\n"
        "9F8KbqSS1h55yxZ9b87eKR+B0zu2ahzBCIHKmKWgc6N13l7aDxxY3D6uq8gtJRU0\n"
        "toumyLbdzGcupVvjbjDP11nl07RESDWBLG1/g3ktJvqIa4BWgU2HMh4rND6y8OD3\n"
        "Hy3H8MY6CElL+MOCbFJjWqhtOxeFyZZV9q3kYnk9CAuQJKMEGuN4GU6tzhW1AgMB\n"
        "AAGjRTBDMA4GA1UdDwEB/wQEAwIChDAdBgNVHSUEFjAUBggrBgEFBQcDAQYIKwYB\n"
        "BQUHAwIwEgYDVR0TAQH/BAgwBgEB/wIBADANBgkqhkiG9w0BAQsFAAOCAQEAF85v\n"
        "d40HK1ouDAtWeO1PbnWfGEmC5Xa478s9ddOd9Clvp2McYzNlAFfM7kdcj6xeiNhF\n"
        "WPIfaGAi/QdURSL/6C1KsVDqlFBlTs9zYfh2g0UXGvJtj1maeih7zxFLvet+fqll\n"
        "xseM4P9EVJaQxwuK/F78YBt0tCNfivC6JNZMgxKF59h0FBpH70ytUSHXdz7FKwix\n"
        "Mfn3qEb9BXSk0Q3prNV5sOV3vgjEtB4THfDxSz9z3+DepVnW3vbbqwEbkXdk3j82\n"
        "2muVldgOUgTwK8eT+XdofVdntzU/kzygSAtAQwLJfn51fS1GvEcYGBc1bDryIqmF\n"
        "p9BI7gVKtWSZYegicA==\n"
        "-----END CERTIFICATE-----\n"
    );

    X509_STORE *store = SSL_CTX_get_cert_store(ctx);
    if(!store) ORIG(&e, E_SSL, "unable to get store: %x\n", FSSL);

    // write ca bytes in a mem bio
    BIO* pembio = BIO_new_mem_buf((void*)pebble_ca.data, (int)pebble_ca.len);
    if(!pembio){
        ORIG(&e, E_NOMEM, "unable to create BIO: %x", FSSL);
    }

    // read the public key from the BIO (no password protection)
    X509 *x509 = PEM_read_bio_X509(pembio, NULL, NULL, NULL);
    BIO_free(pembio);
    if(!x509){
        ORIG(&e, E_SSL, "unable to read pebble CA: %x", FSSL);
    }

    int ret = X509_STORE_add_cert(store, x509);
    X509_free(x509);
    if(ret != 1){
        ORIG(&e, E_SSL, "unable to add cert to store cert: %x\n", FSSL);
    }

    return e;
}
