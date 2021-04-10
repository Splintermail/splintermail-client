#include <string.h>
#include <mysqld_error.h>

#include "mysql_util.h"

#define TRACE_STMT(e, stmt) \
    TRACE((e), \
        "mysql_stmt_error(%x): %x\n", \
        FU(mysql_stmt_errno((stmt))), \
        FS(mysql_stmt_error((stmt))) \
    )

#define TRACE_SQL(e, sql) \
    TRACE((e), \
        "mysql_error(%x): %x\n", \
        FU(mysql_errno((sql))), \
        FSQL(sql) \
    )

REGISTER_ERROR_TYPE(E_SQL, "SQLERROR");
REGISTER_ERROR_TYPE(E_SQL_DUP, "SQL_DUP_ERR");

static derr_type_t _read_errno(unsigned int err){
    switch(err){
        case ER_DUP_ENTRY: return E_SQL_DUP;
    }
    return E_SQL;
}

static derr_type_t sql_err(MYSQL *sql){
    return _read_errno(mysql_errno(sql));
}

static derr_type_t stmt_err(MYSQL_STMT *stmt){
    return _read_errno(mysql_stmt_errno(stmt));
}

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
    const dstr_t *db
){
    derr_t e = E_OK;

    // get null-terminated values, for mysql api
    char *sqluser = NULL;
    char *sqlpass = NULL;
    char *sqlsock = "/var/run/mysqld/mysqld.sock";
    char *sqldb = NULL;

    DSTR_VAR(d_sqluser, 256);
    DSTR_VAR(d_sqlpass, 256);
    DSTR_VAR(d_sqlsock, 256);
    DSTR_VAR(d_sqldb, 256);

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

    if(db){
        PROP(&e, dstr_copy(db, &d_sqldb) );
        PROP(&e, dstr_null_terminate(&d_sqldb) );
        sqldb = d_sqldb.data;
    }

    // NULL for host means "use unix socket"
    char *null_host = NULL;

    MYSQL *mret = mysql_real_connect(
        sql, null_host, sqluser, sqlpass, sqldb, 0, sqlsock, 0
    );
    if(!mret){
        TRACE_SQL(&e, sql);
        ORIG(&e, sql_err(sql), "unable to connect");
    }

    return e;
}

derr_t sql_connect_unix(
    MYSQL *sql, const dstr_t *user, const dstr_t *pass, const dstr_t *sock
){
    derr_t e = E_OK;

    DSTR_STATIC(db, "splintermail");

    PROP(&e, sql_connect_unix_ex(sql, user, pass, sock, &db) );

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
        TRACE_SQL(&e, sql);
        TRACE(&e, "while running: %x\n", FD(query));
        ORIG(&e, sql_err(sql), "mysql_real_query failed");
    }

    return e;
}


derr_t sql_exec_multi(MYSQL *sql, const dstr_t *stmts){
    derr_t e = E_OK;

    // multi-statements on
    int ret = mysql_set_server_option(sql, MYSQL_OPTION_MULTI_STATEMENTS_ON);
    if(ret){
        TRACE_SQL(&e, sql);
        ORIG(&e, sql_err(sql), "failed to enable multi statement support");
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
            TRACE_SQL(&e, sql);
            ORIG(&e, sql_err(sql), "error getting query result");
        }
        // more results? -1 = no, >0 = error, 0 = yes (keep looping)
        ret = mysql_next_result(sql);
        if(ret < 0){
            // no more results
            break;
        }else if(ret > 0){
            // error
            TRACE_SQL(&e, sql);
            ORIG(&e, sql_err(sql), "error getting next result");
        }
    }

    // multi-statements off
    ret = mysql_set_server_option(sql, MYSQL_OPTION_MULTI_STATEMENTS_OFF);
    if(ret){
        TRACE_SQL(&e, sql);
        ORIG(&e, sql_err(sql), "failed to disable multi statement support");
    }

    return e;
}


derr_t sql_use_result(MYSQL *sql, MYSQL_RES **res){
    derr_t e = E_OK;

    *res = mysql_use_result(sql);
    if(!*res){
        TRACE_SQL(&e, sql);
        ORIG(&e, sql_err(sql), "mysql_use_result failed");
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
        TRACE_SQL(&e, sql);
        ORIG(&e, sql_err(sql), "failed to init new statment");
    }

    return e;
}

derr_t sql_stmt_prepare(MYSQL_STMT *stmt, const dstr_t *text){
    derr_t e = E_OK;

    int ret = mysql_stmt_prepare(stmt, text->data, text->len);
    if(ret){
        TRACE_STMT(&e, stmt);
        TRACE(&e, "while preparing: %x\n", FD(text));
        ORIG(&e, stmt_err(stmt), "failed to prepare statement");
    }

    return e;
}

