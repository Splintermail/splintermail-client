typedef struct {
    dstr_t new_nonce;
    dstr_t new_account;
    dstr_t new_order;
    dstr_t revoke_cert;
    dstr_t key_change;
    bool external_account_required;
} acme_urls_t;

void acme_urls_free(acme_urls_t *urls);

typedef void (*get_directory_cb)(void*, acme_urls_t, derr_t);

#define ZEROSSL_DIRS "https://acme.zerossl.com/v2/DV90"

derr_t get_directory(
    const dstr_t url,
    duv_http_t *http,
    get_directory_cb cb,
    void *cb_data
);

typedef void (*new_nonce_cb)(void*, dstr_t nonce, derr_t);

derr_t new_nonce(
    const dstr_t url,
    duv_http_t *http,
    new_nonce_cb cb,
    void *cb_data
);

typedef void (*post_new_account_cb)(
    void*, dstr_t kid, dstr_t orders_url, derr_t
);

derr_t post_new_account(
    const dstr_t url,
    duv_http_t *http,
    key_i *k,
    const dstr_t nonce,
    const dstr_t contact_email,
    // eab_kid may be empty to not use external acount binding
    const dstr_t eab_kid,
    const dstr_t eab_hmac_key,
    post_new_account_cb cb,
    void *cb_data
);
