#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>

#ifdef BUILD_SERVER_CODE
#include <mysql.h>
#include "badbadbad_alert.h"
#endif // BUILD_SERVER_CODE

#include "common.h"
#include "logger.h"
#include "crypto.h"
#include "opt_parse.h"

// this should only have to be 10, but just in case...
#define MAX_ENCRYPTION_KEYS 32

// TODO: delete these.  the fixed-size fpr buffer in keypair_t breaks
//       LIST_APPEND, so LIST(keypair_t) is fundamentally dangerous
LIST_HEADERS(keypair_t)
LIST_FUNCTIONS(keypair_t)

static derr_t do_encryption(EVP_PKEY** pkeys, size_t nkeys, LIST(dstr_t)* fprs){
    derr_t error;

    // buffer for reading from stdin
    DSTR_VAR(in, 4096);

    /* buffer for writing to stdout.  Note that technically the output of
       encrypter_start is unbounded, but because a user can only have 10
       devices, 8129 is plenty big for the entire header of the message. */
    DSTR_VAR(out, 8192);

    encrypter_t enc;
    PROP( encrypter_new(&enc) );

    PROP_GO( encrypter_start(&enc, pkeys, nkeys, fprs, &out), cleanup_enc);

    while(true){
        // write the encrypted buffer to stdout
        PROP_GO( dstr_write(1, &out), cleanup_enc);
        out.len = 0;

        // read plaintext from stdin
        size_t amnt_read;
        PROP_GO( dstr_read(0, &in, 0, &amnt_read), cleanup_enc);
        if(amnt_read == 0){
            break;
        }

        // encrypt what we read
        PROP_GO( encrypter_update(&enc, &in, &out), cleanup_enc);
    }

    // finish the encryption
    PROP_GO( encrypter_finish(&enc, &out), cleanup_enc);

    // write remainder to stdout
    PROP_GO( dstr_write(1, &out), cleanup_enc);
    out.len = 0;

cleanup_enc:
    encrypter_free(&enc);
    return error;
}

static derr_t cli_encrypt(int argc, char** argv){
    derr_t error;
    // load keys from filenames on the command line
    LIST_VAR(keypair_t, keys, MAX_ENCRYPTION_KEYS);
    for(int i = 1; i < argc; i++){
        keypair_t key;
        PROP_GO( keypair_load(&key, argv[i]), cleanup_keys);
        error = LIST_APPEND(keypair_t, &keys, key);
        if(error){
            keypair_free(&key);
            PROP_GO(error, cleanup_keys);
        }
        // fix the now-broken pointer in keys.data[i].fingerprint
        // wow this is ugly that this is necessary
        keys.data[i-1].fingerprint.data = keys.data[i-1].fingerprint_buffer;
        // this is for debug
        DSTR_VAR(hex, 256);
        bin2hex(&key.fingerprint, &hex);
        LOG_DEBUG("%x : %x\n", FS(argv[i]), FD(&hex));
        DSTR_VAR(pemout, 4096);
        PROP( keypair_get_public_pem(&keys.data[i-1], &pemout) );
        LOG_DEBUG("%x\n", FD(&pemout));
    }

    // rearrange the EVP_PKEYs into an array
    EVP_PKEY* pkeys[MAX_ENCRYPTION_KEYS];
    for(size_t i = 0; i < keys.len; i++){
        pkeys[i] = keys.data[i].pair;
    }
    // rearrange the fingerprints into an array as well
    LIST_VAR(dstr_t, fprs, MAX_ENCRYPTION_KEYS);
    for(size_t i = 0; i < keys.len; i++){
        LIST_APPEND(dstr_t, &fprs, keys.data[i].fingerprint);
    }

    // ready to start encrypting
    PROP_GO( do_encryption(pkeys, keys.len, &fprs), cleanup_keys);

cleanup_keys:
    for(size_t i = 0; i < keys.len; i++){
        keypair_free(&keys.data[i]);
    }
    return error;
}

#ifdef BUILD_SERVER_CODE
static void log_ssl_errors(void){
    unsigned long e;
    while( (e = ERR_get_error()) ){
        char buffer[256];
        ERR_error_string_n(e, buffer, sizeof(buffer));
        LOG_ERROR("OpenSSL error: %x\n", FS(buffer));
    }
}

static void log_sql_error(MYSQL* sql){
    if(*mysql_error(sql)){
        LOG_ERROR("%x\n", FS(mysql_error(sql)));
    }
}