derr_t _sql_stmt_bind_params(MYSQL_STMT *stmt, MYSQL_BIND *args, size_t nargs){
    derr_t e = E_OK;

    // ensure that we have the right parameter count before binding
    long unsigned int nparams = mysql_stmt_param_count(stmt);
    if(nparams != nargs){
        TRACE(&e, "expected %x params but got %x\n", FU(nargs), FU(nparams));
        ORIG(&e, E_INTERNAL, "wrong number of params");
    }

    // bind params
    bool ret = mysql_stmt_bind_param(stmt, args);
    if(ret){
        TRACE_STMT(&e, stmt);
        ORIG(&e, stmt_err(stmt), "failed to bind params");
    }

    return e;
}

derr_t _sql_stmt_bind_results(
    MYSQL_STMT *stmt, MYSQL_BIND *args, size_t nargs
){
    derr_t e = E_OK;

    // ensure that we have the right field count before binding
    long unsigned int nfields = mysql_stmt_field_count(stmt);
    if(nfields != nargs){
        TRACE(&e, "expected %x fields but got %x\n", FU(nargs), FU(nfields));
        ORIG(&e, E_INTERNAL, "wrong number of fields");
    }

    // bind results
    bool ret = mysql_stmt_bind_result(stmt, args);
    if(ret){
        TRACE_STMT(&e, stmt);
        ORIG(&e, stmt_err(stmt), "failed to bind results");
    }

    return e;
}

derr_t sql_read_bit_dstr(const dstr_t *val, bool *out){
    derr_t e = E_OK;

    *out = false;

    if(val->len != 1){
        TRACE(&e, "invalid length for bit string: %x\n", FU(val->len));
        ORIG(&e, E_INTERNAL, "invalid length for bit string");
    }

    *out = !(val->data[0] == '\0');

    return e;
}

derr_t sql_stmt_execute(MYSQL_STMT *stmt){
    derr_t e = E_OK;

    if(mysql_stmt_execute(stmt)){
        TRACE_STMT(&e, stmt);
        ORIG(&e, stmt_err(stmt), "failed to exec stmt");
    }

    return e;
}

static derr_t _stmt_rows_affected(MYSQL_STMT *stmt, size_t *out){
    derr_t e = E_OK;

    my_ulonglong affected = mysql_stmt_affected_rows(stmt);
    if(affected == ((my_ulonglong)-1)){
        TRACE_STMT(&e, stmt);
        ORIG(&e, stmt_err(stmt), "failed to get affected rows");
    }

    if(affected > SIZE_MAX){
        ORIG(&e, E_INTERNAL, "too many results!");
    }

    *out = affected;

    return e;
}

derr_t _sql_norow_query(
    MYSQL *sql,
    const dstr_t *query,
    unsigned long *affected,
    MYSQL_BIND *args,
    size_t nargs
){
    derr_t e = E_OK;
    if(affected) *affected = 0;

    // create a statement object
    MYSQL_STMT *stmt;
    PROP(&e, sql_stmt_init(sql, &stmt) );

    // prepare the statement
    PROP_GO(&e, sql_stmt_prepare(stmt, query), cu_stmt);

    // ensure that the statement will not produce results
    unsigned int nfields = mysql_stmt_field_count(stmt);
    if(nfields > 0){
        TRACE(&e, "expected no return fields but got %x\n", FU(nfields));
        ORIG_GO(&e,
            E_INTERNAL, "non-zero return fields in sql_norow_query()",
        cu_stmt);
    }

    // bind arguments
    PROP_GO(&e, _sql_stmt_bind_params(stmt, args, nargs), cu_stmt);

    // execute
    PROP_GO(&e, sql_stmt_execute(stmt), cu_stmt);

    if(affected) PROP_GO(&e, _stmt_rows_affected(stmt, affected), cu_stmt);

cu_stmt:
    mysql_stmt_close(stmt);
    return e;
}

