#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

#include "common.h"
#include "logger.h"
#include "crypto.h"
#include "opt_parse.h"

int main(int argc, char** argv){
    // specify command line options
    opt_spec_t o_debug = {'d', "debug", false, OPT_RETURN_INIT};
    opt_spec_t* spec[] = {&o_debug};
    size_t speclen = sizeof(spec) / sizeof(*spec);
    int newargc;
    // parse command line options
    if(opt_parse(argc, argv, spec, speclen, &newargc))
        return 2;

    logger_add_fileptr(o_debug.found ? LOG_LVL_DEBUG : LOG_LVL_INFO, stderr);

    derr_t error;

    if(newargc != 2){
        LOG_ERROR("usage: decrypt_msg <decryption key file>\n");
        return 3;
    }

    // init SSL
    PROP_GO( crypto_library_init(), exit);

    keypair_t kp;
    PROP_GO( keypair_load(&kp, argv[1]), cleanup_ssl);

    // buffer for reading from stdin
    DSTR_VAR(in, 4096);

    // buffer for writing to stdout.
    DSTR_VAR(out, 4096 + 16);

    // buffer for storing message recipients
    LIST_VAR(dstr_t, recips, FL_DEVICES);
    DSTR_VAR(recips_block, FL_DEVICES * FL_FINGERPRINT * 2);

    decrypter_t dec;
    PROP_GO( decrypter_new(&dec), cleanup_key);

    PROP_GO( decrypter_start(&dec, &kp, &recips, &recips_block), cleanup_dec);

    while(true){
        // read ciphertext from stdin
        size_t amnt_read;
        PROP_GO( dstr_read(0, &in, 0, &amnt_read), cleanup_dec);
        if(amnt_read == 0){
            break;
        }

        // decrypt what we read
        PROP_GO( decrypter_update(&dec, &in, &out), cleanup_dec);

        // write the decrypted buffer to stdout
        PROP_GO( dstr_write(1, &out), cleanup_dec);
        out.len = 0;
    }

    // finish the encryption
    PROP_GO( decrypter_finish(&dec, &out), cleanup_dec);

    // write remainder to stdout
    PROP_GO( dstr_write(1, &out), cleanup_dec);
    out.len = 0;

cleanup_dec:
    decrypter_free(&dec);
    // here we will try and print some useful debug information to stderr
    LOG_ERROR("Recipients identified (%x):\n", FU(recips.len));
    for(size_t i = 0; i < recips.len; i++){
        DSTR_VAR(hexfpr, FL_FINGERPRINT * 2);
        PROP( bin2hex(&recips.data[i], &hexfpr) );
        LOG_ERROR("    %x\n", FD(&hexfpr));
    }
cleanup_key:
    keypair_free(&kp);
cleanup_ssl:
    crypto_library_close();
exit:
    return error != E_OK;
}
