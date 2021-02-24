#include "libdstr/libdstr.h"
#include "server/mysql_util/mysql_util.h"
#include "server/libsmsql.h"

#include "pysm.h"

derr_t smsql_init(
    py_smsql_t *self, const dstr_t sock, const dstr_t *user, const dstr_t *pass
){
    derr_t e = E_OK;

    dstr_copy(&sock, &self->sock);

    if(user != NULL){
        self->_user = true;
        dstr_copy(user, &self->user);
    }

    if(pass != NULL){
        self->_pass = true;
        dstr_copy(pass, &self->pass);
    }

    return e;
}

void smsql_deinit(py_smsql_t *self){
    dstr_free(&self->sock);
    dstr_free(&self->user);
    dstr_free(&self->pass);

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

    MYSQL* mret = mysql_init(&self->sql);
    if(!mret){
        ORIG(&e, E_SQL, "unable to init mysql object");
    }

    PROP_GO(&e,
        sql_connect_unix(
            &self->sql,
            self->_user ? &self->user : NULL,
            self->_pass ? &self->pass : NULL,
            &self->sock
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
