#include <string.h>

#include "mysql_util.h"

REGISTER_ERROR_TYPE(E_SQL, "SQLERROR");

derr_type_t fmthook_sql_error(dstr_t* out, void* arg){
    MYSQL *sql = arg;
    const char *msg = mysql_error(sql);
    // dereferencing msg really is the cannonical way to check for an error
    if(!*msg){
        msg = "(no error)";
    }
    size_t len = strlen(msg);
    // make sure the message will fit
    derr_type_t type = dstr_grow_quiet(out, out->len + len);
    if(type) return type;
    // copy the message
    memcpy(out->data + out->len, msg, len);
    out->len += len;
    return E_NONE;
}

derr_t sql_connect_unix_ex(
    MYSQL *sql,
    const dstr_t *user,
    const dstr_t *pass,
    const dstr_t *sock,
    const char *dbname
){
    derr_t e = E_OK;

    // get null-terminated values, for mysql api
    char *sqluser = NULL;
    char *sqlpass = NULL;
    char *sqlsock = "/var/run/mysqld/mysqld.sock";

    DSTR_VAR(d_sqluser, 256);
    DSTR_VAR(d_sqlpass, 256);
    DSTR_VAR(d_sqlsock, 256);

    // get null-terminated values, for mysql api
    if(user){
        PROP(&e, dstr_copy(user, &d_sqluser) );
        PROP(&e, dstr_null_terminate(&d_sqluser) );
        sqluser = d_sqluser.data;
    }

    if(pass){
        PROP(&e, dstr_copy(pass, &d_sqlpass) );
        PROP(&e, dstr_null_terminate(&d_sqlpass) );
        sqlpass = d_sqlpass.data;
    }

    if(sock){
        PROP(&e, dstr_copy(sock, &d_sqlsock) );
        PROP(&e, dstr_null_terminate(&d_sqlsock) );
        sqlsock = d_sqlsock.data;
    }

    // NULL for host means "use unix socket"
    char *null_host = NULL;

    MYSQL *mret = mysql_real_connect(
        sql, null_host, sqluser, sqlpass, dbname, 0, sqlsock, 0
    );
    if(!mret){
        TRACE(&e, "mysql_error: %x\n", FSQL(sql));
        ORIG(&e, E_SQL, "unable to connect");
    }

    return e;
}

derr_t sql_connect_unix(
    MYSQL *sql, const dstr_t *user, const dstr_t *pass, const dstr_t *sock
){
    derr_t e = E_OK;

    PROP(&e, sql_connect_unix_ex(sql, user, pass, sock, "splintermail") );

    return e;
}

derr_t sql_query(MYSQL *sql, const dstr_t *query){
    derr_t e = E_OK;

    // mariadb will puke if you pass a zero length string
    if(!query->len){
        ORIG(&e, E_INTERNAL, "empty queries not allowed");
    }

    int ret = mysql_real_query(sql, query->data, query->len ? query->len : 1);
    if(ret){
        TRACE(&e, "mysql_error: %x\n", FSQL(sql));
        TRACE(&e, "while running: %x\n", FD(query));
        ORIG(&e, E_SQL, "mysql_real_query failed");
    }

    return e;
}


derr_t sql_exec_multi(MYSQL *sql, const dstr_t *stmts){
    derr_t e = E_OK;

    // multi-statements on
    int ret = mysql_set_server_option(sql, MYSQL_OPTION_MULTI_STATEMENTS_ON);
    if(ret){
        TRACE(&e, "mysql_error: %x\n", FSQL(sql));
        ORIG(&e, E_SQL, "failed to enable multi statement support");
    }

    // execute the block of statements
    PROP(&e, sql_query(sql, stmts) );

    // The multi-statement result loop is insanely complex:
    // see: https://dev.mysql.com/doc/c-api/8.0/en/c-api-multiple-queries.html
    while(true) {
        MYSQL_RES *res = mysql_store_result(sql);
        if(res){
            // the current statement returned some data (we ignore it)
            mysql_free_result(res);
        }else if(mysql_field_count(sql) == 0) {
            // current statement returned no data (still don't care)
        }else{
            // error; res should not have been NULL
            TRACE(&e, "mysql_error: %x\n", FSQL(sql));
            ORIG(&e, E_SQL, "error getting query result");
        }
        // more results? -1 = no, >0 = error, 0 = yes (keep looping)
        ret = mysql_next_result(sql);
        if(ret < 0){
            // no more results
            break;
        }else if(ret > 0){
            // error
            TRACE(&e, "mysql_error: %x\n", FSQL(sql));
            ORIG(&e, E_SQL, "error getting next result");
        }
    }

    // multi-statements off
    ret = mysql_set_server_option(sql, MYSQL_OPTION_MULTI_STATEMENTS_OFF);
    if(ret){
        TRACE(&e, "mysql_error: %x\n", FSQL(sql));
        ORIG(&e, E_SQL, "failed to disable multi statement support");
    }

    return e;
}


