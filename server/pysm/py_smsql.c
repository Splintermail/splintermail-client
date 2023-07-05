#include "libdstr/libdstr.h"
#include "server/mysql_util/mysql_util.h"
#include "server/libsmsql.h"

#include "pysm.h"

derr_t smsql_init(
    py_smsql_t *self,
    const dstr_t sock,
    const dstr_t *user,
    const dstr_t *pass,
    const dstr_t *db
){
    (void)db;
    derr_t e = E_OK;

    PROP_GO(&e, dstr_copy(&sock, &self->sock), fail);

    if(user != NULL){
        self->_user = true;
        PROP_GO(&e, dstr_copy(user, &self->user), fail);
    }

    if(pass != NULL){
        self->_pass = true;
        PROP_GO(&e, dstr_copy(pass, &self->pass), fail);
    }

    if(db != NULL){
        self->_db = true;
        PROP_GO(&e, dstr_copy(db, &self->db), fail);
    }

    return e;

fail:
    smsql_deinit(self);
    return e;
}

void smsql_deinit(py_smsql_t *self){
    dstr_free(&self->sock);
    dstr_free(&self->user);
    dstr_free(&self->pass);
    dstr_free(&self->db);

    if(self->_sql){
        mysql_close(&self->sql);
        self->_sql = false;
    }
}

derr_t smsql_connect(py_smsql_t *self){
    derr_t e = E_OK;

    if(self->_sql){
        ORIG(&e, E_INTERNAL, "sql already connected!");
    }

    PROP(&e, dmysql_init(&self->sql) );

    PROP_GO(&e,
        sql_connect_unix_ex(
            &self->sql,
            self->_user ? &self->user : NULL,
            self->_pass ? &self->pass : NULL,
            &self->sock,
            self->_db ? &self->db : NULL
        ),
    fail_sql);

    self->_sql = true;
    return e;

fail_sql:
    mysql_close(&self->sql);
    return e;
}

void smsql_close(py_smsql_t *self){
    if(!self->_sql) return;
    mysql_close(&self->sql);
    self->_sql = false;
}
