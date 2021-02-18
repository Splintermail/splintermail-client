#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>

#ifdef BUILD_SERVER_CODE
#include "server/mysql_util.h"
#include "server/badbadbad_alert.h"
#endif // BUILD_SERVER_CODE

#include "libdstr/libdstr.h"
#include "libcrypto/libcrypto.h"
#include "fixed_lengths.h"

// this should only have to be 10, but just in case...
#define MAX_ENCRYPTION_KEYS 32

// user has no keys (non-critical error)
derr_type_t E_NOKEYS;
REGISTER_ERROR_TYPE(E_NOKEYS, "NOKEYS");

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
        PROP_GO(&e, encrypter_update(&enc, &in, &out), cleanup_enc);
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
        PROP_GO(&e, keypair_load(&kp, argv[i]), cu_keys);
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

static void trace_sql_error(derr_t *e, MYSQL* sql){
    if(*mysql_error(sql)){
        TRACE(e, "%x\n", FS(mysql_error(sql)));
    }
}

static derr_t get_user_id(MYSQL* sql, const char* email, unsigned int* uid){
    derr_t e = E_OK;
    // build request
    DSTR_VAR(req, 256);
    PROP(&e, FMT(&req, "select user_id from accounts where email = '%x'", FS(email)) );
    LOG_DEBUG("mysql request to fetch user_id:\n    %x\n", FD(&req));

    // do request
    int ret = mysql_real_query(sql, req.data, req.len);
    if(ret != 0){
        trace_sql_error(&e, sql);
        ORIG(&e, E_SQL, "mysql_real_query failed");
    }

    // get result
    MYSQL_RES* res = mysql_use_result(sql);
    if(!res){
        trace_sql_error(&e, sql);
        ORIG(&e, E_SQL, "mysql_use_result failed");
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
        // convert to number
        PROP_GO(&e, dstr_tou(&uidstr, uid, 10), cleanup_res);
    }
    // make sure we exited the loop without an error
    if(*mysql_error(sql)){
        trace_sql_error(&e, sql);
        ORIG_GO(&e, E_SQL, "error fetching rows", cleanup_res);
    }

    if(!found_uid){
        ORIG_GO(&e, E_INTERNAL, "no such user", cleanup_res);
    }

cleanup_res:
    mysql_free_result(res);
    return e;
}

static derr_t get_keys(MYSQL* sql, unsigned int uid, link_t* keys){
    derr_t e = E_OK;
    // build request
    DSTR_VAR(req, 256);
    PROP(&e, FMT(&req, "select fingerprint, public_key from devices "
                    "where user_id = %x", FU(uid)) );
    LOG_DEBUG("mysql request to fetch fingerprints:\n    %x\n", FD(&req));

    // do request
    int ret = mysql_real_query(sql, req.data, req.len);
    if(ret != 0){
        trace_sql_error(&e, sql);
        ORIG(&e, E_SQL, "mysql_real_query failed");
    }

    // get result
    MYSQL_RES* res = mysql_use_result(sql);
    if(!res){
        trace_sql_error(&e, sql);
        ORIG(&e, E_SSL, "mysql_use_result failed");
    }

    // loop through results
    LOG_DEBUG("mysql fingerprint results:\n");
    MYSQL_ROW row;
    while( (row = mysql_fetch_row(res)) ){
        // get lengths of fields
        unsigned long *lens = mysql_fetch_lengths(res);

        // create a keypair from the public_key field
        dstr_t pem;
        DSTR_WRAP(pem, row[1], lens[1], 0);
        keypair_t *kp;
        PROP_GO(&e, keypair_from_pem(&kp, &pem), loop_end);

        // append key to output list
        link_list_append(keys, &kp->link);

        // verify the fingerprint matches
        dstr_t hexfpr;
        DSTR_WRAP(hexfpr, row[0], lens[0], 0);
        LOG_DEBUG("    %x\n", FD(&hexfpr));
        DSTR_VAR(fpr, FL_FINGERPRINT);
        PROP_GO(&e, hex2bin(&hexfpr, &fpr), loop_end);

        if(dstr_cmp(&fpr, kp->fingerprint) != 0){
            ORIG_GO(&e, E_INTERNAL,
                    "mismatched db vs calculated fingerprint", loop_end);
        }
    }
loop_end:
    // if we had an error, we still need to read all the results
    while( (row = mysql_fetch_row(res)) ) {}
    // check if we exited the loop with an error
    if(*mysql_error(sql)){
        trace_sql_error(&e, sql);
        ORIG_GO(&e, E_SQL, "error fetching rows", cleanup_res);
    }
    // also cleanup if we ran into other errors in the loop
    PROP_GO(&e, e, cleanup_res);

cleanup_res:
    mysql_free_result(res);
    return e;
}

