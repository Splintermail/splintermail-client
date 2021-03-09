#include <stdlib.h>

#include <openssl/evp.h>

#include "libcrypto/libcrypto.h"
#include "libsmsql.h"
#include "petname/petname.h"

// helper functions

static derr_type_t to_fsid_quiet(const dstr_t *uuid, dstr_t *out){
    // use a local buffer so bin2b64 doesn't write an extra byte
    DSTR_VAR(buf, SMSQL_FSID_SIZE);

    derr_type_t type = bin2b64_quiet(uuid, &buf, 0, true, NULL);
    if(type) return type;
    // replace '/' with '-' to be filesystem safe
    for(size_t i = 0; i < buf.len; i++){
        if(buf.data[i] == '/') buf.data[i] = '-';
    }

    // write the 'F' prefix to ensure the output is email-localpart-safe
    type = dstr_append_quiet(out, &DSTR_LIT("F"));
    if(type) return type;

    // write all but the final '=' of the output
    dstr_t sub = dstr_sub2(buf, 0, buf.len - 1);
    type = dstr_append_quiet(out, &sub);
    if(type) return type;

    return E_NONE;
}

static derr_type_t to_uuid_quiet(const dstr_t *fsid, dstr_t *out){
    // make a copy so we don't violate the (implicit) const input constraint
    DSTR_VAR(b64, SMSQL_FSID_SIZE);

    // ignore the initial 'F'
    dstr_t sub = dstr_sub2(*fsid, 1, fsid->len);
    derr_type_t type = dstr_append_quiet(&b64, &sub);
    if(type) return type;

    // append the final "="
    type = dstr_append_quiet(&b64, &DSTR_LIT("="));
    if(type) return type;

    // replace '-' with '/' to be valid base64
    for(size_t i = 0; i < b64.len; i++){
        if(b64.data[i] == '-') b64.data[i] = '/';
    }
    type = b642bin_quiet(&b64, out, NULL);
    if(type) return type;

    return E_NONE;
}

derr_t to_fsid(const dstr_t *uuid, dstr_t *out){
    derr_t e = E_OK;
    derr_type_t type = to_fsid_quiet(uuid, out);
    if(type) ORIG(&e, type, "output error");
    return e;
}

