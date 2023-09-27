#include "libcitm/libcitm.h"

#include <openssl/evp.h>

// libuv implementation of io interface //

DEF_CONTAINER_OF(uv_acme_manager_t, schedulable, schedulable_t)

static void scheduled(schedulable_t *s){
    uv_acme_manager_t *uvam = CONTAINER_OF(s, uv_acme_manager_t, schedulable);
    am_advance_state(&uvam->am);
}

static void schedule(uv_acme_manager_t *uvam){
    uvam->scheduler->schedule(uvam->scheduler, &uvam->schedulable);
}

DEF_CONTAINER_OF(uv_acme_manager_t, iface, acme_manager_i)

static void uvam_timer_cb(uv_timer_t *timer){
    uv_acme_manager_t *uvam = timer->data;
    // go straight to am_advance_state();
    am_advance_state(&uvam->am);
}

static time_t uvam_now(acme_manager_i *iface){
    uv_acme_manager_t *uvam = CONTAINER_OF(iface, uv_acme_manager_t, iface);
    uint64_t now_ms = uv_now(uvam->loop);
    return (time_t)(now_ms / 1000);
}

static void do_deadline(
    uv_acme_manager_t *uvam, time_t deadline, uv_timer_t *timer
){
    uint64_t now_ms = uv_now(uvam->loop);
    uint64_t deadline_ms = (uint64_t)(deadline * 1000);
    uint64_t delay_ms = now_ms > deadline_ms ? 0 : deadline_ms - now_ms;
    duv_timer_must_start(timer, uvam_timer_cb, delay_ms);
}

static void uvam_deadline_cert(acme_manager_i *iface, time_t deadline){
    uv_acme_manager_t *uvam = CONTAINER_OF(iface, uv_acme_manager_t, iface);
    do_deadline(uvam, deadline, &uvam->timer_cert);
}

static void uvam_deadline_backoff(acme_manager_i *iface, time_t deadline){
    uv_acme_manager_t *uvam = CONTAINER_OF(iface, uv_acme_manager_t, iface);
    do_deadline(uvam, deadline, &uvam->timer_backoff);
}

static void uvam_deadline_unprepare(acme_manager_i *iface, time_t deadline){
    uv_acme_manager_t *uvam = CONTAINER_OF(iface, uv_acme_manager_t, iface);
    do_deadline(uvam, deadline, &uvam->timer_unprepare);
}

static void uvam_prepare_cb(void *data, derr_t err, json_t *json){
    uv_acme_manager_t *uvam = data;
    am_prepare_done(&uvam->am, err, json);
    schedule(uvam);
}

static void uvam_prepare(
    acme_manager_i *iface, api_token_t token, json_t *json, dstr_t proof
){
    uv_acme_manager_t *uvam = CONTAINER_OF(iface, uv_acme_manager_t, iface);
    apic_token(
        &uvam->apic,
        DSTR_LIT("/api/set_challenge"),
        proof,
        token,
        json,
        uvam_prepare_cb,
        uvam
    );
}

static void uvam_unprepare_cb(void *data, derr_t err, json_t *json){
    uv_acme_manager_t *uvam = data;
    am_unprepare_done(&uvam->am, err, json);
    schedule(uvam);
}

static void uvam_unprepare(
    acme_manager_i *iface, api_token_t token, json_t *json
){
    uv_acme_manager_t *uvam = CONTAINER_OF(iface, uv_acme_manager_t, iface);
    apic_token(
        &uvam->apic,
        DSTR_LIT("/api/delete_challenge"),
        (dstr_t){0},
        token,
        json,
        uvam_unprepare_cb,
        uvam
    );
}

static void keygen(uv_work_t *work){
    uv_acme_manager_t *uvam = work->data;

    TRACE_PROP(&uvam->keygen_err, keygen_or_load(uvam->keypath, &uvam->pkey) );
}

