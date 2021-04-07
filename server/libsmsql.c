#include "libsmsql.h"

// helper functions

derr_t to_fsid(const dstr_t *uuid, dstr_t *out){
    derr_t e = E_OK;
    PROP(&e, bin2hex(uuid, out) );
    return e;
}

derr_t to_uuid(const dstr_t *fsid, dstr_t *out){
    derr_t e = E_OK;
    PROP(&e, hex2bin(fsid, out) );
    return e;
}

// predefined queries

derr_t get_uuid_for_email(
    MYSQL *sql, const dstr_t *email, dstr_t *uuid, bool *ok
){
    derr_t e = E_OK;
    if(uuid->size < SMSQL_UUID_SIZE){
        ORIG(&e, E_FIXEDSIZE, "uuid output too short");
    }

    DSTR_STATIC(query, "SELECT user_uuid from accounts where email = ?");

    PROP(&e,
        sql_onerow_query(
            sql, &query, ok,
            // params
            string_bind_in(email),
            // results
            string_bind_out(uuid)
        )
    );

    return e;
}

derr_t get_email_for_uuid(
    MYSQL *sql, const dstr_t *uuid, dstr_t *email, bool *ok
){
    derr_t e = E_OK;
    if(email->size < SMSQL_EMAIL_SIZE){
        ORIG(&e, E_FIXEDSIZE, "email output too short");
    }

    DSTR_STATIC(query, "SELECT email from accounts where user_uuid = ?");

    PROP(&e,
        sql_onerow_query(
            sql, &query, ok,
            // params
            string_bind_in(uuid),
            // results
            string_bind_out(email)
        )
    );

    return e;
}