derr_t to_uuid(const dstr_t *uuid, dstr_t *out){
    derr_t e = E_OK;
    derr_type_t type = to_uuid_quiet(uuid, out);
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

// validation functions

bool valid_username_chars(const dstr_t *username){
    for(size_t i = 0; i < username->len; i++){
        char c = username->data[i];
        if(c >= 'a' && c <= 'z') continue;
        if(c >= '0' && c <= '9') continue;
        if(c == '.' || c == '-' || c == '_') continue;
        return false;
    }
    return true;
}

derr_t valid_splintermail_email(const dstr_t *email){
    derr_t e = E_OK;

    // length
    if(email->len > SMSQL_EMAIL_SIZE)
        ORIG(&e, E_PARAM, "email too long");

    // ends in @splintermail.com
    DSTR_STATIC(suffix, "@splintermail.com");
    if(!dstr_endswith(email, &suffix))
        ORIG(&e, E_PARAM, "email must end in @splintermail.com");

    // valid username
    const dstr_t username = dstr_sub(email, 0, email->len - suffix.len);
    if(!valid_username_chars(&username))
        ORIG(&e, E_PARAM, "invalid characters in email");

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

// aliases

derr_t smsql_alias_new(smsql_alias_t **out, const dstr_t *email, bool paid){
    derr_t e = E_OK;
    *out = NULL;

    smsql_alias_t *alias = malloc(sizeof(*alias));
    if(!alias) ORIG(&e, E_NOMEM, "nomem");
    *alias = (smsql_alias_t){.paid = paid};

    link_init(&alias->link);

    PROP_GO(&e, dstr_copy(email, &alias->alias), fail);

    *out = alias;
    return e;

fail:
    free(alias);
    return e;
}

void smsql_alias_free(smsql_alias_t **old){
    if(*old == NULL) return;
    dstr_free(&(*old)->alias);
    free(*old);
    *old = NULL;
}

derr_t list_aliases(MYSQL *sql, const dstr_t *uuid, link_t *out){
    derr_t e = E_OK;

    MYSQL_STMT *stmt;

    DSTR_VAR(alias_res, SMSQL_EMAIL_SIZE);
    bool paid_res;

    DSTR_STATIC(q1, "SELECT alias, paid from aliases where user_uuid=?");
    PROP(&e,
        sql_multirow_stmt(
            sql, &stmt, &q1,
            // parameters
            blob_bind_in(uuid),
            // results
            string_bind_out(&alias_res),
            bool_bind_out(&paid_res)
        )
    );

    link_t list;
    link_init(&list);
    link_t *link;

    while(true){
        bool ok;
        PROP_GO(&e, sql_stmt_fetch(stmt, &ok), fail_list);
        if(!ok) break;

        smsql_alias_t *alias;
        PROP_GO(&e, smsql_alias_new(&alias, &alias_res, paid_res), loop_fail);
        link_list_append(&list, &alias->link);

        continue;

    loop_fail:
        sql_stmt_fetchall(stmt);
        goto fail_list;
    }

    // set the output
    link_list_append_list(out, &list);

    mysql_stmt_close(stmt);

    return e;

fail_list:
    while((link = link_list_pop_first(&list))){
        smsql_alias_t *alias = CONTAINER_OF(link, smsql_alias_t, link);
        smsql_alias_free(&alias);
    }
    mysql_stmt_close(stmt);
    return e;
}

static derr_t _add_random_alias_txn(
        MYSQL *sql, const dstr_t *uuid, dstr_t *alias, bool *ok){
    derr_t e = E_OK;

    /* this count-then-append strategy is vulnerable to write-skew, but giving
       some users a couple free aliases is just fine */
    uint64_t count;
    DSTR_STATIC(q1, "select COUNT(*) from aliases where user_uuid=?;");
    PROP(&e,
        sql_onerow_query(
            sql, &q1, NULL, blob_bind_in(uuid), uint64_bind_out(&count)
        )
    );

    if(count > MAX_RANDOM_ALIASES){
        // too many aliases
        *ok = false;
        return e;
    }

    // try for an unused random alias
    for(size_t limit = 0; limit < 1000; limit++){
        DSTR_VAR(temp, SMSQL_EMAIL_SIZE);
        PROP(&e, petname_email(&temp) );

        // double-check that the email is valid
        NOFAIL(&e, E_PARAM, valid_splintermail_email(&temp) );

        DSTR_STATIC(q2, "INSERT INTO emails (email) VALUES (?)");
        derr_t e2 = sql_bound_stmt(sql, &q2, string_bind_in(&temp));
        CATCH(e2, E_SQL_DUP){
            // chose a duplicate alias, try again
            DROP_VAR(&e2);
            continue;
        }else PROP(&e, e2);

        DSTR_STATIC(q3,
            "INSERT INTO aliases (alias, paid, user_uuid) VALUES (?, ?, ?)"
        );
        bool paid = false;
        PROP(&e,
            sql_bound_stmt(sql,
                &q3,
                string_bind_in(&temp),
                bool_bind_in(&paid),
                blob_bind_in(uuid),
            )
        );

        PROP(&e, dstr_append(alias, &temp) );

        *ok = true;
        return e;
    }

    ORIG(&e, E_INTERNAL, "failed to find an available alias");
}

derr_t add_random_alias(
    MYSQL *sql, const dstr_t *uuid, dstr_t *alias, bool *ok
){
    derr_t e = E_OK;
    *ok = false;

    PROP(&e, sql_txn_start(sql) );

    PROP_GO(&e, _add_random_alias_txn(sql, uuid, alias, ok), hard_fail);

    if(*ok){
        PROP(&e, sql_txn_commit(sql) );
    }else{
        // soft fail
        PROP(&e, sql_txn_rollback(sql) );
    }

    return e;

hard_fail:
    sql_txn_abort(sql);
    *ok = false;

    return e;
}


static derr_t _add_primary_alias_txn(
    MYSQL *sql, const dstr_t *uuid, const dstr_t *alias, bool *ok
){
    derr_t e = E_OK;

    DSTR_STATIC(q1, "INSERT INTO emails (email) VALUES (?)");
    PROP(&e, sql_bound_stmt(sql, &q1, string_bind_in(alias)) );

    DSTR_STATIC(q2,
        "INSERT INTO aliases (alias, paid, user_uuid) VALUES (?, ?, ?)"
    );
    bool paid = true;
    PROP(&e,
        sql_bound_stmt(sql,
            &q2,
            string_bind_in(alias),
            bool_bind_in(&paid),
            blob_bind_in(uuid),
        )
    );

    *ok = true;

    return e;
}

derr_t add_primary_alias(
    MYSQL *sql, const dstr_t *uuid, const dstr_t *alias, bool *ok
){
    derr_t e = E_OK;
    *ok = false;

    PROP(&e, valid_splintermail_email(alias) );

    PROP(&e, sql_txn_start(sql) );

    derr_t e2 = _add_primary_alias_txn(sql, uuid, alias, ok);
    CATCH(e2, E_SQL_DUP){
        DROP_VAR(&e2);
        *ok = false;
    }else PROP_GO(&e, e2, hard_fail);

    if(*ok){
        PROP(&e, sql_txn_commit(sql) );
    }else{
        // soft fail
        PROP(&e, sql_txn_rollback(sql) );
    }

    return e;

hard_fail:
    sql_txn_abort(sql);
    *ok = false;

    return e;
}

static derr_t _delete_alias_txn(
    MYSQL *sql, const dstr_t *uuid, const dstr_t *alias, bool *deleted
){
    derr_t e = E_OK;

    // find out if the alias was paid
    DSTR_STATIC(q1, "SELECT paid FROM aliases WHERE alias=? AND user_uuid=?");
    bool ok;
    bool paid;
    PROP(&e,
        sql_onerow_query(
            sql, &q1, &ok,
            // params
            string_bind_in(alias),
            blob_bind_in(uuid),
            // results
            bool_bind_out(&paid))
    );

    if(!ok){
        // no matching alias
        return e;
    }

    DSTR_STATIC(q2, "DELETE FROM aliases WHERE alias=?");
    PROP(&e, sql_bound_stmt(sql, &q2, string_bind_in(alias)) );

    // delete from emails table last (for foriegn key constraints)
    if(paid){
        // only paid aliases get deleted from the emails table
        DSTR_STATIC(q3, "DELETE FROM emails WHERE email=?");
        PROP(&e, sql_bound_stmt(sql, &q3, string_bind_in(alias)) );
    }

    *deleted = true;

    return e;
}

derr_t delete_alias(
    MYSQL *sql, const dstr_t *uuid, const dstr_t *alias, bool *deleted
){
    derr_t e = E_OK;
    *deleted = false;

    PROP(&e, sql_txn_start(sql) );

    PROP_GO(&e, _delete_alias_txn(sql, uuid, alias, deleted), hard_fail);

    PROP(&e, sql_txn_commit(sql) );

    return e;

hard_fail:
    sql_txn_abort(sql);
    *deleted = false;

    return e;
}

// devices

//// will these ever become useful?
// derr_t smsql_device_new(
//     smsql_device_t **out, const dstr_t *public_key, const dstr_t *fingerprint
// ){
//     derr_t e = E_OK;
//     *out = NULL;
//
//     smsql_device_t *device = DMALLOC_STRUCT_PTR(&e, device);
//     CHECK(&e);
//
//     link_init(&device->link);
//
//     PROP_GO(&e, dstr_copy(public_key, &device->public_key), fail);
//     PROP_GO(&e, dstr_copy(fingerprint, &device->fingerprint), fail_key);
//
//     *out = device;
//     return e;
//
// fail_key:
//     dstr_free(&device->public_key);
// fail:
//     free(device);
//     return e;
// }
//
// void smsql_device_free(smsql_device_t **old){
//     smsql_device_t *device = *old;
//     if(device == NULL) return;
//     dstr_free(&device->fingerprint);
//     dstr_free(&device->public_key);
//     free(device);
//     *old = NULL;
// }

derr_t smsql_dstr_new(smsql_dstr_t **out, const dstr_t *val){
    derr_t e = E_OK;
    *out = NULL;

    smsql_dstr_t *dstr = DMALLOC_STRUCT_PTR(&e, dstr);
    CHECK(&e);

    link_init(&dstr->link);

    PROP_GO(&e, dstr_copy(val, &dstr->dstr), fail);

    *out = dstr;
    return e;

fail:
    free(dstr);
    return e;
}

void smsql_dstr_free(smsql_dstr_t **old){
    smsql_dstr_t *dstr = *old;
    if(dstr == NULL) return;
    dstr_free(&dstr->dstr);
    free(dstr);
    *old = NULL;
}

derr_t list_device_fprs(MYSQL *sql, const dstr_t *uuid, link_t *out){
    derr_t e = E_OK;

    MYSQL_STMT *stmt;

    DSTR_VAR(fpr_res, SMSQL_FPR_SIZE);

    DSTR_STATIC(
        q1, "SELECT fingerprint from devices where user_uuid=?"
    );
    PROP(&e,
        sql_multirow_stmt(
            sql, &stmt, &q1,
            // parameters
            blob_bind_in(uuid),
            // results
            string_bind_out(&fpr_res)
        )
    );

    link_t list;
    link_init(&list);
    link_t *link;

    while(true){
        bool ok;
        PROP_GO(&e, sql_stmt_fetch(stmt, &ok), fail_list);
        if(!ok) break;

        smsql_dstr_t *dstr;
        PROP_GO(&e,
            smsql_dstr_new(&dstr, &fpr_res),
        loop_fail);

        link_list_append(&list, &dstr->link);

        continue;

    loop_fail:
        sql_stmt_fetchall(stmt);
        goto fail_list;
    }

    // set the output
    link_list_append_list(out, &list);

    mysql_stmt_close(stmt);

    return e;

fail_list:
    while((link = link_list_pop_first(&list))){
        smsql_dstr_t *dstr = CONTAINER_OF(link, smsql_dstr_t, link);
        smsql_dstr_free(&dstr);
    }
    mysql_stmt_close(stmt);
    return e;
}

derr_t list_device_keys(MYSQL *sql, const dstr_t *uuid, link_t *out){
    derr_t e = E_OK;

    MYSQL_STMT *stmt;

    DSTR_VAR(public_key_res, SMSQL_PUBKEY_SIZE);

    DSTR_STATIC(
        q1, "SELECT public_key from devices where user_uuid=?"
    );
    PROP(&e,
        sql_multirow_stmt(
            sql, &stmt, &q1,
            // parameters
            blob_bind_in(uuid),
            // results
            string_bind_out(&public_key_res)
        )
    );

    link_t list;
    link_init(&list);
    link_t *link;

    while(true){
        bool ok;
        PROP_GO(&e, sql_stmt_fetch(stmt, &ok), fail_list);
        if(!ok) break;

        smsql_dstr_t *dstr;
        PROP_GO(&e,
            smsql_dstr_new(&dstr, &public_key_res),
        loop_fail);

        link_list_append(&list, &dstr->link);

        continue;

    loop_fail:
        sql_stmt_fetchall(stmt);
        goto fail_list;
    }

    // set the output
    link_list_append_list(out, &list);

    mysql_stmt_close(stmt);

    return e;

fail_list:
    while((link = link_list_pop_first(&list))){
        smsql_dstr_t *dstr = CONTAINER_OF(link, smsql_dstr_t, link);
        smsql_dstr_free(&dstr);
    }
    mysql_stmt_close(stmt);
    return e;
}

static derr_t _add_device_locked(
    MYSQL *sql,
    const dstr_t *uuid,
    const dstr_t *pubkey,
    const dstr_t *fpr_hex,
    bool *ok
){
    derr_t e = E_OK;

    // not vulnerable to write skew since we have a write lock on the table
    uint64_t count;
    DSTR_STATIC(q1, "select COUNT(*) from devices where user_uuid=?;");
    PROP(&e,
        sql_onerow_query(
            sql, &q1, NULL, blob_bind_in(uuid), uint64_bind_out(&count)
        )
    );

    if(count >= MAX_DEVICES){
        *ok = false;
        return e;
    }

    DSTR_STATIC(
        q2,
        "INSERT INTO devices (user_uuid, public_key, fingerprint) "
        "VALUES (?, ?, ?)"
    );
    PROP(&e,
        sql_bound_stmt(
            sql, &q2,
            blob_bind_in(uuid),
            string_bind_in(pubkey),
            string_bind_in(fpr_hex)
        )
    );

    *ok = true;

    return e;
}

// validate, get fingerprint, and normalize a pem-encoded public key
static derr_t _validate_for_add_device(
    const dstr_t *pubkey, dstr_t *fpr_hex, dstr_t *norm
){
    derr_t e = E_OK;
    // validate pkey
    EVP_PKEY *pkey;
    PROP(&e, read_pem_encoded_pubkey(pubkey, &pkey) );

    // get fingerprint
    DSTR_VAR(fpr, SMSQL_FPR_SIZE / 2);
    PROP_GO(&e, get_fingerprint(pkey, &fpr), cu);

    // hexify
    PROP_GO(&e, bin2hex(&fpr, fpr_hex), cu);

    // normalize the public key
    PROP_GO(&e, get_public_pem(pkey, norm), cu);

cu:
    EVP_PKEY_free(pkey);
    return e;
}

// take a PEM-encoded public key, validate it, and add it to an account
derr_t add_device(
    MYSQL *sql, const dstr_t *uuid, const dstr_t *pubkey, bool *ok
){
    derr_t e = E_OK;
    *ok = false;

    DSTR_VAR(fpr_hex, SMSQL_FPR_SIZE);
    DSTR_VAR(norm, SMSQL_PUBKEY_SIZE);
    PROP(&e, _validate_for_add_device(pubkey, &fpr_hex, &norm) );

    // now do the locking
    DSTR_STATIC(q1, "LOCK TABLES devices WRITE");
    PROP(&e, sql_lock_statement(sql, &q1) );

    PROP_GO(&e,
        _add_device_locked(sql, uuid, pubkey, &fpr_hex, ok),
    fail_lock);

    PROP(&e, sql_unlock_all(sql) );

    return e;

fail_lock:
    sql_abort_locks(sql);
    return e;
}

derr_t delete_device(MYSQL *sql, const dstr_t *uuid, const dstr_t *fpr_hex){
    derr_t e = E_OK;

    DSTR_STATIC(q1, "DELETE FROM devices WHERE user_uuid=? AND fingerprint=?");
    PROP(&e,
        sql_bound_stmt(sql, &q1, blob_bind_in(uuid), string_bind_in(fpr_hex))
    );

    return e;
}

// tokens

derr_t smsql_uint_new(smsql_uint_t **out, unsigned int val){
    derr_t e = E_OK;
    *out = NULL;

    smsql_uint_t *uint = DMALLOC_STRUCT_PTR(&e, uint);
    CHECK(&e);

    link_init(&uint->link);
    uint->uint = val;

    *out = uint;

    return e;
}

void smsql_uint_free(smsql_uint_t **old){
    smsql_uint_t *uint = *old;
    if(uint == NULL) return;
    free(uint);
    *old = NULL;
}

derr_t list_tokens(MYSQL *sql, const dstr_t *uuid, link_t *out){
    derr_t e = E_OK;

    MYSQL_STMT *stmt;

    unsigned int token;

    DSTR_STATIC(
        q1, "SELECT token from tokens where user_uuid=?"
    );
    PROP(&e,
        sql_multirow_stmt(
            sql, &stmt, &q1,
            // parameters
            blob_bind_in(uuid),
            // results
            uint_bind_out(&token)
        )
    );

    link_t list;
    link_init(&list);
    link_t *link;

    while(true){
        bool ok;
        PROP_GO(&e, sql_stmt_fetch(stmt, &ok), fail_list);
        if(!ok) break;

        smsql_uint_t *uint;
        PROP_GO(&e,
            smsql_uint_new(&uint, token),
        loop_fail);

        link_list_append(&list, &uint->link);

        continue;

    loop_fail:
        sql_stmt_fetchall(stmt);
        goto fail_list;
    }

    // set the output
    link_list_append_list(out, &list);

    mysql_stmt_close(stmt);

    return e;

fail_list:
    while((link = link_list_pop_first(&list))){
        smsql_uint_t *uint = CONTAINER_OF(link, smsql_uint_t, link);
        smsql_uint_free(&uint);
    }
    mysql_stmt_close(stmt);
    return e;
}

static derr_t new_api_secret(dstr_t *secret){
    derr_t e = E_OK;

    DSTR_VAR(secret_bytes, 33);
    PROP(&e, random_bytes(&secret_bytes, secret_bytes.size) );

    PROP(&e, bin2b64(&secret_bytes, secret) );

    return e;
}

derr_t add_token(
    MYSQL *sql, const dstr_t *uuid, unsigned int *token, dstr_t *secret
){
    derr_t e = E_OK;
    *token = 0;

    // we only need one secret for all insertion tries
    DSTR_VAR(secret_temp, SMSQL_APISECRET_SIZE);
    PROP(&e, new_api_secret(&secret_temp) );

    for(size_t limit = 0; limit < 1000; limit++){
        unsigned int token_temp;
        PROP(&e, random_uint(&token_temp) );

        DSTR_STATIC(
            q1,
            "INSERT INTO tokens (user_uuid, token, secret) VALUES (?, ?, ?)"
        );
        derr_t e2 = sql_bound_stmt(
            sql, &q1,
            blob_bind_in(uuid),
            uint_bind_in(&token_temp),
            string_bind_in(&secret_temp)
        );
        CATCH(e2, E_SQL_DUP){
            // chose a duplicate token, try again
            DROP_VAR(&e2);
            continue;
        }else PROP(&e, e2);

        PROP(&e, dstr_append(secret, &secret_temp) );
        *token = token_temp;
        return e;
    }

    ORIG(&e, E_INTERNAL, "failed to find an available token");
}


derr_t delete_token(MYSQL *sql, const dstr_t *uuid, unsigned int token){
    derr_t e = E_OK;

    DSTR_STATIC(q1, "DELETE FROM tokens WHERE user_uuid=? AND token=?");
    PROP(&e,
        sql_bound_stmt(sql, &q1, blob_bind_in(uuid), uint_bind_in(&token))
    );

    return e;
}