derr_t sql_use_result(MYSQL *sql, MYSQL_RES **res){
    derr_t e = E_OK;

    *res = mysql_use_result(sql);
    if(!*res){
        TRACE(&e, "mysql_error: %x\n", FSQL(sql));
        ORIG(&e, E_SQL, "mysql_use_result failed");
    }

    return e;
}


derr_t _sql_read_row(
    MYSQL_RES *res, MYSQL_ROW *row, dstr_t **args, size_t nargs
){
    derr_t e = E_OK;

    unsigned int nfields = mysql_num_fields(res);
    if(nfields != nargs){
        TRACE(&e, "expected %x fields but got %x\n", FU(nargs), FU(nfields));
        ORIG(&e, E_INTERNAL, "wrong nfields");
    }

    unsigned long *lens = mysql_fetch_lengths(res);

    for(unsigned int i = 0; i < nfields; i++){
        dstr_t field;
        DSTR_WRAP(field, (*row)[i], lens[i], 0);
        *args[i] = field;
    }

    return e;
}

derr_t sql_stmt_init(MYSQL *sql, MYSQL_STMT **stmt){
    derr_t e = E_OK;

    *stmt = mysql_stmt_init(sql);
    if(!*stmt){
        TRACE(&e, "mysql_error: %x\n", FSQL(sql));
        ORIG(&e, E_SQL, "failed to init new statment");
    }

    return e;
}

derr_t sql_stmt_prepare(MYSQL_STMT *stmt, const dstr_t *text){
    derr_t e = E_OK;

    int ret = mysql_stmt_prepare(stmt, text->data, text->len);
    if(ret){
        TRACE(&e, "mysql_stmt_error: %x\n", FS(mysql_stmt_error(stmt)));
        TRACE(&e, "while preparing: %x\n", FD(text));
        ORIG(&e, E_SQL, "failed to prepare statement");
    }

    return e;
}

derr_t _sql_stmt_bind_params(MYSQL_STMT *stmt, MYSQL_BIND *args, size_t nargs){
    derr_t e = E_OK;

    // ensure that we have the right parameter count before binding
    long unsigned int nparams = mysql_stmt_param_count(stmt);
    if(nparams != nargs){
        TRACE(&e, "expected %x params but got %x\n", FU(nargs), FU(nparams));
        ORIG(&e, E_SQL, "wrong number of params");
    }

    // bind params
    int ret = mysql_stmt_bind_param(stmt, args);
    if(ret){
        TRACE(&e, "mysql_stmt_error: %x\n", FS(mysql_stmt_error(stmt)));
        ORIG(&e, E_SQL, "failed to bind params");
    }

    return e;
}

derr_t sql_stmt_execute(MYSQL_STMT *stmt){
    derr_t e = E_OK;

    if(mysql_stmt_execute(stmt)){
        TRACE(&e, "mysql_stmt_error: %x\n", FS(mysql_stmt_error(stmt)));
        ORIG(&e, E_SQL, "failed to exec stmt");
    }

    return e;
}

derr_t _sql_bound_stmt(
    MYSQL *sql, const dstr_t *query, MYSQL_BIND *args, size_t nargs
){
    derr_t e = E_OK;

    // create a statement object
    MYSQL_STMT *stmt;
    PROP(&e, sql_stmt_init(sql, &stmt) );

    // prepare the statement
    PROP_GO(&e, sql_stmt_prepare(stmt, query), cu_stmt);

    // bind arguments
    PROP_GO(&e, _sql_stmt_bind_params(stmt, args, nargs), cu_stmt);

    // execute
    PROP_GO(&e, sql_stmt_execute(stmt), cu_stmt);

cu_stmt:
    mysql_stmt_close(stmt);
    return e;
}