static derr_t get_user_id(MYSQL* sql, const char* email, unsigned int* uid){
    derr_t error = E_OK;
    // build request
    DSTR_VAR(req, 256);
    PROP( FMT(&req, "select user_id from accounts where email = '%x'", FS(email)) );
    LOG_DEBUG("mysql request to fetch user_id:\n    %x\n", FD(&req));

    // do request
    int ret = mysql_real_query(sql, req.data, req.len);
    if(ret != 0){
        log_sql_error(sql);
        ORIG(E_SQL, "mysql_real_query failed");
    }

    // get result
    MYSQL_RES* res = mysql_use_result(sql);
    if(!res){
        log_sql_error(sql);
        ORIG(E_SQL, "mysql_use_result failed");
    }

    // make sure that we get at least one result
    bool found_uid = false;

    // loop through results
    MYSQL_ROW row;
    while( (row = mysql_fetch_row(res)) ){
        found_uid = true;
        // get length of uid
        unsigned long *lens = mysql_fetch_lengths(res);
        // wrap uid in a dstr
        dstr_t uidstr;
        DSTR_WRAP(uidstr, row[0], lens[0], 0);
        // count leading zeros
        size_t zeros = 0;
        for(size_t i = 0 ; i < uidstr.len; i++){
            if(uidstr.data[i] == '0'){
                zeros++;
            }else{
                break;
            }
        }
        // get no-leading zeros substring of uidstr
        dstr_t nlz = dstr_sub(&uidstr, zeros, 0);
        // convert to number
        PROP_GO( dstr_tou(&nlz, uid), cleanup_res);
    }
    // make sure we exited the loop without an error
    if(*mysql_error(sql)){
        log_sql_error(sql);
        ORIG_GO(E_SQL, "error fetching rows", cleanup_res);
    }

    if(!found_uid){
        ORIG(E_INTERNAL, "no such user");
    }

cleanup_res:
    mysql_free_result(res);
    return error;
}

static derr_t pem_to_pkey(const char* pemkey, size_t len, EVP_PKEY** pkey){
    derr_t error = E_OK;
    // try to allocate for the EVP_PKEY
    *pkey = EVP_PKEY_new();
    if(!*pkey){
        log_ssl_errors();
        ORIG(E_SSL, "EVP_PKEY_new failed");
    }

    // wrap the pem-encoded key in an SSL memory BIO
    if(len > INT_MAX)
        ORIG(E_INTERNAL, "pem key is way too long");
    BIO* pembio = BIO_new_mem_buf((const void*)pemkey, (int)len);
    if(!pembio){
        log_ssl_errors();
        ORIG_GO(E_SSL, "unable to create BIO", cleanup_1);
    }

    // read the public key from the BIO (no password protection)
    EVP_PKEY* temp;
    temp = PEM_read_bio_PUBKEY(pembio, pkey, NULL, NULL);
    if(!temp){
        log_ssl_errors();
        ORIG_GO(E_SSL, "failed to read public key", cleanup_2);
    }

cleanup_2:
    BIO_free(pembio);
cleanup_1:
    if(error) EVP_PKEY_free(*pkey);
    return error;
}

static derr_t get_keys(MYSQL* sql, unsigned int uid, LIST(keypair_t)* keys){
    derr_t error = E_OK;
    // build request
    DSTR_VAR(req, 256);
    PROP( FMT(&req, "select fingerprint, public_key from devices "
                    "where user_id = %x", FU(uid)) );
    LOG_DEBUG("mysql request to fetch fingerprints:\n    %x\n", FD(&req));

    // do request
    int ret = mysql_real_query(sql, req.data, req.len);
    if(ret != 0){
        log_sql_error(sql);
        ORIG(E_SQL, "mysql_real_query failed");
    }

    // get result
    MYSQL_RES* res = mysql_use_result(sql);
    if(!res){
        log_sql_error(sql);
        ORIG(E_SSL, "mysql_use_result failed");
    }

    // loop through results
    LOG_DEBUG("mysql fingerprint results:\n");
    keys->len = 0;
    MYSQL_ROW row;
    while( (row = mysql_fetch_row(res)) ){
        // initialize a keypair
        keypair_t kp;
        DSTR_WRAP_ARRAY(kp.fingerprint, kp.fingerprint_buffer);

        // get lengths of fields
        unsigned long *lens = mysql_fetch_lengths(res);

        // copy the fingerprint to the keypair
        dstr_t hexfpr;
        DSTR_WRAP(hexfpr, row[0], lens[0], 0);
        LOG_DEBUG("    %x\n", FD(&hexfpr));
        PROP_GO( hex2bin(&hexfpr, &kp.fingerprint), loop_end);

        // read the public key
        PROP_GO( pem_to_pkey(row[1], lens[1], &kp.pair), loop_end);

        // try to append the public key to the list
        error = LIST_APPEND(keypair_t, keys, kp);
        if(error){
            keypair_free(&kp);
            PROP_GO(error, loop_end);
        }
        // fix the now-broken pointers in keys.data[-1].fingerprint
        // wow this is ugly that this is necessary
        keys->data[keys->len - 1].fingerprint.data =
                                keys->data[keys->len - 1].fingerprint_buffer;
    }
loop_end:
    // if we had an error, we still need to read all the results
    while( (row = mysql_fetch_row(res)) ) {}
    // check if we exited the loop with an error
    if(*mysql_error(sql)){
        log_sql_error(sql);
        ORIG_GO(E_SQL, "error fetching rows", cleanup_res);
    }
    // also cleanup if we ran into other errors in the loop
    PROP_GO(error, fail);

cleanup_res:
    mysql_free_result(res);
    return error;

fail:
    for(size_t i = 0; i < keys->len; i++){
        keypair_free(&keys->data[i]);
    }
    mysql_free_result(res);
    return error;
}

