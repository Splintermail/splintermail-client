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

#include <libdstr/libdstr.h>
#include <libcrypto/libcrypto.h>

#include "test_utils.h"


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
    PROP(&e, bin2b64_stream(&bin, &encoded, 64, false) );

    // check the values
    if(bin.len != 16 || (unsigned char)bin.data[0] != 240){
        ORIG(&e, E_VALUE, "bad partial encode\n");
    }
    int result = dstr_cmp(&encoded, &base64_partial);
    if(result != 0){
        ORIG(&e, E_VALUE, "bad partial encode\n");
    }

    // test bin2b64 with forcing the end output
    PROP(&e, bin2b64_stream(&bin, &encoded, 64, true) );

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
    PROP(&e, b642bin_stream(&encoded, &decoded) );

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
        PROP(&e, bin2b64_stream(&bin, &encoded, 64, i + 1 == orig.len) );
    }
    result = dstr_cmp(&encoded, &base64_complete);
    if(result != 0){
        ORIG(&e, E_VALUE, "bad byte-by-byte encode\n");
    }

    DSTR_VAR(base64, 512);
    for(size_t i = 0; i < encoded.len; i++){
        dstr_t sub = dstr_sub(&encoded, i, i+1);
        PROP(&e, dstr_append(&base64, &sub) );
        PROP(&e, b642bin_stream(&base64, &decoded) );
    }
    result = dstr_cmp(&decoded, &orig);
    if(result != 0){
        ORIG(&e, E_VALUE, "bad byte-by-byte decode\n");
    }

    // now one last time but on one line
    encoded.len = 0;
    PROP(&e, dstr_copy(&orig, &bin) );
    PROP(&e, bin2b64_stream(&bin, &encoded, 0, true) );

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
    PROP_GO(&e, gen_key(1024, keyfile), cu_file);

    // load the keys that are now written to a file
    keypair_t *kp;
    PROP_GO(&e, keypair_load(&kp, keyfile), cu_file);
    remove(keyfile);

    DSTR_VAR(pubkey_pem, 4096);
    PROP(&e, keypair_get_public_pem(kp, &pubkey_pem) );
    LOG_DEBUG("PEM-formatted public key: %x", FD(&pubkey_pem));
    LOG_DEBUG("fingerprint (%x) %x\n",
            FU(kp->fingerprint->len), FD_DBG(kp->fingerprint));

    // allocate an encrypter
    encrypter_t ec;
    PROP_GO(&e, encrypter_new(&ec), cleanup_1);

    DSTR_STATIC(plain,
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
        "0987654321ZYXWUTSRQPONMLKJIHGFEDCBAzyxwutsrqponmlkjihgfedcba");
    DSTR_VAR(in, 4096);
    dstr_copy(&plain, &in);
    DSTR_VAR(enc, 4096);
    DSTR_VAR(decr, 4096);
    link_t keys;
    link_init(&keys);
    link_list_append(&keys, &kp->link);
    PROP_GO(&e, encrypter_start(&ec, &keys, &enc), cleanup_2);

    dstr_t sub;
    while(in.len){
        sub = dstr_sub(&in, 0, 1);
        PROP_GO(&e, encrypter_update_stream(&ec, &sub, &enc), cleanup_2);
        dstr_leftshift(&in, 1);
    }

    PROP_GO(&e, encrypter_finish(&ec, &enc), cleanup_2);

    LOG_DEBUG("%x\n", FD(&enc));

    // ok, now we can try and decrypt

    // test that message authentication works by twiddling a byte
    //enc.data[7 * 64 + 2] = 'J';

    decrypter_t dc;
    PROP_GO(&e, decrypter_new(&dc), cleanup_2);

    PROP_GO(&e, decrypter_start(&dc, kp, NULL, NULL), cleanup_3);

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
cu_file:
    // delete the temporary file
    remove(keyfile);
    return e;
}

static derr_t test_keypair(void){
    derr_t e = E_OK;

    const char* keyfile = "_delete_me_if_you_see_me.pem";
    PROP_GO(&e, gen_key(1024, keyfile), cu_file);

    keypair_t *kp;
    PROP_GO(&e, keypair_load(&kp, keyfile), cu_file);
    remove(keyfile);

    // read the public key as PEM text
    DSTR_VAR(pem1, 4096);
    PROP_GO(&e, keypair_get_public_pem(kp, &pem1), cu_kp);

    // duplicate the key
    keypair_t *copy;
    PROP_GO(&e, keypair_copy(kp, &copy), cu_kp);

    // delete the original
    keypair_free(&kp);

    // verify the memory is still there
    DSTR_VAR(pem2, 4096);
    PROP_GO(&e, keypair_get_public_pem(copy, &pem2), cu_copy);

    if(dstr_cmp(&pem1, &pem2) != 0){
        TRACE(&e, "PEM1:\n%xPEM2:\n%x", FD(&pem1), FD(&pem2));
        ORIG_GO(&e, E_VALUE, "pems do not match", cu_copy);
    }

cu_copy:
    keypair_free(&copy);
cu_kp:
    keypair_free(&kp);
cu_file:
    // delete the temporary file
    remove(keyfile);
    return e;
}

typedef struct {
    key_listener_i l1;
    bool l1_add;
    bool l1_del;

    key_listener_i l2;
    bool l2_add;
    bool l2_del;
} test_keyshare_vars_t;
DEF_CONTAINER_OF(test_keyshare_vars_t, l1, key_listener_i);
DEF_CONTAINER_OF(test_keyshare_vars_t, l2, key_listener_i);

