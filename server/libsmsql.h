#ifndef SMSQL_H
#define SMSQL_H

#include "mysql_util/mysql_util.h"

// helper functions

// same as the sql procedures
derr_t to_fsid(const dstr_t *uuid, dstr_t *out);
derr_t to_uuid(const dstr_t *fsid, dstr_t *out);

derr_type_t fmthook_fsid(dstr_t* out, const void* arg);

static inline fmt_t FSID(const dstr_t* uuid){
    return (fmt_t){FMT_EXT, {.ext = {.arg = (const void*)uuid,
                                     .hook = fmthook_fsid} } };
}

// predefined queries

#define SMSQL_UUID_SIZE 32
#define SMSQL_FSID_SIZE 44
#define SMSQL_EMAIL_SIZE 100

derr_t get_uuid_for_email(
    MYSQL *sql, const dstr_t *email, dstr_t *uuid, bool *ok
);
derr_t get_email_for_uuid(
    MYSQL *sql, const dstr_t *uuid, dstr_t *email, bool *ok
);

#endif // SM_SQL_H
