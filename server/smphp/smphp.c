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
#include "server/badbadbad_alert.h"

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
        "/var/run/mysqld/mysql.sock",
        PHP_INI_SYSTEM,
        OnUpdateString,
        sql_sock,
        zend_smphp_globals,
        smphp_globals
    )
    STD_PHP_INI_ENTRY(
        "smphp.server_id",
        "-1",
        PHP_INI_SYSTEM,
        OnUpdateLong,
        server_id,
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
        e_in.type == E_USERMSG
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
#endif
    // production AND debug builds: report errors via badbadbad alerts
    DSTR_VAR(summary, 128);
    DROP_CMD( FMT(&summary, "smphp: %x", FD(&e_in.msg)) );
    badbadbad_alert(&summary, &e_in.msg);
    DROP_VAR(&e_in);
    /* just return NULL; the php code has to check for NULL values anyway,
       so there is no point in providing a generic fallback in two places */
    return NULL;
}


static derr_t _sql_init(MYSQL *sql){
    derr_t e = E_OK;

    MYSQL* mret = mysql_init(sql);
    if(!mret){
        ORIG(&e, E_SQL, "unable to init mysql object");
    }

    // connect with php.ini's sql_sock setting
    char *sql_sock = (char *)smphp_globals.sql_sock;
    dstr_t sock;
    DSTR_WRAP(sock, sql_sock, strlen(sql_sock), true);
    PROP_GO(&e, sql_connect_unix(sql, NULL, NULL, &sock), fail);

    return e;

fail:
    mysql_close(sql);
    return e;
}


// consume a derr_t and report it via badbadbad
static void _drop(derr_t e_in){
    if(!is_error(e_in)) return;
#ifdef BUILD_DEBUG
    // debug builds: dump errors to stderr
    DUMP(e_in);
#endif
    // production AND debug builds: report errors via badbadbad alerts
    DSTR_VAR(summary, 128);
    DROP_CMD( FMT(&summary, "smphp: %x", FD(&e_in.msg)) );
    badbadbad_alert(&summary, &e_in.msg);
    DROP_VAR(&e_in);
}


static derr_t _create_account(
    const dstr_t *email, const dstr_t *pass, dstr_t *uuid
){
    derr_t e = E_OK;

    MYSQL sql;
    PROP(&e, _sql_init(&sql) );

    PROP_GO(&e, create_account(&sql, email, pass, uuid), cu_sql);

cu_sql:
    mysql_close(&sql);

    return e;
}

static derr_t _login(const dstr_t *email, const dstr_t *pass, dstr_t *uuid){
    derr_t e = E_OK;

    MYSQL sql;
    PROP(&e, _sql_init(&sql) );

    PROP_GO(&e, validate_login(&sql, email, pass, uuid), cu_sql);

cu_sql:
    mysql_close(&sql);

    return e;
}

static derr_t _add_session_auth(
    int server_id, const dstr_t *session_id, const dstr_t *uuid
){
    derr_t e = E_OK;

    MYSQL sql;
    PROP(&e, _sql_init(&sql) );

    PROP_GO(&e, add_session_auth(&sql, server_id, session_id, uuid), cu_sql);

cu_sql:
    mysql_close(&sql);

    return e;
}


static derr_t _validate_session_auth(
    int server_id, const dstr_t *session_id, dstr_t *email
){
    derr_t e = E_OK;

    MYSQL sql;
    PROP(&e, _sql_init(&sql) );

    // validate session
    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    PROP_GO(&e,
        validate_session_auth(&sql, server_id, session_id, &uuid),
    cu_sql);

    // lookup username (useful to php)
    bool ok;
    PROP_GO(&e, get_email_for_uuid(&sql, &uuid, email, &ok), cu_sql);
    if(!ok){
        // this is possible in race conditions but it's more likely a bug
        TRACE(&e, "session_id:%x uuid:%x", FD(session_id), FSID(&uuid));
        ORIG_GO(&e, E_INTERNAL, "session with no account was found", cu_sql);
    }

cu_sql:
    mysql_close(&sql);

    return e;
}

static derr_t _session_logout(int server_id, const dstr_t *session_id){
    derr_t e = E_OK;

    MYSQL sql;
    PROP(&e, _sql_init(&sql) );

    PROP_GO(&e, session_logout(&sql, server_id, session_id), cu_sql);

cu_sql:
    mysql_close(&sql);

    return e;
}


static derr_t _new_csrf(
    int server_id, const dstr_t *session_id, dstr_t *csrf
){
    derr_t e = E_OK;

    MYSQL sql;
    PROP(&e, _sql_init(&sql) );

    PROP_GO(&e, new_csrf(&sql, server_id, session_id, csrf), cu_sql);

cu_sql:
    mysql_close(&sql);

    return e;
}

// $error = smphp_valid_email(email: string)
// $error === NULL means internal server error
// $error !== "" means $error is a user-facing error
// otherwise success
PHP_FUNCTION(smphp_valid_email){
    dstr_t email;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(email.data, email.len)
    ZEND_PARSE_PARAMETERS_END();

    zend_string *error = print_error( valid_splintermail_email(&email) );
    if(error != NULL){
        RETURN_STR(error);
    }

    return;
}

// $error = smphp_valid_password(password: string)
// $error === NULL means internal server error
// $error !== "" means $error is a user-facing error
// otherwise success
PHP_FUNCTION(smphp_valid_password){
    dstr_t password;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(password.data, password.len)
    ZEND_PARSE_PARAMETERS_END();

    zend_string *error = print_error( valid_splintermail_password(&password) );
    if(error != NULL){
        RETURN_STR(error);
    }

    return;
}