static derr_t mysql_encrypt(void){
    derr_t error;
    // get USER variable (that is, to whom we are encrypting)
    char* user = getenv("USER");

    // load the mysql config from a file
    DSTR_VAR(conf_text, 256);
    PROP( dstr_fread_file("/etc/encrypt_msg/encrypt_msg.conf", &conf_text) );
    // parse the file
    LIST_VAR(dstr_t, conf, 4);
    DSTR_STATIC(pattern, "\n");
    PROP( dstr_split(&conf_text, &pattern, &conf) );
    // if there is a newline at the end of the file, ignore it
    if(conf.len == 4 && conf.data[3].len == 0) conf.len--;
    if(conf.len != 3){
        ORIG(E_INTERNAL, "config file must contain exactly the mysql host, "
                         "user, and password, each on a separate line");
    }
    // get null-terminated values, for mysql api
    DSTR_VAR(sqlhost, 256);
    DSTR_VAR(sqluser, 256);
    DSTR_VAR(sqlpass, 256);
    PROP( dstr_copy(&conf.data[0], &sqlhost) );
    PROP( dstr_copy(&conf.data[1], &sqluser) );
    PROP( dstr_copy(&conf.data[2], &sqlpass) );
    PROP( dstr_null_terminate(&sqlhost) );
    PROP( dstr_null_terminate(&sqluser) );
    PROP( dstr_null_terminate(&sqlpass) );

    // init mysql
    int ret = mysql_library_init(0, NULL, NULL);
    if(ret != 0){
        ORIG(E_SQL, "unable to init mysql library");
    }

    // get a mysql object
    MYSQL sql;
    MYSQL* mret = mysql_init(&sql);
    if(!mret){
        ORIG_GO(E_SQL, "unable to init mysql object", cleanup_sql);
    }

    // make a connection with mysqld
    mret = mysql_real_connect(&sql, sqlhost.data, sqluser.data, sqlpass.data,
                "splintermail", 0, "/var/run/mysqld/mysqld.sock", 0);
    if(!mret){
        log_sql_error(&sql);
        ORIG_GO(E_SQL, "unable to connect to mysqld", cleanup_sql_obj);
    }

    // get the user id
    unsigned int uid;
    PROP_GO( get_user_id(&sql, user, &uid), cleanup_sql_obj);

    // use the user_id to get the encryption keys
    LIST_VAR(keypair_t, keys, MAX_ENCRYPTION_KEYS);
    PROP_GO( get_keys(&sql, uid, &keys), cleanup_sql_obj);

    if(keys.len == 0){
        LOG_ERROR("no keys for user %x\n", FS(user));
        ORIG_GO(E_NOKEYS, "No keys for user", cleanup_keys);
    }

    // rearrange the EVP_PKEYs into an array
    EVP_PKEY* pkeys[MAX_ENCRYPTION_KEYS];
    for(size_t i = 0; i < keys.len; i++){
        pkeys[i] = keys.data[i].pair;
    }
    // rearrange the fingerprints into an array as well
    LIST_VAR(dstr_t, fprs, MAX_ENCRYPTION_KEYS);
    for(size_t i = 0; i < keys.len; i++){
        LIST_APPEND(dstr_t, &fprs, keys.data[i].fingerprint);
    }


    // finally ready to start encrypting

    PROP_GO( do_encryption(pkeys, keys.len, &fprs), cleanup_keys);

cleanup_keys:
    for(size_t i = 0; i < keys.len; i++){
        keypair_free(&keys.data[i]);
    }
cleanup_sql_obj:
    mysql_close(&sql);
cleanup_sql:
    mysql_library_end();
    return error;
}

#endif

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

#ifdef BUILD_SERVER_CODE
    // open log file
    logger_add_filename(LOG_LVL_INFO, "/var/log/encrypt_msg/log");
#endif

    // init SSL
    PROP_GO( crypto_library_init(), exit);

    if(newargc == 1){
#ifdef BUILD_SERVER_CODE
        PROP_GO( mysql_encrypt(), cleanup_ssl);
#else
        ORIG_GO(E_PARAM, "compiled without mysql support, please provide "
                         "encryption key files as command line arguments",
                cleanup_ssl);
#endif
    } else {
        PROP_GO( cli_encrypt(argc, argv), cleanup_ssl);
    }

cleanup_ssl:
    crypto_library_close();
exit:
#ifdef BUILD_SERVER_CODE
    // any error at all (except for a user having no keys) is badbadbad
    CATCH(E_ANY ^ E_NOKEYS){
        DSTR_STATIC(summary, "unexpected error in encrypt_message, check log");
        badbadbad_alert(&summary, NULL);
        // put a line break in log file for ease of reading
        LOG_ERROR("\n");
    }
#endif
    return error != E_OK;
}
