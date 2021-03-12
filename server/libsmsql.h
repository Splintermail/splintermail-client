#ifndef SMSQL_H
#define SMSQL_H

#include "mysql_util/mysql_util.h"

#define MAX_RANDOM_ALIASES 1000
#define MAX_DEVICES 10

// helper functions

// same as the sql procedures
derr_t to_fsid(const dstr_t *uuid, dstr_t *out);
derr_t to_uuid(const dstr_t *fsid, dstr_t *out);

derr_type_t fmthook_fsid(dstr_t* out, const void* arg);

static inline fmt_t FSID(const dstr_t* uuid){
    return (fmt_t){FMT_EXT, {.ext = {.arg = (const void*)uuid,
                                     .hook = fmthook_fsid} } };
}

// writes SMSQL_PASSWORD_SALT_SIZE bytes
derr_t random_password_salt(dstr_t *salt);

// sha512 password hash
derr_t hash_password(
    const dstr_t *pass, unsigned int rounds, const dstr_t *salt, dstr_t *hash
);

// just the hash validation
derr_t validate_password_hash(
    const dstr_t *pass, const dstr_t *true_hash, bool *ok
);

// validation functions

// checks characters
bool valid_username_chars(const dstr_t *username);

// checks length, ending, and characters
derr_t valid_splintermail_email(const dstr_t *email);

// checks characters
bool valid_password_chars(const dstr_t *pass);

// checks length, characters, and no leading or trailing spaces
derr_t valid_splintermail_password(const dstr_t *pass);

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
#define SMSQL_APISECRET_SIZE 44
#define SMSQL_EMAIL_SIZE 100
#define SMSQL_PUBKEY_SIZE 1024
#define SMSQL_PASSWORD_HASH_SIZE 128
#define SMSQL_PASSWORD_SALT_SIZE 8
#define SMSQL_PASSWORD_SHA512_ROUNDS 500000
#define SMSQL_PASSWORD_SIZE 72
/* FPR is hex-encoded, since it is a precomputed-for-convenience column that
   isn't very meaningful except for showing to users */
#define SMSQL_FPR_SIZE 64

derr_t get_uuid_for_email(
    MYSQL *sql, const dstr_t *email, dstr_t *uuid, bool *ok
);

derr_t get_email_for_uuid(
    MYSQL *sql, const dstr_t *uuid, dstr_t *email, bool *ok
);

// aliases

typedef struct {
    dstr_t alias;
    bool paid;
    link_t link;
} smsql_alias_t;
DEF_CONTAINER_OF(smsql_alias_t, link, link_t);

derr_t smsql_alias_new(smsql_alias_t **out, const dstr_t *email, bool paid);
void smsql_alias_free(smsql_alias_t **old);

// returns a list of smsql_alias_t's
derr_t list_aliases(MYSQL *sql, const dstr_t *uuid, link_t *out);

derr_t add_random_alias(
    MYSQL *sql, const dstr_t *uuid, dstr_t *alias, bool *ok
);

derr_t add_primary_alias(
    MYSQL *sql, const dstr_t *uuid, const dstr_t *alias, bool *ok
);

derr_t delete_alias(
    MYSQL *sql, const dstr_t *uuid, const dstr_t *alias, bool *deleted
);

derr_t delete_all_aliases(MYSQL *sql, const dstr_t *uuid);

// devices

typedef struct {
    dstr_t dstr;
    link_t link;
} smsql_dstr_t;
DEF_CONTAINER_OF(smsql_dstr_t, link, link_t);

derr_t smsql_dstr_new(smsql_dstr_t **out, const dstr_t *val);
void smsql_dstr_free(smsql_dstr_t **old);

//// will these ever become useful?
// typedef struct {
//     dstr_t public_key;
//     // fingerprint is hex-encoded
//     dstr_t fingerprint;
//     link_t link;
// } smsql_device_t;
// DEF_CONTAINER_OF(smsql_device_t, link, link_t);
//
// derr_t smsql_device_new(
//     smsql_device_t **out, const dstr_t *public_key, const dstr_t *fingerprint
// );
// void smsql_device_free(smsql_device_t **old);

// returns a list of hex-encoded fingerprints (smsql_dstr_t's)
derr_t list_device_fprs(MYSQL *sql, const dstr_t *uuid, link_t *out);

// returns a list of pem-encoded public keys (smsql_dstr_t's)
derr_t list_device_keys(MYSQL *sql, const dstr_t *uuid, link_t *out);

// take a PEM-encoded public key, validate it, and add it to an account
derr_t add_device(
    MYSQL *sql, const dstr_t *uuid, const dstr_t *pubkey, bool *ok
);

derr_t delete_device(MYSQL *sql, const dstr_t *uuid, const dstr_t *fpr_hex);

// tokens

typedef struct {
    unsigned int uint;
    link_t link;
} smsql_uint_t;
DEF_CONTAINER_OF(smsql_uint_t, link, link_t);

derr_t smsql_uint_new(smsql_uint_t **out, unsigned int val);
void smsql_uint_free(smsql_uint_t **old);

derr_t list_tokens(MYSQL *sql, const dstr_t *uuid, link_t *out);

derr_t add_token(
    MYSQL *sql, const dstr_t *uuid, unsigned int *token, dstr_t *secret
);

derr_t delete_token(MYSQL *sql, const dstr_t *uuid, unsigned int token);

// misc

// gateway is responsible for quality checks on the email
derr_t create_account(
    MYSQL *sql,
    const dstr_t *email,
    const dstr_t *pass_hash,
    bool *ok,
    dstr_t *uuid
);

// gateway is responsible for ensuring a password is provided
derr_t delete_account(MYSQL *sql, const dstr_t *uuid);

derr_t account_info(
    MYSQL *sql,
    const dstr_t *uuid,
    size_t *num_devices,
    size_t *num_primary_aliases,
    size_t *num_random_aliases
);

// validate a password for a user against the database
derr_t validate_user_password(
    MYSQL *sql, const dstr_t *uuid, const dstr_t *pass, bool *ok
);

/* the gateway should enforce a valid old password is provided before calling
   this to change to the new password */
// gateway is also responsible for quality checks on the password
derr_t change_password(MYSQL *sql, const dstr_t *uuid, const dstr_t *pass);

#endif // SM_SQL_H
