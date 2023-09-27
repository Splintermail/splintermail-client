// libuv implementation of io interface //

typedef struct {
    acme_manager_t am;
    acme_manager_i iface;
    uv_loop_t *loop;
    scheduler_i *scheduler;
    schedulable_t schedulable;

    // nothing else in citm needs http, so we own this
    duv_http_t http;
    // nothing else in citm needs acme, so we own this
    acme_t *acme;
    // nothing else in citm needs an api_client_t, so we own this
    api_client_t apic;
    // to wakeup for cert renewal
    uv_timer_t timer_cert;
    // to wakeup when it's time to try something again
    uv_timer_t timer_backoff;
    // to wakeup when it's time to unprepare again
    uv_timer_t timer_unprepare;

    // for doing keygen off-thread
    uv_work_t work;
    string_builder_t keypath;
    derr_t keygen_err;
    EVP_PKEY *pkey;

    /* we need to intercept the done_cb, because to make uv_acme_manager_close
       idempotent, we need to know if the acme_manager_t failed prior to us
       being closed; it won't be safe to call am_close() otherwise */
    acme_manager_done_cb done_cb;
    /* that means we also need to intercept the update_cb, but only because we
       need to inject our own cb_data */
    acme_manager_update_cb update_cb;
    void *cb_data;

    bool started;
    bool closed;
    bool close_needs_keygen_cb;
    bool keygen_active;
    bool uv_acme_manager_closed;  // make uv_acme_manager_close() idempotent
} uv_acme_manager_t;

// if this fails, you won't see a done_cb but you must still drain the loop
derr_t uv_acme_manager_init(
    uv_acme_manager_t *uvam,
    uv_loop_t *loop,
    duv_scheduler_t *scheduler,
    string_builder_t acme_dir,
    dstr_t acme_dirurl,
    char *acme_verify_name,
    dstr_t sm_baseurl,
    SSL_CTX *client_ctx,
    acme_manager_update_cb update_cb,
    acme_manager_done_cb done_cb,
    void *cb_data,
    SSL_CTX **initial_ctx
);

// will result in a call to done_cb (if init succeeded)
void uv_acme_manager_close(uv_acme_manager_t *uvam);