static void keygen_cb(uv_work_t *work, int status){
    uv_acme_manager_t *uvam = work->data;
    EVP_PKEY *pkey = NULL;

    derr_t e = E_OK;
    PROP_VAR_GO(&e, &uvam->keygen_err, done);

    // check status
    if(status < 0){
        derr_type_t etype = derr_type_from_uv_status(status);
        ORIG_GO(&e, etype, "keygen_cb failed", done);
    }

    // success
    pkey = uvam->pkey;
    uvam->pkey = NULL;

done:
    EVP_PKEY_free(uvam->pkey);
    am_keygen_done(&uvam->am, e, pkey);
    uvam->keygen_active = false;
    // were we waiting on this in order to call am_close_done()?
    if(uvam->close_needs_keygen_cb){
        am_close_done(&uvam->am);
    }
    schedule(uvam);
}

static derr_t uvam_keygen(acme_manager_i *iface, string_builder_t path){
    derr_t e = E_OK;

    uv_acme_manager_t *uvam = CONTAINER_OF(iface, uv_acme_manager_t, iface);

    uvam->keypath = path;
    uvam->work.data = uvam;
    PROP(&e, duv_queue_work(uvam->loop, &uvam->work, keygen, keygen_cb) );
    uvam->keygen_active = true;

    return e;
}

static void uvam_new_account_cb(void *data, derr_t err, acme_account_t acct){
    uv_acme_manager_t *uvam = data;
    am_new_account_done(&uvam->am, err, acct);
    schedule(uvam);
}

static void uvam_new_account(
    acme_manager_i *iface,
    key_i **k, // takes ownership of the key
    const dstr_t contact_email
){
    uv_acme_manager_t *uvam = CONTAINER_OF(iface, uv_acme_manager_t, iface);
    acme_new_account(uvam->acme, k, contact_email, uvam_new_account_cb, uvam);
}

static void uvam_new_order_cb(
    void *data,
    derr_t err,
    dstr_t order,
    dstr_t expires,
    dstr_t authorization,
    dstr_t finalize
){
    uv_acme_manager_t *uvam = data;
    am_new_order_done(
        &uvam->am, err, order, expires, authorization, finalize
    );
    schedule(uvam);
}

static void uvam_new_order(
    acme_manager_i *iface,
    const acme_account_t acct,
    const dstr_t domain
){
    uv_acme_manager_t *uvam = CONTAINER_OF(iface, uv_acme_manager_t, iface);
    acme_new_order(uvam->acme, acct, domain, uvam_new_order_cb, uvam);
}

static void uvam_get_order_cb(
    void *data,
    derr_t err,
    acme_status_e status,
    dstr_t domain,
    dstr_t expires,
    dstr_t authorization,
    dstr_t finalize,
    dstr_t certurl,
    time_t retry_after
){
    uv_acme_manager_t *uvam = data;
    am_get_order_done(
        &uvam->am,
        err,
        status,
        domain,
        expires,
        authorization,
        finalize,
        certurl,
        retry_after
    );
    schedule(uvam);
}

static void uvam_get_order(
    acme_manager_i *iface,
    const acme_account_t acct,
    const dstr_t order
){
    uv_acme_manager_t *uvam = CONTAINER_OF(iface, uv_acme_manager_t, iface);
    acme_get_order(uvam->acme, acct, order, uvam_get_order_cb, uvam);
}

static void uvam_list_orders_cb(void *data, derr_t err, LIST(dstr_t) orders){
    uv_acme_manager_t *uvam = data;
    am_list_orders_done(&uvam->am, err, orders);
    schedule(uvam);
}

static void uvam_list_orders(
    acme_manager_i *iface,
    const acme_account_t acct
){
    uv_acme_manager_t *uvam = CONTAINER_OF(iface, uv_acme_manager_t, iface);
    acme_list_orders(uvam->acme, acct, uvam_list_orders_cb, uvam);
}

static void uvam_get_authz_cb(
    void *data,
    derr_t err,
    acme_status_e status,
    acme_status_e challenge_status,
    dstr_t domain,
    dstr_t expires,
    dstr_t challenge,
    dstr_t token,
    time_t retry_after
){
    uv_acme_manager_t *uvam = data;
    am_get_authz_done(
        &uvam->am,
        err,
        status,
        challenge_status,
        domain,
        expires,
        challenge,
        token,
        retry_after
    );
    schedule(uvam);
}

static void uvam_get_authz(
    acme_manager_i *iface,
    const acme_account_t acct,
    const dstr_t authz
){
    uv_acme_manager_t *uvam = CONTAINER_OF(iface, uv_acme_manager_t, iface);
    acme_get_authz(uvam->acme, acct, authz, uvam_get_authz_cb, uvam);
}

