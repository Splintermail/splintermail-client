// acme_manager_t is the business logic which ties libacme into citm

/*
- is a certificate even required?
  no:
    - insecure works, and that's all that's needed
    - no acme_manager_t at all
  yes:
    - is certificate is manually configured?
      yes:
        - no acme_manager_t t all
        - everything works
      no:
                           (acme_manager_t)
      +-----------------------------------------------------------------------+
      | - is installation configured?                                         |
      |   yes:                                                                |
      |     - is certificate ready?                                           |
      |       yes:                                                            |
      |         - everything works                                            |
      |       no:                                                             |
      |         - insecure works                                              |
      |         - starttls responds "BAD getting certificate, try again later"|
      |         - tls not active                                              |
      |   no:                                                                 |
      |     - insecure works                                                  |
      |     - starttls responds "BAD server needs configuring"                |
      |     - tls not active                                                  |
      +-----------------------------------------------------------------------+
*/

derr_t keygen_or_load(string_builder_t path, EVP_PKEY **out);

struct acme_manager_i;
typedef struct acme_manager_i acme_manager_i;

// acme manager still owns fulldomain
typedef void (*acme_manager_status_cb)(
    void*, status_maj_e maj, status_min_e min, dstr_t fulldomain
);
// callback owns the ctx
typedef void (*acme_manager_update_cb)(void*, SSL_CTX*);
typedef void (*acme_manager_done_cb)(void*, derr_t);

typedef struct {
    derr_t e;
    // iterating through list_orders
    LIST(dstr_t) orders;
    size_t check_order_idx;

    // checking orders
    acme_status_e best;
    acme_status_e challenge_status;

    EVP_PKEY *pkey;

    dstr_t order;
    dstr_t authz;
    dstr_t challenge;
    dstr_t proof;
    dstr_t finalize;
    dstr_t certurl;
    time_t retry_after;
    dstr_t cert;

    bool keygen_started : 1;
    bool keygen_done : 1;
    bool list_orders_sent : 1;
    bool list_orders_done : 1;
    bool checked_existing_orders : 1;
    bool check_order_sent : 1;
    bool check_order_done : 1;
    bool new_order_sent : 1;
    bool new_order_done : 1;
    bool get_authz_sent : 1;
    bool get_authz_resp : 1;
    bool get_authz_done : 1;
    bool prepare_sent : 1;
    bool prepare_done : 1;
    bool challenge_sent : 1;
    bool challenge_done : 1;
    bool finalize_sent : 1;
    bool finalize_done : 1;
} new_cert_t;

typedef struct {
    acme_manager_i *ami;
    string_builder_t acme_dir;
    acme_manager_status_cb status_cb;
    acme_manager_update_cb update_cb;
    acme_manager_done_cb done_cb;
    void *cb_data;

    acme_account_t acct;
    installation_t inst;

    status_maj_e maj;
    status_min_e min;
    dstr_t fulldomain; // defined after successfully loading installation.json

    // state
    derr_t e;
    json_t json;

    time_t expiry;
    time_t renewal;
    time_t backoff_until;
    time_t unprepare_after;

    new_cert_t new_cert;

    size_t failures;

    bool cert_active : 1;    // have we handed out a SSL_CTX to the system?
    bool configured : 1;     // do we have an installation and acme account?
    bool accounted : 1;      // have we obtained an account yet?
    bool new_acct_sent : 1;  // have we requested a new account?
    bool new_acct_done : 1;  // have we been granted a new account?
    bool reloaded : 1;       // have we reloaded acme state from ACME?
    bool want_cert : 1;      // should be be trying to get a new cert?
    bool unprepare_sent : 1; // do we need to clear our dns record?
    bool unprepare_done : 1; // dns record has been cleared
    bool close_sent : 1;     // have we requested a close?
    bool close_done : 1;     // am_close_done() is called
} acme_manager_t;

derr_t acme_manager_init(
    acme_manager_t *am,
    acme_manager_i *ami,
    string_builder_t acme_dir,
    acme_manager_status_cb status_cb,
    acme_manager_update_cb update_cb,
    acme_manager_done_cb done_cb,
    void *cb_data,
    // initial status return values
    SSL_CTX **ctx,
    status_maj_e *maj,
    status_min_e *min,
    dstr_t *fulldomain
);

