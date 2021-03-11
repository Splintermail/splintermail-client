// based on docs at: www.zend.com/resources/writing-php-extensions

// this module does not support the ZTS caching behavior at all
// I think it's ok because the server doesn't use thread-safe php anyway

#include <php.h>
#include <ext/standard/info.h>

// undefine syslog values that we do not need anywhere in our file.
// (probably should rename these macros at some point)
#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_ERROR

#include "libdstr/libdstr.h"
#include "server/mysql_util/mysql_util.h"
#include "server/libsmsql.h"

#include "smphp.h"

/* For compatibility with older PHP versions */
#ifndef ZEND_PARSE_PARAMETERS_NONE
#define ZEND_PARSE_PARAMETERS_NONE() \
    ZEND_PARSE_PARAMETERS_START(0, 0) \
    ZEND_PARSE_PARAMETERS_END()
#endif

ZEND_DECLARE_MODULE_GLOBALS(smphp)

// configure sql socket, but only via php.ini (that is what PHP_INI_SYSTEM is)
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY(
        "smphp.sql_sock",
        "/var/run/sql/sock",
        PHP_INI_SYSTEM,
        OnUpdateString,
        sql_sock,
        zend_smphp_globals,
        smphp_globals
    )
PHP_INI_END()

/* consume a derr_t and return:
   - an empty zend_string on success, or
   - a user-facing error string if applicable, or
   - NULL on any other error */
static zend_string *print_error(derr_t e_in){
    if(!is_error(e_in)){
        /* this doesn't actually allocate anything; it returns the
           empty "interned" string (immutable and not reference counted) */
        return ZSTR_EMPTY_ALLOC();
    }

    derr_t e = E_OK;

    DSTR_STATIC(line_prefix, "ERROR: ");
    if(
        e_in.type == E_PARAM
     && e_in.msg.data != NULL
     && dstr_beginswith(&e_in.msg, &line_prefix))
    {
        // get the first line of the message
        dstr_t line;
        dstr_split2_soft(e_in.msg, DSTR_LIT("\n"), NULL, &line, NULL);

        // get the message without the "ERROR: " prefix
        const dstr_t msg = dstr_sub(&line, line_prefix.len, line.len);

        zend_string *out = zend_string_init(msg.data, msg.len, false);
        DROP_VAR(&e_in);
        return out;
    }

    DROP_VAR(&e);
#ifdef BUILD_DEBUG
    // debug builds: dump errors to stderr
    DUMP(e_in);
#else
    // production builds: report errors via badbadbad alerts
    DSTR_VAR(summary, 128);
    DROP_CMD( FMT(&e, "smphp: %x", FD(e_in->dstr)) );
    badbadbad_alert(&summary, e_in.msg);
#endif
    DROP_VAR(&e_in);
    /* just return NULL; the php code has to check for NULL values anyway,
       so there is no point in providing a generic fallback in two places */
    return NULL;
}


static derr_t _create_account(const dstr_t *email, const dstr_t *pass){
    derr_t e = E_OK;

    // apply gateway checks
    PROP(&e, valid_splintermail_email(email) );
    PROP(&e, valid_splintermail_password(pass) );

    MYSQL sql;
    MYSQL* mret = mysql_init(&sql);
    if(!mret){
        ORIG(&e, E_SQL, "unable to init mysql object");
    }

    char *sql_sock = (char *)smphp_globals.sql_sock;
    dstr_t sock;
    DSTR_WRAP(sock, sql_sock, strlen(sql_sock), true);
    PROP_GO(&e, sql_connect_unix(&sql, NULL, NULL, &sock), cu_sql);

    DSTR_VAR(salt, SMSQL_PASSWORD_SALT_SIZE);
    PROP_GO(&e, random_password_salt(&salt), cu_sql);

    DSTR_VAR(hash, SMSQL_PASSWORD_HASH_SIZE);
    PROP_GO(&e,
        hash_password(pass, SMSQL_PASSWORD_SHA512_ROUNDS, &salt, &hash),
    cu_sql);

    bool ok;
    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP_GO(&e, create_account(&sql, email, &hash, &ok, &uuid), cu_sql);

    if(!ok){
        ORIG_GO(&e, E_PARAM, "username not available", cu_sql);
    }

cu_sql:
    mysql_close(&sql);

    return e;
}