static void uvam_challenge_cb(void *data, derr_t err){
    uv_acme_manager_t *uvam = data;
    am_challenge_done(&uvam->am, err);
    schedule(uvam);
}

static void uvam_challenge(
    acme_manager_i *iface,
    const acme_account_t acct,
    const dstr_t authz,
    const dstr_t challenge
){
    uv_acme_manager_t *uvam = CONTAINER_OF(iface, uv_acme_manager_t, iface);
    acme_challenge(
        uvam->acme, acct, authz, challenge, uvam_challenge_cb, uvam
    );
}

static void uvam_challenge_finish(
    acme_manager_i *iface,
    const acme_account_t acct,
    const dstr_t authz,
    time_t retry_after
){
    uv_acme_manager_t *uvam = CONTAINER_OF(iface, uv_acme_manager_t, iface);
    acme_challenge_finish(
        uvam->acme, acct, authz, retry_after, uvam_challenge_cb, uvam
    );
}

static void uvam_finalize_cb(void *data, derr_t err, dstr_t cert){
    uv_acme_manager_t *uvam = data;
    am_finalize_done(&uvam->am, err, cert);
    schedule(uvam);
}

static void uvam_finalize(
    acme_manager_i *iface,
    const acme_account_t acct,
    const dstr_t order,
    const dstr_t finalize,
    const dstr_t domain,
    EVP_PKEY *pkey  // increments the refcount
){
    uv_acme_manager_t *uvam = CONTAINER_OF(iface, uv_acme_manager_t, iface);
    acme_finalize(
        uvam->acme, acct, order, finalize, domain, pkey, uvam_finalize_cb, uvam
    );
}

static void uvam_finalize_from_processing(
    acme_manager_i *iface,
    const acme_account_t acct,
    const dstr_t order,
    time_t retry_after
){
    uv_acme_manager_t *uvam = CONTAINER_OF(iface, uv_acme_manager_t, iface);
    acme_finalize_from_processing(
        uvam->acme, acct, order, retry_after, uvam_finalize_cb, uvam
    );
}

static void uvam_finalize_from_valid(
    acme_manager_i *iface,
    const acme_account_t acct,
    const dstr_t certurl
){
    uv_acme_manager_t *uvam = CONTAINER_OF(iface, uv_acme_manager_t, iface);
    acme_finalize_from_valid(
        uvam->acme, acct, certurl, uvam_finalize_cb, uvam
    );
}

static void http_close_cb(duv_http_t *http){
    uv_acme_manager_t *uvam = http->data;
    api_client_free(&uvam->apic);
    if(uvam->keygen_active){
        // we can't cancel keygen, so we wait for it instead
        uvam->close_needs_keygen_cb = true;
    }else{
        am_close_done(&uvam->am);
        schedule(uvam);
    }
}

static void _acme_close_cb(void *data){
    uv_acme_manager_t *uvam = data;
    acme_free(&uvam->acme);
    // note, closing the duv_http_t also cancels our apic requests, implicitly
    duv_http_close(&uvam->http, http_close_cb);
}

static void timer_close_cb_iii(uv_handle_t *handle){
    uv_acme_manager_t *uvam = handle->data;
    acme_close(uvam->acme, _acme_close_cb, uvam);
}

static void timer_close_cb_ii(uv_handle_t *handle){
    uv_acme_manager_t *uvam = handle->data;
    duv_timer_close(&uvam->timer_cert, timer_close_cb_iii);
}

static void timer_close_cb_i(uv_handle_t *handle){
    uv_acme_manager_t *uvam = handle->data;
    duv_timer_close(&uvam->timer_backoff, timer_close_cb_ii);
}

static void uvam_close(acme_manager_i *iface){
    uv_acme_manager_t *uvam = CONTAINER_OF(iface, uv_acme_manager_t, iface);
    if(!uvam->closed){
        uvam->closed = true;
        duv_timer_close(&uvam->timer_unprepare, timer_close_cb_i);
    }
}

static void http_close_after_fail(duv_http_t *http){
    (void)http;
}