// doesn't schedule and mostly for testing.  Prefer uv_acme_manager_close().
void am_close(acme_manager_t *am);

void am_advance_state(acme_manager_t *am);

// io interface, for easy testing //

// event sinks (except now)
struct acme_manager_i {
    // return the time right now
    time_t (*now)(acme_manager_i *iface);

    // when deadlines fire, just call am_advance_state()
    void (*deadline_cert)(acme_manager_i *iface, time_t when);
    void (*deadline_backoff)(acme_manager_i *iface, time_t when);
    void (*deadline_unprepare)(acme_manager_i *iface, time_t when);

    // splintermail API calls
    void (*prepare)(
        acme_manager_i *iface, api_token_t token, json_t *json, dstr_t proof
    );
    void (*unprepare)(acme_manager_i *iface, api_token_t token, json_t *json);

    // off-thread work
    derr_t (*keygen)(acme_manager_i *iface, string_builder_t path);

    // acme calls
    void (*new_account)(
        acme_manager_i *iface,
        key_i **k, // takes ownership of the key
        const dstr_t contact_email
    );
    void (*new_order)(
        acme_manager_i *iface, const acme_account_t acct, const dstr_t domain
    );
    void (*get_order)(
        acme_manager_i *iface, const acme_account_t acct, const dstr_t order
    );
    void (*list_orders)(acme_manager_i *iface, const acme_account_t acct);
    void (*get_authz)(
        acme_manager_i *iface, const acme_account_t acct, const dstr_t authz
    );
    void (*challenge)(
        acme_manager_i *iface,
        const acme_account_t acct,
        const dstr_t authz,
        const dstr_t challenge
    );
    void (*challenge_finish)(
        acme_manager_i *iface,
        const acme_account_t acct,
        const dstr_t authz,
        time_t retry_after
    );
    void (*finalize)(
        acme_manager_i *iface,
        const acme_account_t acct,
        const dstr_t order,
        const dstr_t finalize,
        const dstr_t domain,
        EVP_PKEY *pkey  // increments the refcount
    );
    void (*finalize_from_processing)(
        acme_manager_i *iface,
        const acme_account_t acct,
        const dstr_t order,
        time_t retry_after
    );
    void (*finalize_from_valid)(
        acme_manager_i *iface, const acme_account_t acct, const dstr_t certurl
    );
    // will trigger an E_CANCELED callback for any action that is in-flight
    void (*close)(acme_manager_i *iface);
};

// event sources (in addition to the callbacks built-into the event sinks)

void am_check(acme_manager_t *am);

void am_prepare_done(acme_manager_t *am, derr_t err, json_t *json);
void am_unprepare_done(acme_manager_t *am, derr_t err, json_t *json);

void am_keygen_done(acme_manager_t *am, derr_t err, EVP_PKEY *pkey);

void am_new_account_done(
    acme_manager_t *am,
    derr_t err,
    acme_account_t acct
);
void am_new_order_done(
    acme_manager_t *am,
    derr_t err,
    // allocated strings are returned
    dstr_t order,
    dstr_t expires,
    dstr_t authorization,
    dstr_t finalize
);
void am_get_order_done(
    acme_manager_t *am,
    derr_t err,
    // allocated strings are returned
    acme_status_e status,
    dstr_t domain,
    dstr_t expires,
    dstr_t authorization,
    dstr_t finalize,
    dstr_t certurl,     // might be empty
    time_t retry_after  // might be zero
);
// allocated list of allocated strings is returned
void am_list_orders_done(acme_manager_t *am, derr_t err, LIST(dstr_t) orders);
void am_get_authz_done(
    acme_manager_t *am,
    derr_t err,
    acme_status_e status,
    acme_status_e challenge_status,
    // allocated strings are returned
    dstr_t domain,
    dstr_t expires,
    dstr_t challenge,   // only the dns challenge is returned
    dstr_t token,       // only the dns challenge is returned
    time_t retry_after  // might be zero
);
void am_challenge_done(acme_manager_t *am, derr_t err);
void am_finalize_done(acme_manager_t *am, derr_t err, dstr_t cert);
void am_close_done(acme_manager_t *am);
