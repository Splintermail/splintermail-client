#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
//#include <strings.h>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/bn.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/safestack.h>
#include <openssl/engine.h>

#include <libdstr/common.h>
#include <libdstr/logger.h>
#include <crypto.h>

#include "test_utils.h"

#include <libdstr/win_compat.h>

DSTR_STATIC(base64_partial,
    "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4v\n"
    "MDEyMzQ1Njc4OTo7PD0+P0BBQkNERUZHSElKS0xNTk9QUVJTVFVWV1hZWltcXV5f\n"
    "YGFiY2RlZmdoaWprbG1ub3BxcnN0dXZ3eHl6e3x9fn+AgYKDhIWGh4iJiouMjY6P\n"
    "kJGSk5SVlpeYmZqbnJ2en6ChoqOkpaanqKmqq6ytrq+wsbKztLW2t7i5uru8vb6/\n"
    "wMHCw8TFxsfIycrLzM3Oz9DR0tPU1dbX2Nna29zd3t/g4eLj5OXm5+jp6uvs7e7v\n");

DSTR_STATIC(base64_complete,
    "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4v\n"
    "MDEyMzQ1Njc4OTo7PD0+P0BBQkNERUZHSElKS0xNTk9QUVJTVFVWV1hZWltcXV5f\n"
    "YGFiY2RlZmdoaWprbG1ub3BxcnN0dXZ3eHl6e3x9fn+AgYKDhIWGh4iJiouMjY6P\n"
    "kJGSk5SVlpeYmZqbnJ2en6ChoqOkpaanqKmqq6ytrq+wsbKztLW2t7i5uru8vb6/\n"
    "wMHCw8TFxsfIycrLzM3Oz9DR0tPU1dbX2Nna29zd3t/g4eLj5OXm5+jp6uvs7e7v\n"
    "8PHy8/T19vf4+fr7/P3+/w==\n");

DSTR_STATIC(base64_complete_oneline,
    "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4v"
    "MDEyMzQ1Njc4OTo7PD0+P0BBQkNERUZHSElKS0xNTk9QUVJTVFVWV1hZWltcXV5f"
    "YGFiY2RlZmdoaWprbG1ub3BxcnN0dXZ3eHl6e3x9fn+AgYKDhIWGh4iJiouMjY6P"
    "kJGSk5SVlpeYmZqbnJ2en6ChoqOkpaanqKmqq6ytrq+wsbKztLW2t7i5uru8vb6/"
    "wMHCw8TFxsfIycrLzM3Oz9DR0tPU1dbX2Nna29zd3t/g4eLj5OXm5+jp6uvs7e7v"
    "8PHy8/T19vf4+fr7/P3+/w==");

static derr_t test_b64_encoders(void){
    derr_t e = E_OK;
    // prep the test binary data
    DSTR_VAR(orig, 256);
    for(size_t i = 0; i < orig.size; i++){
        orig.data[i] = (char)i;
    }
    orig.len = 256;

    // prepare input to function
    DSTR_VAR(bin, 256);
    PROP(&e, dstr_copy(&orig, &bin) );

    // test bin2b64 without forcing the end output
    DSTR_VAR(encoded, 512);
    PROP(&e, bin2b64(&bin, &encoded, 64, false) );

    // check the values
    if(bin.len != 16 || (unsigned char)bin.data[0] != 240){
        ORIG(&e, E_VALUE, "bad partial encode\n");
    }
    int result = dstr_cmp(&encoded, &base64_partial);
    if(result != 0){
        ORIG(&e, E_VALUE, "bad partial encode\n");
    }

    // test bin2b64 with forcing the end output
    PROP(&e, bin2b64(&bin, &encoded, 64, true) );

    // check the values
    if(bin.len != 0){
        ORIG(&e, E_VALUE, "bad full encode\n");
    }
    result = dstr_cmp(&encoded, &base64_complete);
    if(result != 0){
        ORIG(&e, E_VALUE, "bad full encode\n");
    }

    // now decode the whole thing back to binary
    DSTR_VAR(decoded, 256);
    PROP(&e, b642bin(&encoded, &decoded) );

    result = dstr_cmp(&decoded, &orig);
    if(result != 0){
        ORIG(&e, E_VALUE, "bad decode\n");
    }

    // now do it again, this time one character at a time
    encoded.len = 0;
    decoded.len = 0;
    bin.len = 0;
    for(size_t i = 0; i < orig.len; i++){
        dstr_t sub = dstr_sub(&orig, i, i+1);
        PROP(&e, dstr_append(&bin, &sub) );
        PROP(&e, bin2b64(&bin, &encoded, 64, i + 1 == orig.len) );
    }
    result = dstr_cmp(&encoded, &base64_complete);
    if(result != 0){
        ORIG(&e, E_VALUE, "bad byte-by-byte encode\n");
    }

    DSTR_VAR(base64, 512);
    for(size_t i = 0; i < encoded.len; i++){
        dstr_t sub = dstr_sub(&encoded, i, i+1);
        PROP(&e, dstr_append(&base64, &sub) );
        PROP(&e, b642bin(&base64, &decoded) );
    }
    result = dstr_cmp(&decoded, &orig);
    if(result != 0){
        ORIG(&e, E_VALUE, "bad byte-by-byte decode\n");
    }

    // now one last time but on one line
    encoded.len = 0;
    PROP(&e, dstr_copy(&orig, &bin) );
    PROP(&e, bin2b64(&bin, &encoded, 0, true) );

    result = dstr_cmp(&encoded, &base64_complete_oneline);
    if(result != 0){
        ORIG(&e, E_VALUE, "bad oneline encode\n");
    }

    return e;
}

