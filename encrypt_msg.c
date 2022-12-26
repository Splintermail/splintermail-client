#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>

#ifdef BUILD_SERVER_CODE
#include "server/mysql_util/mysql_util.h"
#include "server/libsmsql.h"
#include "server/badbadbad_alert.h"
#endif // BUILD_SERVER_CODE

#include "libdstr/libdstr.h"
#include "libcrypto/libcrypto.h"
#include "fixed_lengths.h"

// this should only have to be 10, but just in case...
#define MAX_ENCRYPTION_KEYS 32

static derr_t do_encryption(link_t *keys){
    derr_t e = E_OK;

    // buffer for reading from stdin
    DSTR_VAR(in, 4096);

    /* buffer for writing to stdout.  Note that technically the output of
       encrypter_start is unbounded, but because a user can only have 10
       devices, 8129 is plenty big for the entire header of the message. */
    DSTR_VAR(out, 8192);

    encrypter_t enc;
    PROP(&e, encrypter_new(&enc) );

    PROP_GO(&e, encrypter_start(&enc, keys, &out), cleanup_enc);

    while(true){
        // write the encrypted buffer to stdout
        PROP_GO(&e, dstr_write(1, &out), cleanup_enc);
        out.len = 0;

        // read plaintext from stdin
        size_t amnt_read;
        PROP_GO(&e, dstr_read(0, &in, 0, &amnt_read), cleanup_enc);
        if(amnt_read == 0){
            break;
        }

        // encrypt what we read
        PROP_GO(&e, encrypter_update_stream(&enc, &in, &out), cleanup_enc);
    }

    // finish the encryption
    PROP_GO(&e, encrypter_finish(&enc, &out), cleanup_enc);

    // write remainder to stdout
    PROP_GO(&e, dstr_write(1, &out), cleanup_enc);
    out.len = 0;

cleanup_enc:
    encrypter_free(&enc);
    return e;
}

static derr_t cli_encrypt(int argc, char** argv){
    derr_t e = E_OK;
    link_t keys;
    link_init(&keys);
    for(int i = 1; i < argc; i++){
        keypair_t *kp;
        PROP_GO(&e, keypair_load_public(&kp, argv[i]), cu_keys);
        link_list_append(&keys, &kp->link);
        // this is for debug
        DSTR_VAR(hex, 256);
        bin2hex(kp->fingerprint, &hex);
        LOG_DEBUG("%x : %x\n", FS(argv[i]), FD(&hex));
        DSTR_VAR(pemout, 4096);
        PROP(&e, keypair_get_public_pem(kp, &pemout) );
        LOG_DEBUG("%x\n", FD(&pemout));
    }

    // ready to start encrypting
    PROP_GO(&e, do_encryption(&keys), cu_keys);

    link_t *link;
cu_keys:
    while((link = link_list_pop_first(&keys))){
        keypair_t *kp = CONTAINER_OF(link, keypair_t, link);
        keypair_free(&kp);
    }
    return e;
}

#ifdef BUILD_SERVER_CODE

// just stream stdin to stdout
/* this is so that encrypt_msg is a well-behaved sieve filter in the case that
   a user has no keys for their account */
static derr_t do_copy(void){
    derr_t e = E_OK;

    DSTR_VAR(buf, 4096);

    while(true){
        size_t amnt_read;
        PROP(&e, dstr_read(0, &buf, 0, &amnt_read) );
        if(amnt_read == 0){
            break;
        }

        // write the encrypted buffer to stdout
        PROP(&e, dstr_write(1, &buf) );
        buf.len = 0;
    }

    return e;
}

static derr_t _mysql_encrypt(MYSQL *sql, const dstr_t user){
    derr_t e = E_OK;

    dstr_t fsid;
    dstr_t domain;
    dstr_split2_soft(user, DSTR_LIT("@"), NULL, &fsid, &domain);

    if(dstr_cmp(&domain, &DSTR_LIT("x.splintermail.com")) != 0){
        TRACE(&e, "user=%x, domain=%x\n", FD(&user), FD(&domain));
        ORIG(&e, E_INTERNAL, "unknown domain");
    }

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP(&e, to_uuid(&fsid, &uuid) );

    link_t pems;
    link_init(&pems);
    link_t keys;
    link_init(&keys);
    link_t *link;

    // returns a list of pem-encoded public keys (smsql_dstr_t's)
    PROP(&e, list_device_keys(sql, &uuid, &pems) );

    // convert each pem-encoded key into a keypair_t
    while(!link_list_isempty(&pems)){
        smsql_dstr_t *pem = CONTAINER_OF(pems.next, smsql_dstr_t, link);

        // read this key from its pem text
        keypair_t *kp;
        PROP_GO(&e, keypair_from_pubkey_pem(&kp, pem->dstr), cu);

        link_list_append(&keys, &kp->link);

        // free consumed pem key
        link_list_pop_first(&pems);
        smsql_dstr_free(&pem);
    }

    if(link_list_isempty(&keys)){
        // if there are no keys, just copy stdin to stdout
        PROP_GO(&e, do_copy(), cu);
    }else{
        PROP_GO(&e, do_encryption(&keys), cu);
    }

cu:
    while((link = link_list_pop_first(&pems))){
        smsql_dstr_t *pem = CONTAINER_OF(link, smsql_dstr_t, link);
        smsql_dstr_free(&pem);
    }

    while((link = link_list_pop_first(&keys))){
        keypair_t *kp = CONTAINER_OF(link, keypair_t, link);
        keypair_free(&kp);
    }

    return e;
}