derr_t _sql_onerow_query(
    MYSQL *sql, const dstr_t *query, bool *ok, MYSQL_BIND *args, size_t nargs
){
    derr_t e = E_OK;
    if(ok) *ok = false;

    // create a statement object
    MYSQL_STMT *stmt;
    PROP(&e, sql_stmt_init(sql, &stmt) );

    // prepare the statement
    PROP_GO(&e, sql_stmt_prepare(stmt, query), cu_stmt);

    // count input and output fields
    long unsigned int ins = mysql_stmt_param_count(stmt);
    unsigned int outs = mysql_stmt_field_count(stmt);

    if(ins + outs != nargs){
        TRACE(&e,
            "sum of params (%x) and fields (%x) does not match the"
            "number of args provided (%x)\n", FU(ins), FU(outs), FU(nargs));
        ORIG_GO(&e,
            E_INTERNAL, "param/field count mismatch",
        cu_stmt);
    }

    if(outs == 0){
        TRACE(&e, "expected return fields but got none\n");
        ORIG_GO(&e,
            E_INTERNAL, "zero return fields in sql_onerow_query()",
        cu_stmt);
    }

    // bind arguments
    PROP_GO(&e, _sql_stmt_bind_params(stmt, args, ins), cu_stmt);

    // execute
    PROP_GO(&e, sql_stmt_execute(stmt), cu_stmt);

    // bind results
    PROP_GO(&e, _sql_stmt_bind_results(stmt, &args[ins], outs), cu_stmt);

    // always fetch all results to avoid the CR_COMMANDS_OUT_OF_SYNC error
    size_t nrows = 0;
    bool had_trunc = false;
    int ret;
    while(true){
        ret = mysql_stmt_fetch(stmt);
        // break only for errors or NO_DATA
        if(ret == 1 || ret == MYSQL_NO_DATA) break;
        // check for truncated data
        if(ret == MYSQL_DATA_TRUNCATED) had_trunc = true;
        nrows++;
    }

   if(ret == 1){
        // normal errors
        TRACE_STMT(&e, stmt);
        ORIG_GO(&e, stmt_err(stmt), "failed to fetch result", cu_stmt);
    }

    if(had_trunc){
        // it is the caller's responsibility to guarantee this cannot happen
        ORIG_GO(&e, E_INTERNAL, "truncated data detected", cu_stmt);
    }

    if(nrows > 1){
        TRACE(&e, "expected 1 row but got %x\n", FU(nrows));
        ORIG_GO(&e,
            stmt_err(stmt), "too many rows in sql_onerow_query()",
        cu_stmt);
    }else if(nrows == 1){
        if(ok) *ok = true;
    }else{
        // user set ok=NULL, meaning a result should be guaranteed
        if(ok){
            *ok=false;
        }else{
            ORIG(&e,
                E_INTERNAL, "onerow query guaranteed a response but got none"
            );
        }
    }

cu_stmt:
    mysql_stmt_close(stmt);
    return e;
}

derr_t _sql_multirow_stmt(
    MYSQL *sql,
    MYSQL_STMT **stmt,
    const dstr_t *query,
    MYSQL_BIND *args,
    size_t nargs
){
    derr_t e = E_OK;

    // create a statement object
    PROP(&e, sql_stmt_init(sql, stmt) );

    // prepare the statement
    PROP_GO(&e, sql_stmt_prepare(*stmt, query), fail);

    // count input and output fields
    long unsigned int ins = mysql_stmt_param_count(*stmt);
    unsigned int outs = mysql_stmt_field_count(*stmt);

    if(ins + outs != nargs){
        TRACE(&e,
            "sum of params (%x) and fields (%x) does not match the"
            "number of args provided (%x)\n", FU(ins), FU(outs), FU(nargs));
        ORIG_GO(&e,
            E_INTERNAL, "param/field count mismatch",
        fail);
    }

    if(outs == 0){
        TRACE(&e, "expected return fields but got none\n");
        ORIG_GO(&e,
            E_INTERNAL, "zero return fields in sql_multirow_query()",
        fail);
    }

    // bind arguments
    PROP_GO(&e, _sql_stmt_bind_params(*stmt, args, ins), fail);

    // execute
    PROP_GO(&e, sql_stmt_execute(*stmt), fail);

    // bind results
    PROP_GO(&e, _sql_stmt_bind_results(*stmt, &args[ins], outs), fail);

    return e;

fail:
    mysql_stmt_close(*stmt);
    *stmt = NULL;
    return e;
}

// sets data into the previously-defined BINDs.
derr_t sql_stmt_fetch(MYSQL_STMT *stmt, bool *ok){
    derr_t e = E_OK;
    *ok = true;

    int ret = mysql_stmt_fetch(stmt);

    if(ret == 1){
        TRACE_STMT(&e, stmt);
        ORIG(&e, stmt_err(stmt), "failed to fetch result");
    }

    if(ret == MYSQL_DATA_TRUNCATED){
        ORIG(&e, E_INTERNAL, "truncated data detected");
    }

    if(ret == MYSQL_NO_DATA){
        *ok = false;
    }

    return e;
}

// only useful in error handling; will destroy memory pointed to by BINDs
void sql_stmt_fetchall(MYSQL_STMT *stmt){
    while(true){
        int ret = mysql_stmt_fetch(stmt);
        // break only for errors or NO_DATA
        if(ret == 1 || ret == MYSQL_NO_DATA) break;
    }
}

derr_t sql_txn_start(MYSQL *sql){
    derr_t e = E_OK;
    PROP(&e, sql_query(sql, &DSTR_LIT("START TRANSACTION")));
    return e;
}

derr_t sql_txn_commit(MYSQL *sql){
    derr_t e = E_OK;
    PROP(&e, sql_query(sql, &DSTR_LIT("COMMIT")));
    return e;
}

derr_t sql_txn_rollback(MYSQL *sql){
    derr_t e = E_OK;
    PROP(&e, sql_query(sql, &DSTR_LIT("ROLLBACK")));
    return e;
}

// if txn rollback fails, it closes the mysql object
void sql_txn_abort(MYSQL *sql){
    derr_t e = E_OK;
    IF_PROP(&e, sql_query(sql, &DSTR_LIT("ROLLBACK"))) {
        // this seems by far most likely due to connection issues, so just
        // drop the error; that will be obvious already.
        DROP_VAR(&e);

        // render the connection unusable
        mysql_close(sql);
    }
}
