#define _GNU_SOURCE  // for crypt_r
#include <unistd.h>
#include <crypt.h>

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

// basically just 8 random bytes
derr_t random_password_salt(dstr_t *salt){
    derr_t e = E_OK;
    PROP(&e, random_bytes(salt, SMSQL_PASSWORD_SALT_SIZE) );
    return e;
}

// sha512 password hash
// silently truncates passwords > 128 bytes before hashing
derr_t hash_password(
    const dstr_t *pass, unsigned int rounds, const dstr_t *salt, dstr_t *hash
){
    derr_t e = E_OK;

    // we need a null-terminated password
    DSTR_VAR(nt_pass, 128);
    // a too-long password can't be valid, so we'll just silently truncate it
    dstr_t sub = dstr_sub2(*pass, 0, nt_pass.size - 1);
    PROP(&e, dstr_append(&nt_pass, &sub) );
    PROP(&e, dstr_null_terminate(&nt_pass) );

    /* crypt_gensalt() seems to behave in a strange way with regard to the
       provided salt, and it's not part of any standard anyways, so we'll just
       write the crypt() settings string ourselves */
    DSTR_VAR(settings, 128);
    PROP(&e, FMT(&settings, "$6$rounds=%x$%x$", FU(rounds), FX(salt)) );

    /* `man 3 crypt` says that you the data struct is probably too big to
       allocate on the stack.  crypt_ra() will allocate on the heap for
       you but it is not available on debian, so we use crypt_r() instead. */
    struct crypt_data *mem = DMALLOC_STRUCT_PTR(&e, mem);
    CHECK(&e);

    /* `man 3 crypt` indicates that crypt() and friends return some strange,
       ancient return values, and you should basically always check errno. */
    errno = 0;
    char *result = crypt_r(nt_pass.data, settings.data, mem);
    if(errno != 0){
        TRACE(&e, "crypt failed: %x\n", FE(&errno));
        ORIG_GO(&e, errno == ENOMEM ? E_NOMEM : E_OS, "crypt failed", cu);
    }

    PROP_GO(&e, FMT(hash, "%x", FS(result)), cu);

cu:
    free(mem);

    return e;
}

// raises E_PARAM if the hash is malformed or otherwise not parsable
// any outputs are allowed to be NULL without issue
static derr_t _parse_hash(
    const dstr_t *hash, unsigned int *rounds, dstr_t *salt, dstr_t *hash_result
){
    derr_t e = E_OK;

    // example sha512 hash: $6$rounds=NN$hexsaltchars$passhashchars...
    //                         ^^^^^^^^^^optional

    if(!dstr_beginswith(hash, &DSTR_LIT("$6$"))){
        ORIG(&e, E_PARAM, "hash is not a SHA512 hash, should start with $6$");
    }

    // split fields
    LIST_VAR(dstr_t, fields, 5);
    derr_t e2 = dstr_split(hash, &DSTR_LIT("$"), &fields);
    CATCH(e2, E_FIXEDSIZE){
        // too many fields, invalid hash
        RETHROW(&e, &e2, E_PARAM);
    } else PROP_VAR(&e, &e2);

    if(fields.len != 4 && fields.len != 5){
        ORIG(&e, E_PARAM, "hash is not a SHA512 hash, wrong number of fields");
    }

    dstr_t hex_salt;
    dstr_t hash_result_temp;

    if(fields.len == 4){
        hex_salt = fields.data[2];
        hash_result_temp = fields.data[3];
        // default for algorithm
        if(rounds) *rounds = 5000;
    }else{
        hex_salt = fields.data[3];
        hash_result_temp = fields.data[4];
        // parse rounds field
        const dstr_t rounds_field = fields.data[2];
        DSTR_STATIC(prefix, "rounds=");
        if(!dstr_beginswith(&rounds_field, &prefix)){
            ORIG(&e, E_PARAM, "hash has invalid 'rounds' field");
        }
        // get the numeric text for rounds
        const dstr_t rounds_val = dstr_sub2(
            rounds_field, prefix.len, rounds_field.len
        );
        unsigned int temp_rounds;
        // parse the numeric text, which might raise E_PARAM for us
        PROP(&e, dstr_tou(&rounds_val, &temp_rounds, 10) );
        if(rounds) *rounds = temp_rounds;
    }

    if(salt) PROP(&e, hex2bin(&hex_salt, salt) );
    if(hash_result) PROP(&e, dstr_append(hash_result, &hash_result_temp) );

    return e;
}

