#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

#include "libdstr/libdstr.h"
#include "libcrypto/libcrypto.h"

int main(int argc, char** argv){
    derr_t e = E_OK;

    // specify command line options
    opt_spec_t o_debug = {'d', "debug", false};
    opt_spec_t* spec[] = {&o_debug};
    size_t speclen = sizeof(spec) / sizeof(*spec);
    int newargc;
    // parse command line options
    IF_PROP(&e, opt_parse(argc, argv, spec, speclen, &newargc)){
        logger_add_fileptr(LOG_LVL_DEBUG, stderr);
        DUMP(e);
        DROP_VAR(&e);
        return 2;
    }

    logger_add_fileptr(o_debug.found ? LOG_LVL_DEBUG : LOG_LVL_INFO, stderr);

    if(newargc != 2){
        LOG_ERROR("usage: decrypt_msg <decryption key file>\n");
        return 3;
    }

    // init SSL
    PROP_GO(&e, crypto_library_init(), exit);

    keypair_t *kp;
    PROP_GO(&e, keypair_load_private(&kp, argv[1]), cleanup_ssl);

    // buffer for reading from stdin
    DSTR_VAR(in, 4096);

    // buffer for writing to stdout.
    DSTR_VAR(out, 4096 + 16);

    // buffer for storing message recipients
    LIST_VAR(dstr_t, recips, FL_DEVICES);
    DSTR_VAR(recips_block, FL_DEVICES * FL_FINGERPRINT * 2);

    decrypter_t dec;
    PROP_GO(&e, decrypter_new(&dec), cleanup_key);

    PROP_GO(&e, decrypter_start(&dec, kp, &recips, &recips_block), cleanup_dec);

    while(true){
        // read ciphertext from stdin
        size_t amnt_read;
        PROP_GO(&e, dstr_read(0, &in, 0, &amnt_read), cleanup_dec);
        if(amnt_read == 0){
            break;
        }

        // decrypt what we read
        PROP_GO(&e, decrypter_update(&dec, &in, &out), cleanup_dec);

        // write the decrypted buffer to stdout
        PROP_GO(&e, dstr_write(1, &out), cleanup_dec);
        out.len = 0;
    }

    // finish the encryption
    PROP_GO(&e, decrypter_finish(&dec, &out), cleanup_dec);

    // write remainder to stdout
    PROP_GO(&e, dstr_write(1, &out), cleanup_dec);
    out.len = 0;

cleanup_dec:
    decrypter_free(&dec);
    // here we will try and print some useful debug information to stderr
    TRACE(&e, "Recipients identified (%x):\n", FU(recips.len));
    for(size_t i = 0; i < recips.len; i++){
        DSTR_VAR(hexfpr, FL_FINGERPRINT * 2);
        IF_PROP(&e, bin2hex(&recips.data[i], &hexfpr)){}
        TRACE(&e, "    %x\n", FD(&hexfpr));
    }
cleanup_key:
    keypair_free(&kp);
cleanup_ssl:
    crypto_library_close();

    int retval;
exit:
    retval = is_error(e);
    if(is_error(e)){
        DUMP(e);
    }
    DROP_VAR(&e);

    return retval;
}
