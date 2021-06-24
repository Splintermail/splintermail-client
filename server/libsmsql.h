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

// writes SMSQL_PASSWORD_SALT_SIZE random ASCII bytes
derr_t random_password_salt(dstr_t *salt);

// sha512 password hash
// silently truncates passwords > 128 bytes before hashing
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
// raise E_USERMSG on failure
derr_t valid_splintermail_email(const dstr_t *email);

// checks characters
bool valid_password_chars(const dstr_t *pass);

// checks length, characters, and no leading or trailing spaces
// raise E_USERMSG on failure
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
// the salt is generated from 12 random bytes and converted to hex
#define SMSQL_PASSWORD_SALT_SRC_SIZE 12
#define SMSQL_PASSWORD_SALT_SIZE (SMSQL_PASSWORD_SALT_SRC_SIZE * 4 / 3)
#define SMSQL_PASSWORD_SHA512_ROUNDS 500000
#define SMSQL_PASSWORD_SIZE 72
/* FPR is hex-encoded, since it is a precomputed-for-convenience column that
   isn't very meaningful except for showing to users */
#define SMSQL_FPR_SIZE 64

#define SMSQL_SESSION_HARD_TIMEOUT (24*60*60)
#define SMSQL_SESSION_SOFT_TIMEOUT (30*60)
#define SMSQL_CSRF_RANDOM_BYTES 33
#define SMSQL_CSRF_SIZE 44
#define SMSQL_CSRF_TIMEOUT (24*60*60)

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

// throws E_USERMSG if max aliases reached
derr_t add_random_alias(MYSQL *sql, const dstr_t *uuid, dstr_t *alias);

// throws E_USERMSG if alias is unavailable
derr_t add_primary_alias(MYSQL *sql, const dstr_t *uuid, const dstr_t *alias);

// throws E_USERMSG if no alias matched
derr_t delete_alias(MYSQL *sql, const dstr_t *uuid, const dstr_t *alias);

derr_t delete_all_aliases(MYSQL *sql, const dstr_t *uuid);

// devices

typedef struct {
    dstr_t dstr;
    link_t link;
} smsql_dstr_t;
DEF_CONTAINER_OF(smsql_dstr_t, link, link_t);

derr_t smsql_dstr_new(smsql_dstr_t **out, const dstr_t *val);
derr_t smsql_dstr_new_cstr(smsql_dstr_t **out, const char *val);
void smsql_dstr_free(smsql_dstr_t **old);

/* returns a list of hex-encoded fingerprints (smsql_dstr_t's), ordered
   by fingerprint */
derr_t list_device_fprs(MYSQL *sql, const dstr_t *uuid, link_t *out);

/* returns a list of pem-encoded public keys (smsql_dstr_t's), ordered
   by fingerprint */
derr_t list_device_keys(MYSQL *sql, const dstr_t *uuid, link_t *out);

// returns a user's device identified by a particular fingerprint
derr_t get_device(
    MYSQL *sql, const dstr_t *uuid, const dstr_t *fpr, dstr_t *key, bool *ok
);

// take a PEM-encoded public key, validate it, and add it to an account
// raises E_USERMSG on invalid or duplicate key
derr_t add_device(
    MYSQL *sql, const dstr_t *uuid, const dstr_t *pubkey, dstr_t *fpr
);

// throws E_USERMSG if no device matched
derr_t delete_device(MYSQL *sql, const dstr_t *uuid, const dstr_t *fpr_hex);

// tokens

typedef struct {
    uint32_t uint;
    link_t link;
} smsql_uint_t;
DEF_CONTAINER_OF(smsql_uint_t, link, link_t);

derr_t smsql_uint_new(smsql_uint_t **out, uint32_t val);
void smsql_uint_free(smsql_uint_t **old);

derr_t list_tokens(MYSQL *sql, const dstr_t *uuid, link_t *out);

derr_t add_token(
    MYSQL *sql, const dstr_t *uuid, uint32_t *token, dstr_t *secret
);

// throws E_USERMSG if no token matched
derr_t delete_token(MYSQL *sql, const dstr_t *uuid, uint32_t token);