static void l1_add(key_listener_i *l1, keypair_t *kp){
   test_keyshare_vars_t *vars = CONTAINER_OF(l1, test_keyshare_vars_t, l1);
   // we don't actually need the keypair
   keypair_free(&kp);
   vars->l1_add = true;
}

static void l1_del(key_listener_i *l1, const dstr_t *fingerprint){
   test_keyshare_vars_t *vars = CONTAINER_OF(l1, test_keyshare_vars_t, l1);
   (void)fingerprint;
   vars->l1_del = true;
}

static void l2_add(key_listener_i *l2, keypair_t *kp){
   test_keyshare_vars_t *vars = CONTAINER_OF(l2, test_keyshare_vars_t, l2);
   // we don't actually need the keypair
   keypair_free(&kp);
   vars->l2_add = true;
}

static void l2_del(key_listener_i *l2, const dstr_t *fingerprint){
   test_keyshare_vars_t *vars = CONTAINER_OF(l2, test_keyshare_vars_t, l2);
   (void)fingerprint;
   vars->l2_del = true;
}

static derr_t test_keyshare(void){
    derr_t e = E_OK;

    // prepare for callbacks
    test_keyshare_vars_t vars = {
        .l1 = {
            .add = l1_add,
            .del = l1_del,
        },
        .l2 = {
            .add = l2_add,
            .del = l2_del,
        },
    };
    link_init(&vars.l1.link);
    link_init(&vars.l2.link);

    keypair_t *kp1, *kp2;

    const char* keyfile = "_delete_me_if_you_see_me.pem";

    // build a couple random keys
    PROP_GO(&e, gen_key(1024, keyfile), cu_file);
    PROP_GO(&e, keypair_load(&kp1, keyfile), cu_file);
    remove(keyfile);

    PROP_GO(&e, gen_key(1024, keyfile), cu_kp1);
    PROP_GO(&e, keypair_load(&kp2, keyfile), cu_kp1);
    remove(keyfile);

    // create a keyshare
    keyshare_t keyshare;
    PROP_GO(&e, keyshare_init(&keyshare), cu_kp2);

    // register l1 with the keyshare
    link_t keys;
    link_init(&keys);
    PROP_GO(&e, keyshare_register(&keyshare, &vars.l1, &keys), cu_keyshare);

    if(!link_list_isempty(&keys))
        ORIG_GO(&e, E_VALUE, "initial l1 key list not empty", cu_keys);

    // add a key to the keyshare
    PROP_GO(&e, keyshare_add_key(&keyshare, kp1), cu_keys);
    if(!vars.l1_add)
        ORIG_GO(&e, E_VALUE, "l1_add not called", cu_keys);
    vars.l1_add = false;

    // register l2 with the keyshare
    PROP_GO(&e, keyshare_register(&keyshare, &vars.l2, &keys), cu_keys);

    if(link_list_isempty(&keys))
        ORIG_GO(&e, E_VALUE, "initial l2 key list is empty", cu_keys);

    // add a key to the keyshare
    PROP_GO(&e, keyshare_add_key(&keyshare, kp2), cu_keys);
    if(!vars.l1_add)
        ORIG_GO(&e, E_VALUE, "l1_add not called", cu_keys);
    if(!vars.l2_add)
        ORIG_GO(&e, E_VALUE, "l2_add not called", cu_keys);
    vars.l1_add = false;
    vars.l2_add = false;

    // del a key from the keyshare
    keyshare_del_key(&keyshare, kp1->fingerprint);
    if(!vars.l1_del)
        ORIG_GO(&e, E_VALUE, "l1_del not called", cu_keys);
    if(!vars.l2_del)
        ORIG_GO(&e, E_VALUE, "l2_del not called", cu_keys);
    vars.l1_del = false;
    vars.l2_del = false;

    // unregister l1
    keyshare_unregister(&keyshare, &vars.l1);

    // del a key from the keyshare
    keyshare_del_key(&keyshare, kp1->fingerprint);
    if(vars.l1_del)
        ORIG_GO(&e, E_VALUE, "l1_del called after unregister", cu_keys);
    if(!vars.l2_del)
        ORIG_GO(&e, E_VALUE, "l2_del not called", cu_keys);
    vars.l2_del = false;

    // unregister l2
    keyshare_unregister(&keyshare, &vars.l2);

    link_t *link;
cu_keys:
    while((link = link_list_pop_first(&keys))){
        keypair_t *kp = CONTAINER_OF(link, keypair_t, link);
        keypair_free(&kp);
    }
cu_keyshare:
    keyshare_free(&keyshare);
cu_kp2:
    keypair_free(&kp1);
cu_kp1:
    keypair_free(&kp2);
    // delete the temporary file
cu_file:
    remove(keyfile);
    return e;
}

static derr_t test_zeroized(void){
    decrypter_t dc = {0};
    encrypter_t ec = {0};

    derr_t e = E_OK;

    decrypter_free(&dc);
    encrypter_free(&ec);

    PROP_GO(&e, decrypter_new(&dc), cu);
    decrypter_free(&dc);

    PROP_GO(&e, encrypter_new(&ec), cu);
    encrypter_free(&ec);

cu:
    decrypter_free(&dc);
    encrypter_free(&ec);
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
    PROP_GO(&e, test_keypair(), test_fail);
    PROP_GO(&e, test_keyshare(), test_fail);
    PROP_GO(&e, test_zeroized(), test_fail);

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