// just the hash validation
derr_t validate_password_hash(
    const dstr_t *pass, const dstr_t *true_hash, bool *ok
){
    derr_t e = E_OK;
    *ok = false;

    unsigned int true_rounds;
    DSTR_VAR(true_salt, SMSQL_PASSWORD_HASH_SIZE);
    DSTR_VAR(true_hash_result, SMSQL_PASSWORD_HASH_SIZE);

    // the true hash is from our system and must be valid
    NOFAIL(&e, E_ANY,
        _parse_hash(true_hash, &true_rounds, &true_salt, &true_hash_result)
    );

    // raises E_USERPARAM on invalid password, which is a user input
    /* this will return too fast on invalid passwords, but that's ok... "your
       password is not even valid" is not information useful to a timing
       attack, especially when the validator is open-source */
    DSTR_VAR(hash, SMSQL_PASSWORD_HASH_SIZE);
    PROP(&e, hash_password(pass, true_rounds, &true_salt, &hash) );

    DSTR_VAR(hash_result, SMSQL_PASSWORD_HASH_SIZE);
    for(size_t i = 0; i < SMSQL_PASSWORD_HASH_SIZE; i++){
        hash_result.data[i] = '\0';
    }

    // this should obviously be a valid hash since we just generated it
    NOFAIL(&e, E_ANY, _parse_hash(&hash, NULL, NULL, &hash_result));

    *ok = dstr_eq_consttime(&hash_result, &true_hash_result);

    return e;
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
        ORIG(&e, E_USERMSG, "email too long");

    // ends in @splintermail.com
    DSTR_STATIC(suffix, "@splintermail.com");
    if(!dstr_endswith(email, &suffix))
        ORIG(&e, E_USERMSG, "email must end in @splintermail.com");

    const dstr_t username = dstr_sub2(*email, 0, email->len - suffix.len);

    // non-empty username
    if(username.len == 0)
        ORIG(&e, E_USERMSG, "empty username");

    // valid username
    if(!valid_username_chars(&username))
        ORIG(&e, E_USERMSG, "invalid characters in email");

    return e;
}

bool valid_password_chars(const dstr_t *pass){
    for(size_t i = 0; i < pass->len; i++){
        char c = pass->data[i];
        if(c >= 'a' && c <= 'z') continue;
        if(c >= 'A' && c <= 'Z') continue;
        if(c >= '0' && c <= '9') continue;
        switch(c){
            case ' ': case '`': case '~': case '!': case '@': case '#':
            case '$': case '%': case '^': case '&': case '*': case '(':
            case ')': case '-': case '=': case '_': case '+': case '[':
            case ']': case '{': case '}': case '|': case ';': case '\\':
            case ':': case '"': case ',': case '.': case '/': case '\'':
            case '<': case '>': case '?':
                continue;
            default:
                return false;
        }
    }
    return true;
}

derr_t valid_splintermail_password(const dstr_t *pass){
    derr_t e = E_OK;

    if(pass->len > SMSQL_PASSWORD_SIZE)
        ORIG(
            &e, E_USERMSG, "password must not exceed 72 characters in length"
        );

    if(pass->len < 16)
        ORIG(
            &e, E_USERMSG, "password must be at least 16 characters in length"
        );

    if(!valid_password_chars(pass))
        ORIG(&e, E_USERMSG, "invalid characters in password");

    if(pass->data[0] == ' ' || pass->data[pass->len-1] == ' '){
        ORIG(&e, E_USERMSG, "no leading or trailing spaces in password");
    }

    return e;
}

// predefined queries

derr_t get_uuid_for_email(
    MYSQL *sql, const dstr_t *email, dstr_t *uuid, bool *ok
){
    derr_t e = E_OK;
    *ok = false;
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
    MYSQL *sql, const dstr_t *uuid, dstr_t *alias
){
    derr_t e = E_OK;

    /* due to the FOR UPDATE this is not vulnerable to write skew, but it could
       be a relatively long transaction, so it might be better to allow write
       skew and just let users have a couple free aliasas once in a while */
    unsigned int count;
    DSTR_STATIC(
        q1,
        "SELECT num_random_aliases FROM accounts WHERE user_uuid=? FOR UPDATE;"
    );
    PROP(&e,
        sql_onerow_query(
            sql, &q1, NULL, blob_bind_in(uuid), uint_bind_out(&count)
        )
    );

    if(count >= MAX_RANDOM_ALIASES){
        ORIG(&e, E_USERMSG, "too many aliases already");
    }

    // try for an unused random alias
    for(size_t limit = 0; limit < 1000; limit++){
        DSTR_VAR(temp, SMSQL_EMAIL_SIZE);
        PROP(&e, petname_email(&temp) );

        // double-check that the email is valid
        NOFAIL(&e, E_USERMSG, valid_splintermail_email(&temp) );

        DSTR_STATIC(q2, "INSERT INTO emails (email) VALUES (?)");
        derr_t e2 = sql_norow_query(sql, &q2, NULL, string_bind_in(&temp));
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
            sql_norow_query(sql,
                &q3,
                NULL,
                string_bind_in(&temp),
                bool_bind_in(&paid),
                blob_bind_in(uuid),
            )
        );

        // increment num_paid_aliases in accounts
        count++;
        DSTR_STATIC(
            q4,
            "UPDATE accounts SET num_random_aliases=? WHERE user_uuid=?;"
        );
        PROP(&e,
            sql_norow_query(
                sql, &q4, NULL, uint_bind_in(&count), blob_bind_in(uuid)
            )
        );

        PROP(&e, dstr_append(alias, &temp) );

        return e;
    }

    ORIG(&e, E_INTERNAL, "failed to find an available alias");
}

// throws E_USERMSG if max aliases reached
derr_t add_random_alias(MYSQL *sql, const dstr_t *uuid, dstr_t *alias){
    derr_t e = E_OK;

    PROP(&e, sql_txn_start(sql) );

    PROP_GO(&e, _add_random_alias_txn(sql, uuid, alias), hard_fail);

    PROP(&e, sql_txn_commit(sql) );

    return e;

hard_fail:
    sql_txn_abort(sql);

    return e;
}


