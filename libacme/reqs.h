#define LETSENCRYPT "https://acme-staging-v02.api.letsencrypt.org/directory"

struct acme_t;
typedef struct acme_t acme_t;

derr_t acme_new(acme_t **out, duv_http_t *http, dstr_t directory, void *data);
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
    dstr_t domain,
    dstr_t status,
    dstr_t expires,
    dstr_t authorization,
    dstr_t finalize
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
    dstr_t domain,
    dstr_t status,
    dstr_t expires,
    dstr_t challenge, // only the dns challenge is returned
    dstr_t token      // only the dns challenge is returned
);

void acme_get_authz(
    const acme_account_t acct,
    const dstr_t authz,
    acme_get_authz_cb cb,
    void *cb_data
);

typedef void (*acme_challenge_cb)(void*, derr_t);

void acme_challenge(
    const acme_account_t acct,
    const dstr_t challenge,
    acme_challenge_cb cb,
    void *cb_data
);
