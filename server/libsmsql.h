#ifndef SMSQL_H
#define SMSQL_H

#include "mysql_util/mysql_util.h"

// helper functions

// same as the sql procedures
derr_t to_fsid(const dstr_t *uuid, dstr_t *out);
derr_t to_uuid(const dstr_t *fsid, dstr_t *out);

// FSID is secretly an alias for FX.  Someday we could potentially migrate
// FSID encoding schemes so both APIs remain.
#define FSID FX

// predefined queries

#define SMSQL_UUID_SIZE 32
/* FSID was once a modified base64 to be filesystem-safe, but it caused too
   many issues; first postfix would sometimes lower-case the base64, and also
   it would puke if the base64 began with a punctuation (emails aren't supposed
   to).  Those are both fixable, but saving 20 characters of text is not worth
   the complexity required, or the risk of finding other pitfalls so now the
   fsid is just a lower-case hex id, which can't possibly fail. */
#define SMSQL_FSID_SIZE 64
#define SMSQL_EMAIL_SIZE 100

derr_t get_uuid_for_email(
    MYSQL *sql, const dstr_t *email, dstr_t *uuid, bool *ok
);
derr_t get_email_for_uuid(
    MYSQL *sql, const dstr_t *uuid, dstr_t *email, bool *ok
);

#endif // SM_SQL_H