static derr_t mysql_encrypt(const dstr_t *debug_sock){
    derr_t e = E_OK;
    // get USER variable (that is, to whom we are encrypting)
    char* user = getenv("USER");

    // get null-terminated values, for mysql api
    char *sqlhost = NULL;
    char *sqluser = NULL;
    char *sqlpass = NULL;
    char *sqlsock = NULL;
    DSTR_VAR(d_sqlhost, 256);
    DSTR_VAR(d_sqluser, 256);
    DSTR_VAR(d_sqlpass, 256);
    DSTR_VAR(d_sqlsock, 256);
    if(debug_sock){
        PROP(&e, dstr_copy(debug_sock, &d_sqlsock) );
        PROP(&e, dstr_null_terminate(&d_sqlsock) );
        sqlsock = d_sqlsock.data;
    }else{
        // load the mysql config from a file
        DSTR_VAR(conf_text, 256);
        PROP(&e, dstr_read_file("/etc/encrypt_msg/encrypt_msg.conf", &conf_text) );
        // parse the file
        LIST_VAR(dstr_t, conf, 4);
        DSTR_STATIC(pattern, "\n");
        PROP(&e, dstr_split(&conf_text, &pattern, &conf) );
        // if there is a newline at the end of the file, ignore it
        if(conf.len == 4 && conf.data[3].len == 0) conf.len--;
        if(conf.len != 3){
            ORIG(&e, E_INTERNAL, "config file must contain exactly the mysql "
                    "host, user, and password, each on a separate line");
        }
        // get null-terminated values, for mysql api
        PROP(&e, dstr_copy(&conf.data[0], &d_sqlhost) );
        PROP(&e, dstr_copy(&conf.data[1], &d_sqluser) );
        PROP(&e, dstr_copy(&conf.data[2], &d_sqlpass) );
        PROP(&e, dstr_null_terminate(&d_sqlhost) );
        PROP(&e, dstr_null_terminate(&d_sqluser) );
        PROP(&e, dstr_null_terminate(&d_sqlpass) );
        sqlhost = d_sqlhost.data;
        sqluser = d_sqluser.data;
        sqlpass = d_sqlpass.data;
        sqlsock = "/var/run/mysqld/mysqld.sock";
    }

    // init mysql
    int ret = mysql_library_init(0, NULL, NULL);
    if(ret != 0){
        ORIG(&e, E_SQL, "unable to init mysql library");
    }

    // get a mysql object
    MYSQL sql;
    MYSQL* mret = mysql_init(&sql);
    if(!mret){
        ORIG_GO(&e, E_SQL, "unable to init mysql object", cleanup_sql);
    }

    // make a connection with mysqld
    mret = mysql_real_connect(&sql, sqlhost, sqluser, sqlpass,
                "splintermail", 0, sqlsock, 0);
    if(!mret){
        trace_sql_error(&e, &sql);
        ORIG_GO(&e, E_SQL, "unable to connect to mysqld", cleanup_sql_obj);
    }

    // get the user id
    unsigned int uid;
    PROP_GO(&e, get_user_id(&sql, user, &uid), cleanup_sql_obj);

    // use the user_id to get the encryption keys
    link_t keys;
    link_init(&keys);
    PROP_GO(&e, get_keys(&sql, uid, &keys), cu_keys);

    if(link_list_isempty(&keys)){
        TRACE(&e, "no keys for user %x\n", FS(user));
        ORIG_GO(&e, E_NOKEYS, "No keys for user", cu_keys);
    }

    // ready to start encrypting
    PROP_GO(&e, do_encryption(&keys), cu_keys);

    link_t *link;
cu_keys:
    while((link = link_list_pop_first(&keys))){
        keypair_t *kp = CONTAINER_OF(link, keypair_t, link);
        keypair_free(&kp);
    }
cleanup_sql_obj:
    mysql_close(&sql);
cleanup_sql:
    mysql_library_end();
    return e;
}

#endif

int main(int argc, char** argv){
    // specify command line options
    opt_spec_t o_debug = {'d', "debug", false, OPT_RETURN_INIT};
    opt_spec_t o_help = {'h', "help", false, OPT_RETURN_INIT};
#ifdef BUILD_SERVER_CODE
    opt_spec_t o_debug_sock = {'\0', "debug-sock", true, OPT_RETURN_INIT};
    opt_spec_t* spec[] = {&o_debug, &o_help, &o_debug_sock};
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
        printf("usage: USER=email encrypt_msg [--debug-sock SOCK]\n");
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
        const dstr_t *debug_sock = NULL;
        if(o_debug_sock.found){
            debug_sock = &o_debug_sock.val;
        }
        PROP_GO(&e, mysql_encrypt(debug_sock), cleanup_ssl);
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
    int exitval;
exit:
#ifdef BUILD_SERVER_CODE
    // determine exit code, since we will DROP any error before exiting
    exitval = (is_error(e));
    // any error at all (except for a user having no keys) is badbadbad
    CATCH(e, E_NOKEYS){
        // silently drop E_NOKEYS error, which is no error at all
    }else CATCH(e, E_ANY){
        // write errors to logfile
        DUMP(e);
        DSTR_STATIC(summary, "unexpected error in encrypt_message, check log");
        // badbadbad_alert will print to the log file directly
        badbadbad_alert(&summary, NULL);
        // put a line break in log file for ease of reading
        LOG_ERROR("\n");
    }
#else
    // the non-server case; report all errors
    DUMP(e);
#endif
    DROP_VAR(&e);
    return exitval;
}