static derr_t test_hex_encoders(void){
    derr_t e = E_OK;
    DSTR_STATIC(bin, "hex is way more secure than ROT13");
    DSTR_STATIC(hex, "68657820697320776179206d6f7265207"
                     "36563757265207468616e20524f543133");
    DSTR_VAR(in, 256);
    DSTR_VAR(out, 256);

    // test encoder
    PROP(&e, dstr_copy(&bin, &in) );
    PROP(&e, bin2hex(&in, &out) );
    if(dstr_cmp(&out, &hex) != 0){
        TRACE(&e, "expected \"%x\"\n"
                  "but got  \"%x\"\n", FD(&hex), FD(&out));
        ORIG(&e, E_VALUE, "bin2hex() failed test");
    }
    out.len = 0;

    // test decoder
    PROP(&e, dstr_copy(&hex, &in) );
    PROP(&e, hex2bin(&in, &out) );
    if(dstr_cmp(&out, &bin) != 0){
        TRACE(&e, "expected \"%x\"\n"
                  "but got  \"%x\"\n", FD(&bin), FD(&out));
        ORIG(&e, E_VALUE, "hex2bin() failed test");
    }
    return e;
}

static derr_t test_crypto(void){
    derr_t e = E_OK;

    const char* keyfile = "_delete_me_if_you_see_me.pem";
    PROP(&e, gen_key(1024, keyfile) );

    // load the keys that are now written to a file
    keypair_t kp;
    PROP(&e, keypair_load(&kp, keyfile) );
    // delete the temporary file
    remove(keyfile);

    DSTR_VAR(pubkey_pem, 4096);
    PROP(&e, keypair_get_public_pem(&kp, &pubkey_pem) );
    LOG_DEBUG("PEM-formatted public key: %x", FD(&pubkey_pem));
    LOG_DEBUG("fingerprint (%x) %x\n", FU(kp.fingerprint.len), FD_DBG(&kp.fingerprint));

    // allocate an encrypter
    encrypter_t ec;
    PROP_GO(&e, encrypter_new(&ec), cleanup_1);

    DSTR_STATIC(plain, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234567890987654321ZYXWUTSRQPONMLKJIHGFEDCBAzyxwutsrqponmlkjihgfedcba");
    DSTR_VAR(in, 4096);
    dstr_copy(&plain, &in);
    DSTR_VAR(enc, 4096);
    DSTR_VAR(decr, 4096);
    LIST_VAR(dstr_t, fprs, 1);
    LIST_APPEND(dstr_t, &fprs, kp.fingerprint);
    PROP_GO(&e, encrypter_start(&ec, &kp.pair, 1, &fprs, &enc), cleanup_2);

    dstr_t sub;
    while(in.len){
        sub = dstr_sub(&in, 0, 1);
        PROP_GO(&e, encrypter_update(&ec, &sub, &enc), cleanup_2);
        dstr_leftshift(&in, 1);
    }

    PROP_GO(&e, encrypter_finish(&ec, &enc), cleanup_2);

    LOG_DEBUG("%x\n", FD(&enc));

    // ok, now we can try and decrypt

    // test that message authentication works by twiddling a byte
    //enc.data[7 * 64 + 2] = 'J';

    decrypter_t dc;
    PROP_GO(&e, decrypter_new(&dc), cleanup_2);

    PROP_GO(&e, decrypter_start(&dc, &kp, NULL, NULL), cleanup_3);

    //PROP_GO(&e, decrypter_update(&dc, &enc, &decr), cleanup_3);

    // decrypt "byte-by-byte"
    DSTR_VAR(bbb_in, 4096);
    for(size_t i = 0; i < enc.len; i++){
        sub = dstr_sub(&enc, i, i+1);
        PROP_GO(&e, dstr_append(&bbb_in, &sub), cleanup_3);
        PROP_GO(&e, decrypter_update(&dc, &bbb_in, &decr), cleanup_3);
    }

    PROP_GO(&e, decrypter_finish(&dc, &decr), cleanup_3);

    LOG_DEBUG("%x\n", FD_DBG(&decr));

    int result = dstr_cmp(&plain, &decr);
    if(result != 0){
        ORIG_GO(&e, E_VALUE, "bad decryption", cleanup_3);
    }

cleanup_3:
    decrypter_free(&dc);
cleanup_2:
    encrypter_free(&ec);
cleanup_1:
    keypair_free(&kp);
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    crypto_library_init();

    PROP_GO(&e, test_b64_encoders(), test_fail);
    PROP_GO(&e, test_hex_encoders(), test_fail);
    PROP_GO(&e, test_crypto(), test_fail);

    LOG_ERROR("PASS\n");
    crypto_library_close();
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    crypto_library_close();
    return 1;
}