// list($uuid, $error) = smphp_create_account(email: string, pass: string)
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

    DSTR_VAR(uuid, SMSQL_UUID_SIZE);
    zend_string *error = print_error( _create_account(&email, &pass, &uuid) );
    if(error == NULL){
        return;
    }

    ZVAL_ARR(return_value, zend_new_array(2));
    /* if return_value is NULL then after:
            list($error, $uuid) = smsql_create_account(email, pass);
       php will print a warning about unknown keys and will set $error and
       $uuid to NULL */
    if(return_value == NULL){
        return;
    }

    // write the UUID first so we know if to write the error string or not.
    zend_string *zuuid = zend_string_init(uuid.data, uuid.len, false);
    if(!zuuid){
        // let $error and $uuid be NULL by returning now.
        return;
    }

    // mysteriously, this doesn't return any status.
    add_index_str(return_value, 0, zuuid);

    // write the error
    add_index_str(return_value, 1, error);

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
    if(error == NULL){
        return;
    }

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
        // let $error and $uuid be NULL by returning now.
        return;
    }

    // mysteriously, this doesn't return any status.
    add_index_str(return_value, 0, zuuid);

    // write the error
    add_index_str(return_value, 1, error);

    return;
}

// smphp_add_session_auth(server_id: int, session_id: string, uuid: string)
// (no return value)
PHP_FUNCTION(smphp_add_session_auth){
    long server_id = 0;
    dstr_t session_id;
    dstr_t uuid;

    ZEND_PARSE_PARAMETERS_START(3, 3)
        Z_PARAM_LONG(server_id)
        Z_PARAM_STRING(session_id.data, session_id.len)
        Z_PARAM_STRING(uuid.data, uuid.len)
    ZEND_PARSE_PARAMETERS_END();

    if(server_id > INT_MAX || server_id < INT_MIN) return;

    _drop( _add_session_auth((int)server_id, &session_id, &uuid) );
}

// smphp_validate_session_auth(
//     server_id: int, session_id: string
// ) -> email: Optional[string]
PHP_FUNCTION(smphp_validate_session_auth){
    long server_id = 0;
    dstr_t session_id;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_LONG(server_id)
        Z_PARAM_STRING(session_id.data, session_id.len)
    ZEND_PARSE_PARAMETERS_END();

    if(server_id > INT_MAX || server_id < INT_MIN) return;

    derr_t e = E_OK;

    DSTR_VAR(email, SMSQL_EMAIL_SIZE);
    derr_t e2 = _validate_session_auth((int)server_id, &session_id, &email);
    CATCH(e2, E_USERMSG){
        // user is not logged in; drop the user message and return NULL
        DROP_VAR(&e2);
        // return NULL
        return;
    }else PROP_VAR_GO(&e, &e2, fail);

    zend_string *out = zend_string_init(email.data, email.len, false);
    if(out != NULL){
        RETURN_STR(out);
    }

fail:
    _drop(e);
}

// smphp_session_logout(server_id: int, session_id: string)
// (no return value)
PHP_FUNCTION(smphp_session_logout){
    long server_id = 0;
    dstr_t session_id;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_LONG(server_id)
        Z_PARAM_STRING(session_id.data, session_id.len)
    ZEND_PARSE_PARAMETERS_END();

    if(server_id > INT_MAX || server_id < INT_MIN) return;

    _drop( _session_logout((int)server_id, &session_id) );
}


// smphp_new_csrf(server_id: int, session_id: string) -> string
PHP_FUNCTION(smphp_new_csrf){
    long server_id = 0;
    dstr_t session_id;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_LONG(server_id)
        Z_PARAM_STRING(session_id.data, session_id.len)
    ZEND_PARSE_PARAMETERS_END();

    if(server_id > INT_MAX || server_id < INT_MIN) return;

    derr_t e = E_OK;

    DSTR_VAR(csrf, SMSQL_CSRF_SIZE);
    PROP_GO(&e, _new_csrf((int)server_id, &session_id, &csrf), fail);

    zend_string *out = zend_string_init(csrf.data, csrf.len, false);
    if(out != NULL){
        RETURN_STR(out);
    }

fail:
    _drop(e);
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

ZEND_BEGIN_ARG_INFO(arginfo_smphp_valid_email, 1)
    ZEND_ARG_INFO(0, email)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_smphp_valid_password, 1)
    ZEND_ARG_INFO(0, password)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_smphp_create_account, 2)
    ZEND_ARG_INFO(0, email)
    ZEND_ARG_INFO(0, pass)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_smphp_login, 2)
    ZEND_ARG_INFO(0, email)
    ZEND_ARG_INFO(0, pass)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_smphp_add_session_auth, 3)
    ZEND_ARG_INFO(0, server_id)
    ZEND_ARG_INFO(0, session_id)
    ZEND_ARG_INFO(0, uuid)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_smphp_validate_session_auth, 2)
    ZEND_ARG_INFO(0, server_id)
    ZEND_ARG_INFO(0, session_id)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_smphp_session_logout, 2)
    ZEND_ARG_INFO(0, server_id)
    ZEND_ARG_INFO(0, session_id)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_smphp_new_csrf, 2)
    ZEND_ARG_INFO(0, server_id)
    ZEND_ARG_INFO(0, session_id)
ZEND_END_ARG_INFO()

#define SMPHP_FE(fn) PHP_FE(smphp_ ## fn, arginfo_smphp_ ## fn)
static const zend_function_entry smphp_functions[] = {
    SMPHP_FE(valid_email)
    SMPHP_FE(valid_password)
    SMPHP_FE(create_account)
    SMPHP_FE(login)
    SMPHP_FE(add_session_auth)
    SMPHP_FE(validate_session_auth)
    SMPHP_FE(session_logout)
    SMPHP_FE(new_csrf)
    PHP_FE_END
};
#undef SMPHP_FE

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
