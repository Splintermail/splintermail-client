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

// validation functions

// checks characters
bool valid_username_chars(const dstr_t *username);

// checks length, ending, and characters
derr_t valid_splintermail_email(const dstr_t *email);

// predefined queries

#define SMSQL_UUID_SIZE 32
/* FSID is a modified base64 to be filesystem-safe, and also to be
   email-localpart-safe.  This is done by converting '/' into '-', prefixing
   with an F to ensure the address never starts with punctuation, and removing
   the training "=" to shorten things.  Postfix also must be configured to not
   case-fold the localpart when it invokes dovecot-lda.

   This is a lot of complexity to shorten the uuid email address, but when I
   tried using hex-encoding (~20 extra characters) way too many log messages
   got truncated and it was hard to debug the system. */
#define SMSQL_FSID_SIZE 44
#define SMSQL_EMAIL_SIZE 100

derr_t get_uuid_for_email(
    MYSQL *sql, const dstr_t *email, dstr_t *uuid, bool *ok
);

derr_t get_email_for_uuid(
    MYSQL *sql, const dstr_t *uuid, dstr_t *email, bool *ok
);

derr_t add_primary_alias(
    MYSQL *sql, const dstr_t *uuid, const dstr_t *alias, bool *ok
);

derr_t delete_alias(
    MYSQL *sql, const dstr_t *uuid, const dstr_t *alias, bool *deleted
);


#endif // SM_SQL_H
