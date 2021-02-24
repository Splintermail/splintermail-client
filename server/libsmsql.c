#include "libsmsql.h"

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