static derr_t _login(const dstr_t *email, const dstr_t *pass, dstr_t *uuid){
    derr_t e = E_OK;

    MYSQL sql;
    MYSQL* mret = mysql_init(&sql);
    if(!mret){
        ORIG(&e, E_SQL, "unable to init mysql object");
    }

    char *sql_sock = (char *)smphp_globals.sql_sock;
    dstr_t sock;
    DSTR_WRAP(sock, sql_sock, strlen(sql_sock), true);
    PROP_GO(&e, sql_connect_unix(&sql, NULL, NULL, &sock), cu_sql);

    bool ok;
    PROP_GO(&e, get_uuid_for_email(&sql, email, uuid, &ok), cu_sql);
    if(!ok){
        ORIG_GO(&e, E_PARAM, "invalid credentials", cu_sql);
    }

    PROP_GO(&e, validate_user_password(&sql, uuid, pass, &ok), cu_sql);
    if(!ok){
        ORIG_GO(&e, E_PARAM, "invalid credentials", cu_sql);
    }

cu_sql:
    mysql_close(&sql);

    return e;
}

// $error = smphp_create_account(email: string, pass: string)
// $error === NULL means internal server error
// $error !== "" means $error is a user-facing error
// otherwise success
PHP_FUNCTION(smphp_create_account){
    dstr_t email;
    dstr_t pass;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(email.data, email.len)
        Z_PARAM_STRING(pass.data, pass.len)
    ZEND_PARSE_PARAMETERS_END();

    zend_string *error = print_error( _create_account(&email, &pass) );
    if(error != NULL){
        RETURN_STR(error);
    }

    return;
}

// list($uuid, $error) = smphp_login(email: string, pass: string)
// $error === "" means ok, $uuid will be valid
// $error === NULL means internal server error
// otherwise $error is user-facing
PHP_FUNCTION(smphp_login){
    dstr_t email;
    dstr_t pass;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(email.data, email.len)
        Z_PARAM_STRING(pass.data, pass.len)
    ZEND_PARSE_PARAMETERS_END();

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    zend_string *error = print_error( _login(&email, &pass, &uuid) );

    ZVAL_ARR(return_value, zend_new_array(2));
    /* if return_value is NULL then after:
            list($error, $uuid) = smsql_login(email, pass);
       php will print a warning about unknown keys and will set $error and
       $uuid to NULL */
    if(return_value == NULL){
        return;
    }

    // write the UUID first so we know if to write the error string or not.
    zend_string *zuuid = zend_string_init(uuid.data, uuid.len, false);
    if(!zuuid){
        // let $error and $hash be NULL by returning now.
        return;
    }

    // mysteriously, this doesn't return any status.
    add_index_str(return_value, 0, zuuid);

    // write the error
    if(error != NULL){
        add_index_str(return_value, 1, error);
    }
    return;
}

PHP_MINIT_FUNCTION(smphp){
    REGISTER_INI_ENTRIES();

#ifdef BUILD_DEBUG
    // debug_builds: log issues to stderr
    DROP_CMD( logger_add_fileptr(LOG_LVL_DEBUG, stderr) );
#endif

    int ret = mysql_library_init(0, NULL, NULL);
    if(ret != 0){
        return FAILURE;
    }

    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(smphp){
    mysql_library_end();
    return SUCCESS;
}

PHP_MINFO_FUNCTION(smphp){
    php_info_print_table_start();
    php_info_print_table_header(2, "smphp support", "enabled");
    php_info_print_table_end();
}

ZEND_BEGIN_ARG_INFO(arginfo_smphp_create_account, 2)
    ZEND_ARG_INFO(0, email)
    ZEND_ARG_INFO(0, pass)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_smphp_login, 2)
    ZEND_ARG_INFO(0, email)
    ZEND_ARG_INFO(0, pass)
ZEND_END_ARG_INFO()

static const zend_function_entry smphp_functions[] = {
    PHP_FE(smphp_create_account,  arginfo_smphp_create_account)
    PHP_FE(smphp_login,           arginfo_smphp_login)
    PHP_FE_END
};

zend_module_entry smphp_module_entry = {
    STANDARD_MODULE_HEADER,
    "smphp",                     /* Extension name */
    smphp_functions,             /* zend_function_entry */
    PHP_MINIT(smphp),            /* PHP_MINIT - Module initialization */
    PHP_MSHUTDOWN(smphp),        /* PHP_MSHUTDOWN - Module shutdown */
    NULL,                        /* PHP_RINIT - Request initialization */
    NULL,                        /* PHP_RSHUTDOWN - Request shutdown */
    PHP_MINFO(smphp),            /* PHP_MINFO - Module info */
    "0.1.0"          ,           /* Version */
    PHP_MODULE_GLOBALS(smphp),   /* Module globals */
    NULL,                        /* PHP_GINIT - Globals initialization */
    NULL,                        /* PHP_GSHUTDOWN - Globals shutdown */
    NULL,
    STANDARD_MODULE_PROPERTIES_EX
};

ZEND_GET_MODULE(smphp)
