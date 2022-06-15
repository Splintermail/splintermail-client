// python throws a false array-bounds error
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif // __GNUC__

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif // __GNUC__

#include "libdstr/libdstr.h"
#include "server/mysql_util/mysql_util.h"
#include "server/libsmsql.h"

#include "args.h"

// some python error has already been raised
extern derr_type_t E_NORAISE;

// custom type; represents a MYSQL connection
typedef struct {
    PyObject_HEAD
    dstr_t sock;
    bool _user;
    dstr_t user;
    bool _pass;
    dstr_t pass;
    bool _db;
    dstr_t db;
    bool _sql;
    MYSQL sql;
} py_smsql_t;

derr_t smsql_init(
    py_smsql_t *self,
    const dstr_t sock,
    const dstr_t *user,
    const dstr_t *pass,
    const dstr_t *db
);
void smsql_deinit(py_smsql_t *self);

derr_t smsql_connect(py_smsql_t *self);
void smsql_close(py_smsql_t *self);