// misc

// throws E_USERMSG on failure
derr_t create_account(
    MYSQL *sql,
    const dstr_t *email,
    const dstr_t *pass,
    dstr_t *uuid
);

// gateway is responsible for ensuring a password is provided first
// gateway is also responsible for calling trigger_deleter(), below
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

// returns uuid or throws E_USERMSG on failure
derr_t validate_login(
    MYSQL *sql, const dstr_t *email, const dstr_t *pass, dstr_t *uuid
);

// validate a token against the database, returning uuid and email
/* checks signature of payload against secret for token, but some higher-level
   checks like "does the path in the payload match the API path" are the
   responsibility of the gateway */
// raises E_USERMSG on error
derr_t validate_token_auth(
    MYSQL *sql,
    uint32_t token,
    uint64_t nonce,
    const dstr_t *payload,
    const dstr_t *sig,
    dstr_t *uuid
);

// the gateway should enforce a valid old password is provided first
// throws E_USERMSG on invalid password
derr_t change_password(MYSQL *sql, const dstr_t *uuid, const dstr_t *pass);

// uses time() for the login and last_seen times.
/* this API implies that you should always create a fresh session_id on login
   (which is already a mandatory practice to avoid session fixation attacks) */
derr_t add_session_auth(
    MYSQL *sql, int server_id, const dstr_t *session_id, const dstr_t *uuid
);

derr_t session_logout(MYSQL *sql, int server_id, const dstr_t *session_id);

/* check if a session id is valid, and get the user_uuid if it is.
   valid sessions meet the following criteria:
     - at least one row in the sessions table matches the session_id
     - login time is not older than the hard-timeout
     - last_seen time of any matching row is not older than the soft-timeout
     - no matching row has the void bit set

   Calling validate_session_auth() automatically updates the visited time
   of the session */
// throws E_USERMSG on bad sessions
derr_t validate_session_auth(
    MYSQL *sql, int server_id, const dstr_t *session_id, dstr_t *uuid
);

// new_csrf returns a token you can embed in a webpage
derr_t new_csrf(
    MYSQL *sql, int server_id, const dstr_t *session_id, dstr_t *csrf
);

// validate_csrf() just checks if the token was valid for this session
// throws E_USERMSG on bad tokens
derr_t validate_csrf(MYSQL *sql, const dstr_t *session_id, const dstr_t *csrf);

// returns true if uuid/address matches accounts.email or aliases.alias
derr_t user_owns_address(
    MYSQL *sql, const dstr_t *uuid, const dstr_t *address, bool *ok
);

/* This will atomically check if the user is allowed to send to this many
   recipients right now, and add recipients to the user's msg_count if so.
   If not, this function will return ok=false.
   If ok=true, msg_sent will also be true if this is the first time the query
   failed today (the accounts.msg_true is set to true as well).

   Therefore, only policy.py should ever call this function, as if ok and
   msg_sent both come back false it is the caller's responsibility to send
   the limit message. */
derr_t limit_check(
    MYSQL *sql,
    const dstr_t *uuid,
    unsigned int recipients,
    bool *ok,
    bool *msg_sent,
    unsigned int *limit
);

// used by health-check-service (hcs)
// out must be pre-allocated
derr_t gtid_current_pos(MYSQL *sql, dstr_t *out);

/* AFTER deleting an account, it is safe to try to trigger the deletions
   service.  It is expected that the caller is tolerant of errors, since they
   are often not fatal; the deleter will periodically GC any stray files. */
derr_t trigger_deleter(MYSQL *sql, const dstr_t *uuid);

// returns a list of uuids to delete (smsql_dstr_t's)
derr_t list_deletions(MYSQL *sql, int server_id, link_t *out);

// remove a deletions entry for this server_id
derr_t deletions_finished_one(MYSQL *sql, int server_id, const dstr_t *uuid);

derr_t gc_sessions_and_csrf(MYSQL *sql, int server_id, time_t now);

// sysadmin utils

// returns a list of smsql_dstr_t's
derr_t list_users(MYSQL *sql, link_t *out);

#endif // SM_SQL_H
