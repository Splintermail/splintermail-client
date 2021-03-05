#include <stdlib.h>

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

// transaction layer
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