static derr_t mysql_encrypt(const dstr_t *sock){
    derr_t e = E_OK;

    // get USER variable (that is, to whom we are encrypting)
    bool ok;
    const dstr_t user = dgetenv(DSTR_LIT("USER"), &ok);
    if(!ok){
        ORIG(&e, E_INTERNAL, "no USER set");
    }

    // get sql server ready
    int ret = mysql_library_init(0, NULL, NULL);
    if(ret != 0){
        ORIG(&e, E_SQL, "unable to init mysql library");
    }

    MYSQL sql;
    MYSQL* mret = mysql_init(&sql);
    if(!mret){
        ORIG_GO(&e, E_SQL, "unable to init mysql object", cu_sql_lib);
    }

    PROP_GO(&e, sql_connect_unix(&sql, NULL, NULL, sock), cu_sql);

    PROP_GO(&e, _mysql_encrypt(&sql, user), cu_sql);

cu_sql:
    mysql_close(&sql);

cu_sql_lib:
    mysql_library_end();

    return e;
}

#endif

int main(int argc, char** argv){
    // specify command line options
    opt_spec_t o_debug = {'d', "debug", false};
    opt_spec_t o_help = {'h', "help", false};
#ifdef BUILD_SERVER_CODE
    opt_spec_t o_sock = {'s', "socket", true};
    opt_spec_t* spec[] = {&o_debug, &o_help, &o_sock};
#else
    opt_spec_t* spec[] = {&o_debug, &o_help};
#endif
    size_t speclen = sizeof(spec) / sizeof(*spec);
    int newargc;
    // parse command line options
    derr_t e = opt_parse(argc, argv, spec, speclen, &newargc);
    if(is_error(e)){
        DROP_VAR(&e);
        return 2;
    }

    // print help?
    if(o_help.found){
        printf("encrypt_msg: apply splintermail encryption to stdin\n");
        printf("usage: encrypt_msg KEY_FILE [...]\n");
#ifdef BUILD_SERVER_CODE
        printf("usage: USER=email encrypt_msg [--socket SOCK]\n");
#endif
        exit(0);
    }

    logger_add_fileptr(o_debug.found ? LOG_LVL_DEBUG : LOG_LVL_INFO, stderr);


#ifdef BUILD_SERVER_CODE
    // open log file
    logger_add_filename(LOG_LVL_INFO, "/var/log/encrypt_msg/log");
#endif

    // init SSL
    PROP_GO(&e, crypto_library_init(), exit);

    if(newargc == 1){
#ifdef BUILD_SERVER_CODE
        dstr_t *sock = &DSTR_LIT("/var/run/mysqld/mysqld.sock");
        if(o_sock.found){
            sock = &o_sock.val;
        }
        PROP_GO(&e, mysql_encrypt(sock), cleanup_ssl);
#else
        ORIG_GO(&e, E_PARAM, "compiled without mysql support, please provide "
                         "encryption key files as command line arguments",
                cleanup_ssl);
#endif
    } else {
        PROP_GO(&e, cli_encrypt(argc, argv), cleanup_ssl);
    }

cleanup_ssl:
    crypto_library_close();
exit:
#ifdef BUILD_SERVER_CODE
    // any error at all is badbadbad
    CATCH(e, E_ANY){
        // write errors to logfile
        DUMP(e);
        DSTR_STATIC(summary, "error in encrypt_message");
        // badbadbad_alert will print to the log file directly
        badbadbad_alert(&summary, &e.msg);
        // put a line break in log file for ease of reading
        LOG_ERROR("\n");
    }
#else
    // the non-server case; report all errors
    DUMP(e);
#endif
    int exitval = is_error(e);
    DROP_VAR(&e);
    return exitval;
}
