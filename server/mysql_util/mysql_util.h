#ifndef MYSQL_HELPER_H
#define MYSQL_HELPER_H

// mysql throws missing prototype error with gcc, so ignore that warning
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#endif // __GNUC__

#include <mysql.h>

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif // __GNUC__

#include "libdstr/libdstr.h"

#include "binds.h"

// an error in the SQL library
extern derr_type_t E_SQL;

derr_type_t fmthook_sql_error(dstr_t* out, void* arg);

static inline fmt_t FSQL(MYSQL *sql){
    return (fmt_t){
        FMT_EXT_NOCONST,
        {
            .ext_noconst = {
                .arg = (void*)sql,
                .hook = fmthook_sql_error,
            }
        }
    };
}

// sock is allowed to be NULL (defaults to "/var/run/mysqld/mysqld.sock")
derr_t sql_connect_unix_ex(
    MYSQL *sql,
    const dstr_t *user,
    const dstr_t *pass,
    const dstr_t *sock,
    const char *dbname
);

derr_t sql_connect_unix(
    MYSQL *sql, const dstr_t *user, const dstr_t *pass, const dstr_t *sock
);

derr_t sql_query(MYSQL *sql, const dstr_t *query);

derr_t sql_exec_multi(MYSQL *sql, const dstr_t *stmts);

derr_t sql_use_result(MYSQL *sql, MYSQL_RES **res);

derr_t _sql_read_row(
    MYSQL_RES *res, MYSQL_ROW *row, dstr_t **args, size_t nargs
);
#define sql_read_row(res, row, ...) \
    _sql_read_row( \
        (res), \
        (row), \
        &(dstr_t*[]){NULL, __VA_ARGS__}[1], \
        sizeof((dstr_t*[]){NULL, __VA_ARGS__}) / sizeof(dstr_t*) - 1 \
    )


derr_t sql_stmt_init(MYSQL *sql, MYSQL_STMT **stmt);

derr_t sql_stmt_prepare(MYSQL_STMT *stmt, const dstr_t *query);

derr_t _sql_stmt_bind_params(MYSQL_STMT *stmt, MYSQL_BIND *args, size_t nargs);
#define sql_stmt_bind_params(stmt, ...) \
    _sql_stmt_bind_params( \
        (stmt), \
        &(MYSQL_BIND[]){(MYSQL_BIND){0}, __VA_ARGS__}[1], \
        sizeof((MYSQL_BIND[]){(MYSQL_BIND){0}, __VA_ARGS__}) / sizeof(MYSQL_BIND) - 1 \
    )

derr_t _sql_stmt_bind_results(MYSQL_STMT *stmt, MYSQL_BIND *args, size_t nargs);
#define sql_stmt_bind_results(stmt, ...) \
    _sql_stmt_bind_results( \
        (stmt), \
        &(MYSQL_BIND[]){(MYSQL_BIND){0}, __VA_ARGS__}[1], \
        sizeof((MYSQL_BIND[]){(MYSQL_BIND){0}, __VA_ARGS__}) / sizeof(MYSQL_BIND) - 1 \
    )

derr_t sql_read_bit_dstr(const dstr_t *val, bool *out);

derr_t sql_stmt_execute(MYSQL_STMT *stmt);

derr_t _sql_bound_stmt(
    MYSQL *sql, const dstr_t *query, MYSQL_BIND *args, size_t nargs
);
#define sql_bound_stmt(sql, query, ...) \
    _sql_bound_stmt( \
        (sql), \
        (query), \
        &(MYSQL_BIND[]){(MYSQL_BIND){0}, __VA_ARGS__}[1], \
        sizeof((MYSQL_BIND[]){(MYSQL_BIND){0}, __VA_ARGS__}) / sizeof(MYSQL_BIND) - 1 \
    )

// returns ok=true if a row was received
derr_t _sql_onerow_query(
    MYSQL *mysql, const dstr_t *query, bool *ok, MYSQL_BIND *args, size_t nargs
);
#define sql_onerow_query(sql, query, ok, ...) \
    _sql_onerow_query( \
        (sql), \
        (query), \
        (ok), \
        &(MYSQL_BIND[]){(MYSQL_BIND){0}, __VA_ARGS__}[1], \
        sizeof((MYSQL_BIND[]){(MYSQL_BIND){0}, __VA_ARGS__}) / sizeof(MYSQL_BIND) - 1 \
    )

#endif // MYSQL_HELPER_H