static derr_t _add_primary_alias_txn(
    MYSQL *sql, const dstr_t *uuid, const dstr_t *alias
){
    derr_t e = E_OK;

    DSTR_STATIC(q1, "INSERT INTO emails (email) VALUES (?)");
    PROP(&e, sql_norow_query(sql, &q1, NULL, string_bind_in(alias)) );

    DSTR_STATIC(q2,
        "INSERT INTO aliases (alias, paid, user_uuid) VALUES (?, ?, ?)"
    );
    bool paid = true;
    derr_t e2 = sql_norow_query(sql,
        &q2,
        NULL,
        string_bind_in(alias),
        bool_bind_in(&paid),
        blob_bind_in(uuid),
    );
    CATCH(e2, E_SQL_DUP){
        DROP_VAR(&e2);
        ORIG(&e, E_USERMSG, "alias not available");
    }else PROP_VAR(&e, &e2);

    return e;
}

// throws E_USERMSG if alias is unavailable
derr_t add_primary_alias(MYSQL *sql, const dstr_t *uuid, const dstr_t *alias){
    derr_t e = E_OK;

    PROP(&e, valid_splintermail_email(alias) );

    PROP(&e, sql_txn_start(sql) );

    PROP_GO(&e, _add_primary_alias_txn(sql, uuid, alias), hard_fail);

    PROP(&e, sql_txn_commit(sql) );

    return e;

hard_fail:
    sql_txn_abort(sql);

    return e;
}

static derr_t _delete_alias_txn(
    MYSQL *sql, const dstr_t *uuid, const dstr_t *alias
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
        ORIG(&e, E_USERMSG, "no such alias");
    }

    DSTR_STATIC(q2, "DELETE FROM aliases WHERE alias=?");
    PROP(&e, sql_norow_query(sql, &q2, NULL, string_bind_in(alias)) );

    // delete from emails table last (for foriegn key constraints)
    if(paid){
        // only paid aliases get deleted from the emails table
        DSTR_STATIC(q3, "DELETE FROM emails WHERE email=?");
        PROP(&e, sql_norow_query(sql, &q3, NULL, string_bind_in(alias)) );
    }

    return e;
}

derr_t delete_alias(MYSQL *sql, const dstr_t *uuid, const dstr_t *alias){
    derr_t e = E_OK;

    PROP(&e, sql_txn_start(sql) );

    PROP_GO(&e, _delete_alias_txn(sql, uuid, alias), hard_fail);

    PROP(&e, sql_txn_commit(sql) );

    return e;

hard_fail:
    sql_txn_abort(sql);

    return e;
}

static derr_t _delete_all_aliases_txn(MYSQL *sql, const dstr_t *uuid){
    derr_t e = E_OK;

    link_t *link;

    link_t aliases;
    link_init(&aliases);

    // list all aliases
    PROP(&e, list_aliases(sql, uuid, &aliases) );

    while(!link_list_isempty(&aliases)){
        smsql_alias_t *alias = CONTAINER_OF(aliases.next, smsql_alias_t, link);

        // delete all aliases, paid or free
        DSTR_STATIC(q1, "DELETE FROM aliases WHERE alias=?");
        PROP_GO(&e,
            sql_norow_query(sql, &q1, NULL, string_bind_in(&alias->alias)),
        cu);

        if(alias->paid){
            // paid aliases are also deleted from the emails table
            DSTR_STATIC(q2, "DELETE FROM emails WHERE email=?");
            PROP_GO(&e,
                sql_norow_query(sql, &q2, NULL, string_bind_in(&alias->alias)),
            cu);
        }

        link_list_pop_first(&aliases);
        smsql_alias_free(&alias);
    }

cu:
    while((link = link_list_pop_first(&aliases))){
        smsql_alias_t *alias = CONTAINER_OF(link, smsql_alias_t, link);
        smsql_alias_free(&alias);
    }

    return e;
}

derr_t delete_all_aliases(MYSQL *sql, const dstr_t *uuid){
    derr_t e = E_OK;

    PROP(&e, sql_txn_start(sql) );

    PROP_GO(&e, _delete_all_aliases_txn(sql, uuid), hard_fail);

    PROP(&e, sql_txn_commit(sql) );

    return e;

hard_fail:
    sql_txn_abort(sql);

    return e;
}

// devices

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

static derr_t _add_device_txn(
    MYSQL *sql,
    const dstr_t *uuid,
    const dstr_t *pubkey,
    const dstr_t *fpr_hex
){
    derr_t e = E_OK;

    // not vulnerable to write skew since we use FOR UPDATE
    uint64_t count;
    DSTR_STATIC(
        q1, "SELECT COUNT(*) FROM devices WHERE user_uuid=? FOR UPDATE;"
    );
    PROP(&e,
        sql_onerow_query(
            sql, &q1, NULL, blob_bind_in(uuid), uint64_bind_out(&count)
        )
    );

    if(count >= MAX_DEVICES){
        ORIG(&e, E_USERMSG, "max devices already reached");
    }

    DSTR_STATIC(
        q2,
        "INSERT INTO devices (user_uuid, public_key, fingerprint) "
        "VALUES (?, ?, ?)"
    );
    derr_t e2 = sql_norow_query(
        sql, &q2, NULL,
        blob_bind_in(uuid),
        string_bind_in(pubkey),
        string_bind_in(fpr_hex)
    );
    CATCH(e2, E_SQL_DUP){
        DROP_VAR(&e);
        ORIG(&e, E_USERMSG, "duplicate public key");
    }else PROP_VAR(&e, &e2);

    return e;
}