static void acme_close_after_fail(void *data){
    uv_acme_manager_t *uvam = data;
    acme_free(&uvam->acme);
    duv_http_close(&uvam->http, http_close_after_fail);
}

static void timer_close_after_fail_iii(uv_handle_t *handle){
    uv_acme_manager_t *uvam = handle->data;
    acme_close(uvam->acme, acme_close_after_fail, uvam);
}

static void timer_close_after_fail_ii(uv_handle_t *handle){
    uv_acme_manager_t *uvam = handle->data;
    duv_timer_close(&uvam->timer_cert, timer_close_after_fail_iii);
}

static void timer_close_after_fail_i(uv_handle_t *handle){
    uv_acme_manager_t *uvam = handle->data;
    duv_timer_close(&uvam->timer_backoff, timer_close_after_fail_ii);
}

static void uvam_done_cb(void *data, derr_t err){
    uv_acme_manager_t *uvam = data;
    // remember that am_close() is no longer safe to call
    uvam->uv_acme_manager_closed = true;
    // pass the callback forward
    uvam->done_cb(uvam->cb_data, err);
}

static void uvam_update_cb(void *data, SSL_CTX *ctx){
    // blindly pass this forward with the right cb_data
    uv_acme_manager_t *uvam = data;
    uvam->update_cb(uvam->cb_data, ctx);
}

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
){
    derr_t e = E_OK;

    *initial_ctx = NULL;

    *uvam = (uv_acme_manager_t){
        .iface = (acme_manager_i){
            .now = uvam_now,
            .deadline_cert = uvam_deadline_cert,
            .deadline_backoff = uvam_deadline_backoff,
            .deadline_unprepare = uvam_deadline_unprepare,
            .prepare = uvam_prepare,
            .unprepare = uvam_unprepare,
            .keygen = uvam_keygen,
            .new_account = uvam_new_account,
            .new_order = uvam_new_order,
            .get_order = uvam_get_order,
            .list_orders = uvam_list_orders,
            .get_authz = uvam_get_authz,
            .challenge = uvam_challenge,
            .challenge_finish = uvam_challenge_finish,
            .finalize = uvam_finalize,
            .finalize_from_processing = uvam_finalize_from_processing,
            .finalize_from_valid = uvam_finalize_from_valid,
            .close = uvam_close,
        },
        .loop = loop,
        .scheduler = &scheduler->iface,
        .update_cb = update_cb,
        .done_cb = done_cb,
        .cb_data = cb_data,
    };

    schedulable_prep(&uvam->schedulable, scheduled);

    uvam->http.data = uvam;
    IF_PROP(&e, duv_http_init(&uvam->http, loop, scheduler, client_ctx) ){
        duv_http_close(&uvam->http, http_close_after_fail);
        return e;
    }

    IF_PROP(&e,
        acme_new_ex(&uvam->acme, &uvam->http, acme_dirurl, acme_verify_name)
    ){
        acme_close(uvam->acme, acme_close_after_fail, uvam);
        return e;
    }

    duv_timer_must_init(loop, &uvam->timer_cert);
    duv_timer_must_init(loop, &uvam->timer_backoff);
    duv_timer_must_init(loop, &uvam->timer_unprepare);
    uvam->timer_cert.data = uvam;
    uvam->timer_backoff.data = uvam;
    uvam->timer_unprepare.data = uvam;

    // remaining failures trigger timer_close_after_fail

    PROP_GO(&e, api_client_init(&uvam->apic, &uvam->http, sm_baseurl), fail);

    PROP_GO(&e,
        acme_manager_init(
            &uvam->am,
            &uvam->iface,
            acme_dir,
            uvam_update_cb,
            uvam_done_cb,
            uvam, // cb_data
            initial_ctx
        ),
    fail);
    schedule(uvam);

    uvam->started = true;

    return e;

fail:
    api_client_free(&uvam->apic);
    duv_timer_close(&uvam->timer_unprepare, timer_close_after_fail_i);
    return e;
}

void uv_acme_manager_close(uv_acme_manager_t *uvam){
    if(!uvam->started || uvam->uv_acme_manager_closed) return;
    uvam->uv_acme_manager_closed = true;
    am_close(&uvam->am);
    schedule(uvam);
}
