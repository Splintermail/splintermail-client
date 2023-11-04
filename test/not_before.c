#include "libdstr/libdstr.h"
#include "libcrypto/libcrypto.h"

#include <stdio.h>

#include <openssl/pem.h>
#include <openssl/x509.h>

// not_before is helper tool for e2e_citm

static derr_t dmain(void){
    derr_t e = E_OK;

    X509 *x509 = NULL;

    PROP(&e, ssl_library_init() );

    x509 = PEM_read_X509(stdin, NULL, NULL, NULL);
    if(!x509){
        ORIG_GO(&e, E_SSL, "failed to read X509 on stdin: %x", cu,  FSSL);
    }

    const ASN1_TIME *not_before = X509_get0_notBefore(x509);
    if(!not_before){
        ORIG_GO(&e, E_SSL, "failed to get notBefore: %x", cu, FSSL);
    }

    struct tm tm;
    int ret = ASN1_TIME_to_tm(not_before, &tm);
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "failed to convert ASN1_TIME: %x", cu, FSSL);
    }

    time_t epoch;
    PROP_GO(&e, dmktime_utc(dtm_from_tm(tm), &epoch), cu);

    PROP_GO(&e, FFMT(stdout, "%x\n", FI(epoch)), cu);

cu:
    X509_free(x509);
    ssl_library_close();
    return e;
}

int main(void){
    derr_t e = dmain();

    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
        return 1;
    }
    return 0;
}