// validate, get fingerprint, and normalize a pem-encoded public key
// raises E_USERMSG on failure
static derr_t _validate_for_add_device(
    const dstr_t *pubkey, dstr_t *fpr_hex, dstr_t *norm
){
    derr_t e = E_OK;

    // validate pkey
    EVP_PKEY *pkey;
    derr_t e2 = read_pem_encoded_pubkey(pubkey, &pkey);
    CATCH(e2, E_PARAM){
        DROP_VAR(&e);
        ORIG(&e, E_USERMSG, "invalid public key");
    }else PROP_VAR(&e, &e2);

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
// raises E_USERMSG on failure
derr_t add_device(
    MYSQL *sql, const dstr_t *uuid, const dstr_t *pubkey, dstr_t *fpr
){
    derr_t e = E_OK;

    DSTR_VAR(fpr_hex, SMSQL_FPR_SIZE);
    DSTR_VAR(norm, SMSQL_PUBKEY_SIZE);
    PROP(&e, _validate_for_add_device(pubkey, &fpr_hex, &norm) );

    PROP(&e, sql_txn_start(sql) );

    PROP_GO(&e,
        _add_device_txn(sql, uuid, &norm, &fpr_hex),
    hard_fail);

    PROP_GO(&e, dstr_append(fpr, &fpr_hex), hard_fail);

    PROP(&e, sql_txn_commit(sql) );

    return e;

hard_fail:
    sql_txn_abort(sql);

    return e;
}

derr_t delete_device(MYSQL *sql, const dstr_t *uuid, const dstr_t *fpr_hex){
    derr_t e = E_OK;

    size_t affected;
    DSTR_STATIC(q1, "DELETE FROM devices WHERE user_uuid=? AND fingerprint=?");
    PROP(&e,
        sql_norow_query(
            sql, &q1, &affected, blob_bind_in(uuid), string_bind_in(fpr_hex)
        )
    );

    if(affected == 0){
        ORIG(&e, E_USERMSG, "no such device");
    }

    return e;
}

// tokens

derr_t smsql_uint_new(smsql_uint_t **out, uint32_t val){
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

    uint32_t token;

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
    MYSQL *sql, const dstr_t *uuid, uint32_t *token, dstr_t *secret
){
    derr_t e = E_OK;
    *token = 0;

    // we only need one secret for all insertion tries
    DSTR_VAR(secret_temp, SMSQL_APISECRET_SIZE);
    PROP(&e, new_api_secret(&secret_temp) );

    for(size_t limit = 0; limit < 1000; limit++){
        uint32_t token_temp;
        PROP(&e, random_uint(&token_temp) );

        DSTR_STATIC(
            q1,
            "INSERT INTO tokens (user_uuid, token, secret) VALUES (?, ?, ?)"
        );
        derr_t e2 = sql_norow_query(
            sql, &q1, NULL,
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


derr_t delete_token(MYSQL *sql, const dstr_t *uuid, uint32_t token){
    derr_t e = E_OK;

    size_t affected;
    DSTR_STATIC(q1, "DELETE FROM tokens WHERE user_uuid=? AND token=?");
    PROP(&e,
        sql_norow_query(
            sql, &q1, &affected, blob_bind_in(uuid), uint_bind_in(&token)
        )
    );

    if(affected == 0){
        ORIG(&e, E_USERMSG, "no such token");
    }

    return e;
}

// misc

static derr_t _create_account_txn(
    MYSQL *sql,
    const dstr_t *email,
    const dstr_t *pass_hash,
    const dstr_t *uuid
){
    derr_t e = E_OK;

    DSTR_STATIC(q1, "INSERT INTO emails (email) VALUES (?)");
    derr_t e2 = sql_norow_query(sql, &q1, NULL, string_bind_in(email));
    CATCH(e2, E_SQL_DUP){
        // duplicate email
        DROP_VAR(&e2);
        ORIG(&e, E_USERMSG, "username not available");
    }else PROP_VAR(&e, &e2);

    // domain_id is hardcoded to 1="splintermail.com"
    DSTR_STATIC(
        q2,
        "INSERT INTO accounts (email, password, user_uuid, domain_id)"
        "VALUES (?, ?, ?, 1)"
    );
    PROP(&e,
        sql_norow_query(
            sql, &q2, NULL,
            string_bind_in(email),
            string_bind_in(pass_hash),
            blob_bind_in(uuid)
        )
    );

    return e;
}

// throws E_USERMSG if email is taken
derr_t create_account(
    MYSQL *sql,
    const dstr_t *email,
    const dstr_t *pass,
    dstr_t *uuid
){
    derr_t e = E_OK;

    // validate inputs
    PROP(&e, valid_splintermail_email(email) );
    PROP(&e, valid_splintermail_password(pass) );

    DSTR_VAR(salt, SMSQL_PASSWORD_SALT_SIZE);
    PROP(&e, random_password_salt(&salt) );

    DSTR_VAR(hash, SMSQL_PASSWORD_HASH_SIZE);
    PROP(&e, hash_password(pass, SMSQL_PASSWORD_SHA512_ROUNDS, &salt, &hash) );

    // set the uuid bytes first, to ensure we don't fail after the txn
    PROP(&e, random_bytes(uuid, SMSQL_UUID_SIZE) );

    PROP(&e, sql_txn_start(sql) );

    PROP_GO(&e, _create_account_txn(sql, email, &hash, uuid), hard_fail);

    PROP(&e, sql_txn_commit(sql) );

    return e;

hard_fail:
    sql_txn_abort(sql);

    return e;
}

static derr_t _delete_account_txn(
    MYSQL *sql, const dstr_t *uuid, const dstr_t *email
){
    derr_t e = E_OK;

    // delete all the users's aliases, deleting paid aliases from emails table
    PROP(&e, _delete_all_aliases_txn(sql, uuid) );

    {
        DSTR_STATIC(q, "DELETE FROM tokens WHERE user_uuid = ?");
        PROP(&e, sql_norow_query(sql, &q, NULL, blob_bind_in(uuid)) );
    }

    {
        DSTR_STATIC(q, "DELETE FROM devices WHERE user_uuid = ?");
        PROP(&e, sql_norow_query(sql, &q, NULL, blob_bind_in(uuid)) );
    }

    {
        DSTR_STATIC(q, "DELETE FROM accounts WHERE user_uuid = ?");
        PROP(&e, sql_norow_query(sql, &q, NULL, blob_bind_in(uuid)) );
    }

    {
        DSTR_STATIC(q, "DELETE FROM emails WHERE email = ?");
        PROP(&e, sql_norow_query(sql, &q, NULL, string_bind_in(email)) );
    }

    return e;
}

// gateway is responsible for ensuring a password is provided
derr_t delete_account(MYSQL *sql, const dstr_t *uuid){
    derr_t e = E_OK;

    // start by getting this uuid's email
    DSTR_VAR(email, SMSQL_EMAIL_SIZE);
    bool ok;
    PROP(&e, get_email_for_uuid(sql, uuid, &email, &ok) );
    if(!ok){
        // nothing to delete; more likely to be a bug than a race condition
        ORIG(&e, E_INTERNAL, "no account matches uuid!");
    }

    PROP(&e, sql_txn_start(sql) );

    PROP_GO(&e, _delete_account_txn(sql, uuid, &email), hard_fail);

    PROP(&e, sql_txn_commit(sql) );

    return e;

hard_fail:
    sql_txn_abort(sql);

    return e;
}

derr_t account_info(
    MYSQL *sql,
    const dstr_t *uuid,
    size_t *num_devices,
    size_t *num_primary_aliases,
    size_t *num_random_aliases
){
    derr_t e = E_OK;

    /* count the devices table in a sub-query, but count aliases in a single
       pass through the aliases table.

       see stackoverflow.com/a/12789493/4951379
       and stackoverflow.com/a/12789441/4951379 */
    DSTR_STATIC(q1,
        "SELECT"
        "    (select COUNT(*) from devices where user_uuid=?) AS devices,"
        "    sum(case when paid=true then 1 else 0 end) AS paids,"
        "    sum(case when paid=false then 1 else 0 end) AS frees "
        "FROM ("
        "    select paid from aliases where user_uuid=?"
        ") as x"
    );
    PROP(&e,
        sql_onerow_query(
            sql, &q1, NULL,
            // params
            blob_bind_in(uuid),
            blob_bind_in(uuid),
            // results
            uint64_bind_out(num_devices),
            uint64_bind_out(num_primary_aliases),
            uint64_bind_out(num_random_aliases)
        )
    );

    return e;
}

// validate a password for a user against the database
derr_t validate_user_password(
    MYSQL *sql, const dstr_t *uuid, const dstr_t *pass, bool *ok
){
    derr_t e = E_OK;
    *ok = false;

    DSTR_VAR(hash, SMSQL_PASSWORD_HASH_SIZE);

    DSTR_STATIC(q1, "SELECT password FROM accounts WHERE user_uuid=?");
    PROP(&e,
        sql_onerow_query(
            sql, &q1, NULL,
            // param
            blob_bind_in(uuid),
            // result
            string_bind_out(&hash)
        )
    );

    PROP(&e, validate_password_hash(pass, &hash, ok) );

    return e;
}

// returns uuid or throws E_USERMSG on failure
derr_t validate_login(
    MYSQL *sql, const dstr_t *email, const dstr_t *pass, dstr_t *uuid
){
    derr_t e = E_OK;

    bool ok;
    PROP(&e, get_uuid_for_email(sql, email, uuid, &ok) );
    if(!ok){
        ORIG(&e, E_USERMSG, "bad credentials");
    }

    PROP(&e, validate_user_password(sql, uuid, pass, &ok) );
    if(!ok){
        ORIG(&e, E_USERMSG, "bad credentials");
    }

    return e;
}

static derr_t _validate_token_auth_txn(
    MYSQL *sql,
    uint32_t token,
    uint64_t nonce,
    const dstr_t *payload,
    const dstr_t *sig,
    dstr_t *uuid
){
    derr_t e = E_OK;

    // check token ID (and get secret/nonce/uuid)
    // use FOR UPDATE to prevent write skew on nonce (replay attacks):w
    DSTR_VAR(secret, SMSQL_APISECRET_SIZE);
    DSTR_VAR(temp_uuid, SMSQL_UUID_SIZE);
    uint64_t old_nonce;
    DSTR_STATIC(
        q1,
        "SELECT secret, user_uuid, nonce FROM tokens WHERE token=? FOR UPDATE"
    );
    bool ok;
    PROP(&e,
        sql_onerow_query(
            sql, &q1, &ok,
            // param
            uint_bind_in(&token),
            // result
            blob_bind_out(&secret),
            blob_bind_out(&temp_uuid),
            uint64_bind_out(&old_nonce)
        )
    );
    if(!ok){
        ORIG(&e, E_USERMSG, "token not recognized");
    }

    // check signature
    DSTR_VAR(true_sig, 256);
    PROP(&e, hmac(&secret, payload, &true_sig) );
    if(!dstr_eq_consttime(sig, &true_sig)){
        ORIG(&e, E_USERMSG, "invalid signature");
    }

    // check/update nonce
    if(nonce <= old_nonce){
        ORIG(&e, E_USERMSG, "incorrect nonce");
    }
    DSTR_STATIC(q2, "UPDATE tokens SET nonce=? WHERE token=?");
    PROP(&e,
        sql_norow_query(
            sql, &q2, NULL, uint64_bind_in(&nonce), uint_bind_in(&token)
        )
    );

    // valid!
    PROP(&e, dstr_append(uuid, &temp_uuid) );

    return e;
}

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
){
    derr_t e = E_OK;

    PROP(&e, sql_txn_start(sql) );

    PROP_GO(&e,
        _validate_token_auth_txn(sql, token, nonce, payload, sig, uuid),
    hard_fail);

    PROP(&e, sql_txn_commit(sql) );

    return e;

hard_fail:
    sql_txn_abort(sql);

    return e;
}

// the gateway should enforce a valid old password is provided first
// throws E_USERMSG on invalid password
derr_t change_password(MYSQL *sql, const dstr_t *uuid, const dstr_t *pass){
    derr_t e = E_OK;

    PROP(&e, valid_splintermail_password(pass) );

    DSTR_VAR(salt, SMSQL_PASSWORD_SALT_SIZE);
    PROP(&e, random_password_salt(&salt) );

    DSTR_VAR(hash, SMSQL_PASSWORD_HASH_SIZE);
    PROP(&e, hash_password(pass, SMSQL_PASSWORD_SHA512_ROUNDS, &salt, &hash) );

    DSTR_STATIC(q1, "UPDATE accounts SET password=? WHERE user_uuid=?");
    PROP(&e,
        sql_norow_query(
            sql, &q1, NULL,
            string_bind_out(&hash),
            blob_bind_in(uuid)
        )
    );

    return e;
}

// uses time() for the login and last_seen times
// this implies that you should always create a fresh session_id on login
derr_t add_session_auth(
    MYSQL *sql, int server_id, const dstr_t *session_id, const dstr_t *uuid
){
    derr_t e = E_OK;

    time_t now;
    PROP(&e, dtime(&now) );

    DSTR_STATIC(
        q1,
        "INSERT INTO sessions ("
        "   session_id, server_id, user_uuid, login, last_seen"
        ") VALUES (?, ?, ?, ?, ?)"
    );
    PROP(&e,
        sql_norow_query(
            sql, &q1, NULL,
            string_bind_in(session_id),
            int_bind_in(&server_id),
            blob_bind_in(uuid),
            int64_bind_in(&now),
            int64_bind_in(&now)
        )
    );

    return e;
}


/* _do_logout_txn is used both by _session_logout_txn and, in the case of a
   timeout, by _validate_session_auth, to ensure that a logout decision is
   final */
static derr_t _do_logout_txn(
    MYSQL *sql,
    int server_id,
    const dstr_t *session_id,
    const dstr_t *uuid,
    time_t now,
    bool saw_our_server_id
){
    derr_t e = E_OK;

    if(saw_our_server_id){
        // Update last_seen for our server_id
        DSTR_STATIC(
            q2,
            "UPDATE sessions SET void=b'1' WHERE session_id=? AND server_id=?"
        );
        PROP(&e,
            sql_norow_query(
                sql, &q2, NULL,
                string_bind_in(session_id),
                int_bind_in(&server_id)
            )
        );
    }else{
        // Create a new row with void=true for our server_id
        DSTR_STATIC(
            q2,
            "INSERT INTO sessions ("
            "   session_id, server_id, user_uuid, login, last_seen, void"
            ") VALUES (?, ?, ?, ?, ?, b'1')"
        );
        PROP(&e,
            sql_norow_query(
                sql, &q2, NULL,
                string_bind_in(session_id),
                int_bind_in(&server_id),
                blob_bind_in(uuid),
                int64_bind_in(&now),
                int64_bind_in(&now),
            )
        );
    }

    return e;
}


static derr_t _session_logout_txn(
    MYSQL *sql,
    int server_id,
    const dstr_t *session_id,
    time_t now
){
    derr_t e = E_OK;

    /* similar pattern to validation; an UPDATE isn't sufficient in case the
       logout is happening on a new server so we have to SELECT then either
       INSERT or UPDATE */
    DSTR_STATIC(
        q1,
        "SELECT user_uuid, void, server_id "
        "FROM sessions WHERE session_id=? FOR UPDATE"
    );

    // row fetch result values
    DSTR_VAR(uuid_res, SMSQL_UUID_SIZE);
    bool void_res = false;
    int server_id_res = 0;

    MYSQL_STMT *stmt;

    PROP(&e,
        sql_multirow_stmt(
            sql, &stmt, &q1,
            // parameters
            string_bind_in(session_id),
            // results
            blob_bind_out(&uuid_res),
            bool_bind_out(&void_res),
            int_bind_out(&server_id_res)
        )
    );

    // values calculated from multiple rows
    bool saw_our_server_id = false;
    bool session_voided = false;

    while(true){
        bool fetch_ok;
        PROP_GO(&e, sql_stmt_fetch(stmt, &fetch_ok), cu);
        if(!fetch_ok) break;
        LOG_ERROR("got row server=%x void=%x\n", FI(server_id_res), FB(void_res));

        if(server_id_res == server_id) saw_our_server_id = true;
        if(void_res) session_voided = true;
    }

    // noop for already-voided sessions
    if(session_voided) goto cu;

    PROP_GO(&e,
        _do_logout_txn(
            sql, server_id, session_id, &uuid_res, now, saw_our_server_id
        ),
    cu);

cu:
    mysql_stmt_close(stmt);

    return e;
}

derr_t session_logout(MYSQL *sql, int server_id, const dstr_t *session_id){
    derr_t e = E_OK;

    time_t now;
    PROP(&e, dtime(&now) );

    PROP(&e, sql_txn_start(sql) );

    PROP_GO(&e,
        _session_logout_txn(sql, server_id, session_id, now),
    hard_fail);

    PROP(&e, sql_txn_commit(sql) );

    return e;

hard_fail:
    sql_txn_abort(sql);

    return e;
}

static derr_t _validate_session_auth_txn(
    MYSQL *sql,
    int server_id,
    const dstr_t *session_id,
    time_t now,
    dstr_t *uuid
){
    derr_t e = E_OK;

    DSTR_STATIC(
        q1,
        "SELECT user_uuid, login, last_seen, void, server_id "
        "FROM sessions WHERE session_id=? FOR UPDATE"
    );

    // row fetch result values
    DSTR_VAR(uuid_res, SMSQL_UUID_SIZE);
    time_t login_res = 0;
    time_t last_seen_res = 0;
    bool void_res = false;
    int server_id_res = 0;

    MYSQL_STMT *stmt;

    PROP(&e,
        sql_multirow_stmt(
            sql, &stmt, &q1,
            // parameters
            string_bind_in(session_id),
            // results
            blob_bind_out(&uuid_res),
            int64_bind_out(&login_res),
            int64_bind_out(&last_seen_res),
            bool_bind_out(&void_res),
            int_bind_out(&server_id_res)
        )
    );

    // values calculated from multiple rows
    bool saw_any_row = false;
    bool saw_our_server_id = false;
    bool session_voided = false;
    time_t last_seen = 0;

    while(true){
        bool fetch_ok;
        PROP_GO(&e, sql_stmt_fetch(stmt, &fetch_ok), cu);
        if(!fetch_ok) break;

        saw_any_row = true;
        if(server_id_res == server_id) saw_our_server_id = true;
        if(void_res) session_voided = true;
        if(last_seen_res > last_seen) last_seen = last_seen_res;
    }

    // detect non-existent sessions
    if(!saw_any_row)
        ORIG_GO(&e, E_USERMSG, "not logged in", cu);

    // detect explicit logouts
    if(session_voided)
        ORIG_GO(&e, E_USERMSG, "not logged in", cu);

    /* Check timeouts.  In the timeout case, enforce that the decision is final
       by actually setting void=True in the database */
    if(
        now > login_res + SMSQL_SESSION_HARD_TIMEOUT
        || now > last_seen + SMSQL_SESSION_SOFT_TIMEOUT
    ){
        PROP_GO(&e,
            _do_logout_txn(
                sql, server_id, session_id, &uuid_res, now, saw_our_server_id
            ),
        cu);
        ORIG_GO(&e, E_USERMSG, "not logged in", cu);
    }

    if(saw_our_server_id){
        // Update last_seen for our server_id
        DSTR_STATIC(
            q2,
            "UPDATE sessions SET last_seen=? "
            "WHERE session_id=? AND server_id=?"
        );
        PROP_GO(&e,
            sql_norow_query(
                sql, &q2, NULL,
                int64_bind_in(&now),
                string_bind_in(session_id),
                int_bind_in(&server_id)
            ),
        cu);
    }else{
        // Create a new row with last_seen for our server_id
        DSTR_STATIC(
            q2,
            "INSERT INTO sessions ("
            "   session_id, server_id, user_uuid, login, last_seen"
            ") VALUES (?, ?, ?, ?, ?)"
        );
        PROP_GO(&e,
            sql_norow_query(
                sql, &q2, NULL,
                string_bind_in(session_id),
                int_bind_in(&server_id),
                blob_bind_in(&uuid_res),
                int64_bind_in(&login_res),
                int64_bind_in(&now)
            ),
        cu);
    }

    PROP_GO(&e, dstr_append(uuid, &uuid_res), cu);

cu:
    mysql_stmt_close(stmt);

    return e;
}

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
){
    derr_t e = E_OK;

    time_t now;
    PROP(&e, dtime(&now) );

    PROP(&e, sql_txn_start(sql) );

    PROP_GO(&e,
        _validate_session_auth_txn(sql, server_id, session_id, now, uuid),
    hard_fail);

    PROP(&e, sql_txn_commit(sql) );

    return e;

hard_fail:
    sql_txn_abort(sql);

    return e;
}

// new_csrf returns a token you can embed in a webpage
derr_t new_csrf(
    MYSQL *sql, int server_id, const dstr_t *session_id, dstr_t *csrf
){
    derr_t e = E_OK;

    time_t now;
    PROP(&e, dtime(&now) );

    DSTR_VAR(rando, SMSQL_CSRF_RANDOM_BYTES);
    PROP(&e, random_bytes(&rando, rando.size) );

    DSTR_VAR(temp, SMSQL_CSRF_SIZE);
    PROP(&e, bin2b64(&rando, &temp) );

    DSTR_STATIC(
        q1,
        "INSERT INTO csrf ("
        "    csrf_id, server_id, session_id, created"
        ") VALUES (?, ?, ?, ?)"
    );
    PROP(&e,
        sql_norow_query(
            sql, &q1, NULL,
            string_bind_in(&temp),
            int_bind_in(&server_id),
            string_bind_in(session_id),
            int64_bind_in(&now)
        )
    );

    PROP(&e, dstr_append(csrf, &temp) );

    return e;
}

// validate_csrf() just checks if the token was valid for this session
// throws E_USERMSG on bad tokens
derr_t validate_csrf(MYSQL *sql, const dstr_t *session_id, const dstr_t *csrf){
    derr_t e = E_OK;

    time_t now;
    PROP(&e, dtime(&now) );

    DSTR_STATIC(
        q1, "SELECT created FROM csrf WHERE csrf_id=? AND session_id=?"
    );
    bool ok;
    time_t created;
    PROP(&e,
        sql_onerow_query(
            sql, &q1, &ok,
            // params
            string_bind_in(csrf),
            string_bind_in(session_id),
            int64_bind_out(&created)
        )
    );

    if(!ok || now > created + SMSQL_CSRF_TIMEOUT)
        ORIG(&e, E_USERMSG, "bad csrf token");

    return e;
}

// returns true if uuid/address matches accounts.email or aliases.alias
derr_t user_owns_address(
    MYSQL *sql, const dstr_t *uuid, const dstr_t *address, bool *ok
){
    derr_t e = E_OK;
    *ok = false;

    DSTR_STATIC(q1,
        "SELECT 1 FROM accounts WHERE user_uuid=? AND email=? "
        "UNION "
        "SELECT 1 FROM aliases WHERE user_uuid=? AND alias=?"
    );

    int one;
    PROP(&e,
        sql_onerow_query(
            sql, &q1, ok,
            // params
            blob_bind_in(uuid),
            string_bind_in(address),
            blob_bind_in(uuid),
            string_bind_in(address),
            // results
            int_bind_out(&one)
        )
    )

    return e;
}

static derr_t _limit_check_txn(
    MYSQL *sql,
    const dstr_t *uuid,
    unsigned int recipients,
    bool *ok,
    bool *msg_sent,
    unsigned int *limit
){
    derr_t e = E_OK;

    // use FOR UPDATE to avoid write skew
    unsigned int count;
    DSTR_STATIC(
        q1,
        "SELECT daily_msg_limit, msg_count, limit_msg_sent "
        "FROM accounts WHERE user_uuid=? FOR UPDATE;"
    );
    PROP(&e,
        sql_onerow_query(
            sql, &q1, NULL,
            // params
            blob_bind_in(uuid),
            // results
            uint_bind_out(limit),
            uint_bind_out(&count),
            bool_bind_out(msg_sent)
        )
    );

    // check for overflow, then for limit
    if(recipients > UINT_MAX - count || count + recipients > *limit){
        // limit was hit; assume caller will send the limit message
        DSTR_STATIC(q2,
            "UPDATE accounts SET limit_msg_sent=b'1' WHERE user_uuid=?"
        );
        PROP(&e, sql_norow_query(sql, &q2, NULL, blob_bind_in(uuid)) );
    }else{
        // limit was not hit and overflow is not possible
        count += recipients;
        DSTR_STATIC(q2,
            "UPDATE accounts SET msg_count=? WHERE user_uuid=?"
        );
        PROP(&e,
            sql_norow_query(
                sql, &q2, NULL, uint_bind_in(&count), blob_bind_in(uuid)
            )
        );
        *ok = true;
    }

    return e;
}

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
){
    derr_t e = E_OK;
    *ok = false;
    *msg_sent = false;

    PROP(&e, sql_txn_start(sql) );

    PROP_GO(&e,
        _limit_check_txn(sql, uuid, recipients, ok, msg_sent, limit),
    hard_fail);

    // commit the txn even if ok=false as we are also updating msg_sent
    PROP(&e, sql_txn_commit(sql) );

    return e;

hard_fail:
    sql_txn_abort(sql);

    return e;
}
