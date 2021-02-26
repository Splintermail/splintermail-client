#include "libsmsql.h"

// helper functions

static derr_type_t to_fsid_quiet(const dstr_t *uuid, dstr_t *out){
    size_t start_len = out->len;
    derr_type_t type = bin2b64_quiet(uuid, out, 0, true, NULL);
    if(type) return type;
    // replace '/' with '-' to be filesystem safe
    for(size_t i = start_len; i < out->len; i++){
        if(out->data[i] == '/') out->data[i] = '-';
    }
    return type;
}

static derr_type_t unfsid_quiet(const dstr_t *fsid, dstr_t *out){
    // replace '-' with '/' to be valid base64
    for(size_t i = 0; i < fsid->len; i++){
        if(fsid->data[i] == '-') fsid->data[i] = '/';
    }
    derr_type_t type = b642bin_quiet(fsid, out, NULL);
    if(type) return type;
    return type;
}

derr_t to_fsid(const dstr_t *uuid, dstr_t *out){
    derr_t e = E_OK;
    derr_type_t type = to_fsid_quiet(uuid, out);
    if(type) ORIG(&e, type, "output error");
    return e;
}

derr_t to_uuid(const dstr_t *uuid, dstr_t *out){
    derr_t e = E_OK;
    derr_type_t type = unfsid_quiet(uuid, out);
    if(type) ORIG(&e, type, "output error");
    return e;
}

derr_type_t fmthook_fsid(dstr_t* out, const void* arg){
    // cast the input
    const dstr_t* uuid = (const dstr_t*)arg;
    derr_type_t type = to_fsid_quiet(uuid, out);
    if(type) return type;
    return E_NONE;
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
