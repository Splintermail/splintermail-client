#define LETSENCRYPT "https://acme-staging-v02.api.letsencrypt.org/directory"

struct acme_t;
typedef struct acme_t acme_t;

typedef void (*acme_close_cb)(acme_t*);

derr_t acme_new(acme_t **out, duv_http_t *http, dstr_t directory, void *data);
void acme_close(acme_t *acme, acme_close_cb close_cb);
void acme_free(acme_t **acme);

typedef struct {
    acme_t *acme;
    key_i *key;
    dstr_t kid;
    dstr_t orders;
} acme_account_t;

derr_t acme_account_from_json(
    acme_account_t *acct, json_ptr_t ptr, acme_t *acme
);
derr_t acme_account_from_dstr(
    acme_account_t *acct, dstr_t dstr, acme_t *acme
);
derr_t acme_account_from_file(acme_account_t *acct, char *file, acme_t *acme);
derr_t acme_account_from_path(
    acme_account_t *acct, string_builder_t path, acme_t *acme
);

void acme_account_free(acme_account_t *acct);

// all possible statuses in the protocol; not all objects can take all statuses
typedef enum {
    // order is based on overall "betterness", especially for order objects
    ACME_INVALID = 0,
    ACME_REVOKED,
    ACME_DEACTIVATED,
    ACME_EXPIRED,
    ACME_PENDING,
    ACME_READY,
    ACME_PROCESSING,
    ACME_VALID,
} acme_status_e;

dstr_t acme_status_dstr(acme_status_e status);

typedef void (*acme_new_account_cb)(
    void*,
    derr_t,
    acme_account_t acct
);

void acme_new_account(
    acme_t *acme,
    key_i **k, // takes ownership of the key
    const dstr_t contact_email,
    acme_new_account_cb cb,
    void *cb_data
);

typedef void (*acme_new_order_cb)(
    void*,
    derr_t,
    // allocated strings are returned
    dstr_t order,
    dstr_t expires,
    dstr_t authorization,
    dstr_t finalize
);

void acme_new_order(
    const acme_account_t acct,
    const dstr_t domain,
    acme_new_order_cb cb,
    void *cb_data
);

typedef void (*acme_get_order_cb)(
    void*,
    derr_t,
    // allocated strings are returned
    acme_status_e status,
    dstr_t domain,
    dstr_t expires,
    dstr_t authorization,
    dstr_t finalize,
    dstr_t certurl,     // might be empty
    time_t retry_after  // might be zero
);

void acme_get_order(
    const acme_account_t acct,
    const dstr_t order,
    acme_get_order_cb cb,
    void *cb_data
);

// allocated list of allocated strings is returned
typedef void (*acme_list_orders_cb)(void*, derr_t, LIST(dstr_t) orders);

void acme_list_orders(
    const acme_account_t acct,
    acme_list_orders_cb cb,
    void *cb_data
);

typedef void (*acme_get_authz_cb)(
    void*,
    derr_t,
    // allocated strings are returned
    acme_status_e status,
    dstr_t domain,
    dstr_t expires,
    dstr_t challenge,   // only the dns challenge is returned
    dstr_t token,       // only the dns challenge is returned
    time_t retry_after  // might be zero
);

void acme_get_authz(
    const acme_account_t acct,
    const dstr_t authz,
    acme_get_authz_cb cb,
    void *cb_data
);

// will automatically await a challenge result
typedef void (*acme_challenge_cb)(void*, derr_t);

void acme_challenge(
    const acme_account_t acct,
    const dstr_t authz,
    const dstr_t challenge,
    acme_challenge_cb cb,
    void *cb_data
);

// finish challenging, when you wake up to an authz with status=processing
void acme_challenge_finish(
    const acme_account_t acct,
    const dstr_t authz,
    time_t retry_after,
    acme_challenge_cb cb,
    void *cb_data
);

// will automatically await cert
typedef void (*acme_finalize_cb)(void*, derr_t, dstr_t cert);

void acme_finalize(
    const acme_account_t acct,
    const dstr_t order,
    const dstr_t finalize,
    const dstr_t domain,
    EVP_PKEY *pkey,  // increments the refcount
    acme_finalize_cb cb,
    void *cb_data
);

// finish finalizing, when you wake up to an order with status=processing
void acme_finalize_from_processing(
    const acme_account_t acct,
    const dstr_t order,
    time_t retry_after,
    acme_finalize_cb cb,
    void *cb_data
);

// finish finalizing, when you wake up to an order with status=valid
void acme_finalize_from_valid(
    const acme_account_t acct,
    const dstr_t certurl,
    acme_finalize_cb cb,
    void *cb_data
);
