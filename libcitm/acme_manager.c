#include "libcitm/libcitm.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#define DAY 86400

#define ONCE(x) if(!x && (x = true))

// some paths
static string_builder_t am_cert(acme_manager_t *am){
    return sb_append(&am->acme_dir, SBS("cert.pem"));
}
static string_builder_t am_certnew(acme_manager_t *am){
    return sb_append(&am->acme_dir, SBS("certnew.pem"));
}
static string_builder_t am_key(acme_manager_t *am){
    return sb_append(&am->acme_dir, SBS("key.pem"));
}
static string_builder_t am_keynew(acme_manager_t *am){
    return sb_append(&am->acme_dir, SBS("keynew.pem"));
}
static string_builder_t am_inst(acme_manager_t *am){
    return sb_append(&am->acme_dir, SBS("installation.json"));
}
static string_builder_t am_jwk(acme_manager_t *am){
    return sb_append(&am->acme_dir, SBS("jwk.json"));
}
static string_builder_t am_acct(acme_manager_t *am){
    return sb_append(&am->acme_dir, SBS("account.json"));
}

// error type checks
static bool is_network(derr_type_t etype){
    return etype == E_SOCK || etype == E_CONN;
}
static bool is_network_or_resp(derr_type_t etype){
    return is_network(etype) || etype == E_RESPONSE;
}

static derr_t keyread(string_builder_t path, EVP_PKEY **out){
    derr_t e = E_OK;

    // read keyfile
    DSTR_VAR(pem, 8192);
    derr_t e2 = dstr_read_path(&path, &pem);
    CATCH(&e2, E_FIXEDSIZE){
        DROP_VAR(&e2);
        ORIG_GO(&e, E_PARAM, "keyfile (%x) too long", cu, FSB(path));
    }else PROP_VAR_GO(&e, &e2, cu);

    // load key into memory
    PROP_GO(&e, read_pem_encoded_privkey(pem, out), cu);

cu:
    dstr_zeroize(&pem);

    return e;
}

derr_t keygen_or_load(string_builder_t path, EVP_PKEY **out){
    derr_t e = E_OK;

    *out = NULL;

    bool ok;
    PROP(&e, exists_path(&path, &ok) );
    if(ok){
        // read existing key
        IF_PROP(&e, keyread(path, out) ){
            LOG_DEBUG(
                "failed loading keynew @%x, will regen; error:\n%x",
                FSB(path),
                FD(e.msg)
            );
            DROP_VAR(&e);
            goto regen;
        }
        // successfully loaded an existing key
        return e;
    }

regen:
    // generate key
    PROP(&e, gen_key_path(2048, &path) );

    PROP(&e, keyread(path, out) );

    return e;
}

static derr_t jwk_gen_or_load(string_builder_t path, key_i **out){
    derr_t e = E_OK;

    *out = NULL;
    FILE *f = NULL;
    key_i *key = NULL;

    // does the jwk file even exist? (avoid printing frivolous errors)
    bool ok;
    PROP_GO(&e, exists_path2(path, &ok), gen_after_fail);
    if(!ok) goto gen_after_noexist;

    // try to read an existing file
    DSTR_VAR(buf, 1024);
    PROP_GO(&e, dstr_read_path(&path, &buf), gen_after_fail);
    PROP_GO(&e, json_to_key(buf, out), gen_after_fail);

    // successfully loaded existing key
    dstr_zeroize(&buf);

    return e;

gen_after_fail:
    // if loading fails, generate a new key
    LOG_DEBUG("loading jwk failed:\n%x----\n", FD(e.msg));
    DROP_VAR(&e);

gen_after_noexist:
    LOG_ERROR("generating a new jwk for a new ACME account\n");
    PROP_GO(&e, gen_es256(&key), fail);

    // preserve key to a file
    PROP_GO(&e, dfopen_path(&path, "w", &f), fail);
    PROP_GO(&e, jdump(DJWKPVT(key), WF(f), 2), fail);
    PROP_GO(&e, dfclose2(&f), fail);

    // successfully generated a new key
    *out = key;

    return e;

fail:
    if(f) fclose(f);
    if(key) key->free(key);
    return e;
}

static derr_t pre_apic_call(acme_manager_t *am){
    derr_t e = E_OK;

    json_free(&am->json);

    // refresh nonce

    /* The reason we use a separate nonce file is to ensure we don't overwrite
       a user's new installation right after they reconfigure it.

       The reason we use a unique nonce file is to ensure that we don't confuse
       an old nonce and a new nonce after a reconfiguration event occurs. */

    // find the nonce filename for our api key
    DSTR_VAR(name, 64);
    FMT_QUIET(&name, "%x.nonce", FU(am->inst.token.key));
    string_builder_t path = sb_append(&am->acme_dir, SBD(name));

    // read/increment/write the nonce from file
    PROP(&e, nonce_read_increment_write_path(&path, &am->inst.token.nonce) );

    return e;
}

static void freesteal(dstr_t *dst, dstr_t *src){
    dstr_free(dst);
    *dst = STEAL(dstr_t, src);
}

static void new_cert_free(new_cert_t *nc){
    DROP_VAR(&nc->e);
    for(size_t i = 0; i < nc->orders.len; i++){
        dstr_free(&nc->orders.data[i]);
    }
    LIST_FREE(dstr_t, &nc->orders);
    EVP_PKEY_free(nc->pkey);
    dstr_free(&nc->order);
    dstr_free(&nc->authz);
    dstr_free(&nc->challenge);
    dstr_free(&nc->proof);
    dstr_free(&nc->finalize);
    dstr_free(&nc->certurl);
    dstr_free(&nc->cert);
    *nc = (new_cert_t){0};
}

static void am_free(acme_manager_t *am){
    new_cert_free(&am->new_cert);
    dstr_free(&am->fulldomain);
    installation_free0(&am->inst);
    acme_account_free(&am->acct);
    json_free(&am->json);
    *am = (acme_manager_t){0};
}

static derr_t check_cert(
    string_builder_t path, time_t *expiry, const dstr_t name, bool *namematch
){
    derr_t e = E_OK;
    if(expiry) *expiry = 0;
    if(namematch) *namematch = false;

    FILE *f = NULL;
    X509 *x509 = NULL;

    // open cert file
    PROP_GO(&e, dfopen_path(&path, "r", &f), cu);

    // read the x509 cert
    x509 = PEM_read_X509(f, NULL, NULL, NULL);
    if(!x509) ORIG_GO(&e, E_SSL, "failed to read certificate: %x", cu, FSSL);

    if(expiry){
        // get the asn1-encoded time
        const ASN1_TIME *notafter = X509_get0_notAfter(x509);

        // convert to struct tm
        struct tm tm;
        int ret = ASN1_TIME_to_tm(notafter, &tm);
        if(ret != 1){
            ORIG_GO(&e, E_PARAM, "failed to read cert notAfter time", cu);
        }

        // convert to time_t
        PROP_GO(&e, dmktime_utc(dtm_from_tm(tm), expiry), cu);
    }

    if(namematch){
        int ret = X509_check_host(x509, name.data, name.len, 0, NULL);
        *namematch = (ret == 1);
    }

cu:
    if(f) fclose(f);
    X509_free(x509);

    return e;
}

static derr_t check_expiry(string_builder_t path, time_t *expiry){
    return check_cert(path, expiry, (dstr_t){0}, NULL);
}

static time_t get_renewal(time_t expiry){
    if(expiry < 15*DAY) return 0;
    return expiry - 15*DAY;
}

static void backoff(acme_manager_t *am, time_t delay){
    acme_manager_i *ami = am->ami;
    am->backoff_until = ami->now(ami) + delay;
    ami->deadline_backoff(ami, am->backoff_until);
}

static time_t _get_backoff(
    size_t attempts, time_t *backoffs, size_t nbackoffs
){
    if(!nbackoffs) return 0;
    // overflow case
    if(attempts > nbackoffs) return backoffs[nbackoffs-1];
    // underflow case
    if(!attempts) return backoffs[0];
    return backoffs[attempts - 1];
}

#define get_backoff(attempts, ...) \
    _get_backoff( \
        attempts, \
        (time_t[]){__VA_ARGS__}, \
        sizeof((time_t[]){__VA_ARGS__}) / sizeof(time_t) \
    )

static bool in_backoff(acme_manager_t *am){
    acme_manager_i *ami = am->ami;
    return ami->now(ami) < am->backoff_until;
}

void am_prepare_done(acme_manager_t *am, derr_t err, json_t *json){
    new_cert_t *nc = &am->new_cert;

    MULTIPROP_VAR_GO(&nc->e, done, &err);

    // parse response
    dstr_t result;
    jspec_t *jspec = JAPI(JOBJ(true, JKEY("result", JDREF(&result))));

    bool ok;
    DSTR_VAR(errbuf, 512);
    PROP_GO(&nc->e, jspec_read_ex(jspec, json->root, &ok, &errbuf), done);
    if(!ok){
        ORIG_GO(&nc->e, E_RESPONSE, "%x", done, FD(errbuf));
    }

    if(dstr_eq(result, DSTR_LIT("ok"))){
        // done preparing
        nc->prepare_done = true;
    }else if(dstr_eq(result, DSTR_LIT("timeout"))){
        // retry
        nc->prepare_sent = false;
    }else{
        ORIG_GO(&nc->e,
            E_RESPONSE,
            "invalid result from set_challenge API: %x",
            done,
            FD(result)
        );
    }

done:
    return;
}

void am_unprepare_done(acme_manager_t *am, derr_t err, json_t *json){
    acme_manager_i *ami = am->ami;

    if(is_error(am->e)){
        // already have an error, we're trying to shut down
        DROP_VAR(&err);
        goto done;
    }

    if(!is_error(err)){
        // the HTTP request succeeded, but we need to check the response still
        dstr_t okstring;
        jspec_t *jspec = JAPI(JDREF(&okstring));

        bool ok;
        DSTR_VAR(errbuf, 512);
        PROP_GO(&err, jspec_read_ex(jspec, json->root, &ok, &errbuf), done);
        if(!ok){
            ORIG_GO(&err, E_RESPONSE, "%x", done, FD(errbuf));
        }

        am->unprepare_done = true;
    }

done:
    if(is_error(err)){
        // dump errors and retry 10 min later
        LOG_ERROR("unprepare() failed:\n");
        DUMP(err);
        DROP_VAR(&err);
        LOG_ERROR("trying again in 10 minutes...\n");
        am->unprepare_sent = false;
        am->unprepare_after = ami->now(ami) + 10*60;
        ami->deadline_unprepare(ami, am->unprepare_after);
    }
    return;
}

void am_keygen_done(acme_manager_t *am, derr_t err, EVP_PKEY *pkey){
    new_cert_t *nc = &am->new_cert;

    // keygen errors are fatal, so they go to am->e instead of nc->e
    MULTIPROP_VAR_GO(&am->e, done, &err);

    nc->keygen_done = true;
    nc->pkey = pkey;

done:
    return;
}

void am_new_account_done(
    acme_manager_t *am,
    derr_t err,
    acme_account_t acct
){
    if(is_network(err.type)){
        // retryable failure
        LOG_ERROR("Network failure creating new account:\n");
        DUMP(err);
        DROP_VAR(&err);
        time_t delay = get_backoff(++am->failures, 1, 5, 15, 30, 45, 60);
        backoff(am, delay);
        LOG_ERROR("Retrying in %x seconds...\n", FI(delay));
        am->new_acct_sent = false;
        goto done;
    }
    // even though we are retrying forever, we're not retrying all errors
    MULTIPROP_VAR_GO(&am->e, done, &err);
    am->failures = 0;
    am->acct = acct;
    am->new_acct_done = true;
done:
    return;
}

void am_new_order_done(
    acme_manager_t *am,
    derr_t err,
    dstr_t order,
    dstr_t expires,
    dstr_t authorization,
    dstr_t finalize
){
    new_cert_t *nc = &am->new_cert;
    MULTIPROP_VAR_GO(&nc->e, done, &err);

    freesteal(&nc->order, &order);
    dstr_free(&expires);
    freesteal(&nc->authz, &authorization);
    freesteal(&nc->finalize, &finalize);

    nc->new_order_done = true;

done:
    return;
}

void am_get_order_done(
    acme_manager_t *am,
    derr_t err,
    acme_status_e status,
    dstr_t domain,
    dstr_t expires,
    dstr_t authorization,
    dstr_t finalize,
    dstr_t certurl,     // might be empty
    time_t retry_after  // might be zero
){
    new_cert_t *nc = &am->new_cert;

    MULTIPROP_VAR_GO(&nc->e, done, &err);

    nc->check_order_done = true;

    // ignore orders which aren't our exact domain
    if(!dstr_eq(domain, am->fulldomain)) goto done;

    // ignore this order if it isn't our best status yet, or if it is useless
    if(status <= nc->best || status < ACME_PENDING) goto done;

    // remember this status and this order url
    nc->best = status;
    freesteal(&nc->order, &nc->orders.data[nc->check_order_idx]);

    switch(status){
        case ACME_INVALID:
        case ACME_REVOKED:
        case ACME_DEACTIVATED:
        case ACME_EXPIRED:
            // don't care
            break;

        case ACME_PENDING:
            // prepare for get_authz, then acme_finalize
            freesteal(&nc->authz, &authorization);
            freesteal(&nc->finalize, &finalize);
            break;

        case ACME_READY:
            // prepare for acme_finalize
            freesteal(&nc->finalize, &finalize);
            break;

        case ACME_PROCESSING:
            // prepare for acme_finalize_from_processing
            nc->retry_after = retry_after;
            break;

        case ACME_VALID:
            // prepare for acme_finalize_from_valid
            freesteal(&nc->certurl, &certurl);
            break;
    }

done:
    dstr_free(&domain);
    dstr_free(&expires);
    dstr_free(&authorization);
    dstr_free(&finalize);
    dstr_free(&certurl);
}

void am_list_orders_done(acme_manager_t *am, derr_t err, LIST(dstr_t) orders){
    new_cert_t *nc = &am->new_cert;
    MULTIPROP_VAR_GO(&nc->e, done, &err);

    nc->orders = orders;
    orders = (LIST(dstr_t)){0};
    nc->list_orders_done = 1;

done:
    LIST_FREE(dstr_t, &orders);
    return;
}

void am_get_authz_done(
    acme_manager_t *am,
    derr_t err,
    acme_status_e status,
    acme_status_e challenge_status,
    dstr_t domain,
    dstr_t expires,
    dstr_t challenge,   // only the dns challenge is returned
    dstr_t dns01_token, // only the dns challenge is returned
    time_t retry_after  // might be zero
){
    new_cert_t *nc = &am->new_cert;
    MULTIPROP_VAR_GO(&nc->e, done, &err);

    (void)status;
    nc->challenge_status = challenge_status;
    dstr_free(&domain);
    dstr_free(&expires);
    freesteal(&nc->challenge, &challenge);
    freesteal(&nc->proof, &dns01_token);
    nc->retry_after = retry_after;

    nc->get_authz_resp = true;

done:
    return;
}

void am_challenge_done(acme_manager_t *am, derr_t err){
    new_cert_t *nc = &am->new_cert;
    MULTIPROP_VAR_GO(&nc->e, done, &err);

    nc->challenge_done = true;

done:
    return;
}

void am_finalize_done(acme_manager_t *am, derr_t err, dstr_t cert){
    new_cert_t *nc = &am->new_cert;
    MULTIPROP_VAR_GO(&nc->e, done, &err);

    freesteal(&nc->cert, &cert);

    nc->finalize_done = true;

done:
    return;
}

void am_close_done(acme_manager_t *am){
    am->close_done = true;
}

// fetch each order, looking for valid (preferred) or processing
static void advance_check_existing_orders(
    acme_manager_t *am, bool *done_processing
){
    new_cert_t *nc = &am->new_cert;
    acme_manager_i *ami = am->ami;

    *done_processing = false;

    if(nc->check_order_done){
        if(nc->best == ACME_VALID) goto done; // stop iteration immediately
        nc->check_order_sent = false;
        nc->check_order_done = false;
        nc->check_order_idx++;
    }
    // done iterating yet?
    if(nc->check_order_idx >= nc->orders.len) goto done;
    ONCE(nc->check_order_sent){
        // check the next order
        dstr_t order = nc->orders.data[nc->check_order_idx];
        ami->get_order(ami, am->acct, order);
    }
    return;

done:
    *done_processing = true;
}

static derr_t rename_new_key_and_cert(acme_manager_t *am, bool check){
    derr_t e = E_OK;

    string_builder_t key = am_key(am);
    string_builder_t keynew = am_keynew(am);
    string_builder_t cert = am_cert(am);
    string_builder_t certnew = am_certnew(am);

    // does cert.pem.new exist?
    bool ok;
    PROP(&e, exists_path(&certnew, &ok) );
    if(!ok) return e;

    if(check){
        // load the cert to make sure we didn't crash mid-write
        IF_PROP(&e, check_cert(certnew, NULL, (dstr_t){0}, NULL) ){
            // read existing key
            LOG_DEBUG(
                "failed loading certnew @%x, will recreate; error:\n%x",
                FSB(certnew),
                FD(e.msg)
            );
            DROP_VAR(&e);
            return e;
        }
    }

    // does key.pem.new exist?
    PROP(&e, exists_path(&keynew, &ok) );
    if(ok){
        // rename key
        PROP(&e, drename_atomic_path(&keynew, &key) );
    }

    // rename cert
    PROP(&e, drename_atomic_path(&certnew, &cert) );

    return e;
}

static derr_t advance_new_cert(acme_manager_t *am, bool *new_cert_ready){
    derr_t e = E_OK;

    acme_manager_i *ami = am->ami;
    new_cert_t *nc = &am->new_cert;

    *new_cert_ready = false;
    bool ok;

    // temporary errors in new cert efforts are reraised at this point
    PROP_VAR_GO(&e, &nc->e, cu);

    // start generating a key, this is asynchronous to the ACME client logic
    ONCE(nc->keygen_started){
        // no restart for keygen; it is not allowed to fail
        PROP_GO(&e, ami->keygen(ami, am_keynew(am)), cu);
    }

    if(!am->reloaded){
        // start by listing orders
        ONCE(nc->list_orders_sent){
            ami->list_orders(ami, am->acct);
            nc->best = ACME_INVALID;
        }
        if(!nc->list_orders_done) return e;
        if(!nc->checked_existing_orders){
            advance_check_existing_orders(am, &ok);
            if(!ok) return e;
            nc->checked_existing_orders = true;
            // jump ahead to pick up where the last attempt left off
            if(nc->best == ACME_PENDING){
                // skip to get_authz
                nc->new_order_done = true;
            }else if(nc->best == ACME_READY){
                // skip to acme_finalize
                nc->new_order_done = true;
                nc->get_authz_done = true;
                nc->prepare_done = true;
                nc->challenge_done = true;
            }else if(nc->best == ACME_PROCESSING){
                // skip to acme_finalize_from_processing
                ami->finalize_from_processing(
                    ami, am->acct, nc->order, nc->retry_after
                );
                // skip steps
                nc->new_order_done = true;
                nc->get_authz_done = true;
                nc->prepare_done = true;
                nc->challenge_done = true;
                nc->finalize_sent = true;
            }else if(nc->best == ACME_VALID){
                // we had an order with status=valid
                ami->finalize_from_valid(ami, am->acct, nc->certurl);
                // skip steps
                nc->new_order_done = true;
                nc->get_authz_done = true;
                nc->prepare_done = true;
                nc->challenge_done = true;
                nc->finalize_sent = true;
            }
        }
        am->reloaded = true;
    }

    // create a new order
    if(!nc->new_order_done){
        ONCE(nc->new_order_sent){
            ami->new_order(ami, am->acct, am->fulldomain);
        }
        return e;
    }

    if(!nc->get_authz_done){
        ONCE(nc->get_authz_sent){
            ami->get_authz(ami, am->acct, nc->authz);
        }
        if(!nc->get_authz_resp) return e;
        if(nc->challenge_status == ACME_PENDING){
            // proceed to prepare (noop)
        }else if(nc->challenge_status == ACME_PROCESSING){
            // we already submitted the challenge, wait for it to complete
            ami->challenge_finish(ami, am->acct, nc->authz, nc->retry_after);
            // skip some steps
            nc->prepare_done = true;
            nc->challenge_sent = true;
        }else if(nc->challenge_status == ACME_VALID){
            // challenge must have completed since we listed orders
            // skip some steps
            nc->prepare_done = true;
            nc->challenge_done = true;
        }else{
            // ACME_INVALID, or something we don't expect
            // challenge must have failed since we listed orders
            ORIG_GO(&e,
                E_RESPONSE,
                "challenge in bad state: %x",
                cu,
                FD(acme_status_dstr(nc->challenge_status))
            );
        }
        nc->get_authz_done = true;
    }

    if(!nc->prepare_done){
        ONCE(nc->prepare_sent){
            PROP_GO(&e, pre_apic_call(am), cu);
            ami->prepare(ami, am->inst.token, &am->json, nc->proof);
            // clear our unprepare status
            am->unprepare_sent = false;
            am->unprepare_done = false;
            am->unprepare_after = 0;
        }
        return e;
    }

    if(!nc->challenge_done){
        ONCE(nc->challenge_sent){
            ami->challenge(ami, am->acct, nc->authz, nc->challenge);
        }
        return e;
    }

    // wait for the keygen thread to finish; it will populate nc->pkey
    if(!nc->keygen_done) return e;

    /* poll for order status=ready?  I want to believe the acme server is smart
       enough that it is always in the right state; I'll omit that wait until
       there is reason to believe it is necessary. */

    if(!nc->finalize_done){
        ONCE(nc->finalize_sent){
            ami->finalize(
                ami,
                am->acct,
                nc->order,
                nc->finalize,
                am->fulldomain,
                nc->pkey
            );
        }
        return e;
    }

    // write the cert to file
    string_builder_t certnew = am_certnew(am);
    PROP_GO(&e, dstr_write_path(&certnew, &nc->cert), cu);

    // overwrite cert and key
    PROP_GO(&e, rename_new_key_and_cert(am, false), cu);

    *new_cert_ready = true;

cu:
    new_cert_free(nc);

    return e;
}

/* The reason to load the installation api token once, and keep the nonce in a
   separate file (which is updated freqeuently) is to support the most correct
   behavior after user reconfiguration.  If a user reconfigures while citm is
   active, citm will continue to use its loaded installation, modifying its
   nonce (which should be a unique filename based on the api key) and not
   affecting the newly-configured installation.  If we read/incremented/wrote
   the entire installation file every time we made a request, a user could
   reconfigure the installation and we might overwrite their new installation
   data immediately. */
static derr_t load_installation(acme_manager_t *am, bool *ok){
    derr_t e = E_OK;

    *ok = false;

    string_builder_t inst = am_inst(am);

    // check if the file exists
    bool exists;
    PROP(&e, exists_path(&inst, &exists) );
    if(!exists) return e;

    // read the file
    derr_t e2 = installation_read_path(inst, &am->inst);
    CATCH(&e2, E_PARAM){
        LOG_ERROR(
            "corrupted installation file @%x:\n%x", FSB(inst), FD(e2.msg)
        );
        DROP_VAR(&e2);
        // delete the corrupted file
        DROP_CMD( dunlink_path(&inst) );
        return e;
    }else PROP_VAR(&e, &e2);

    // create the full domain from the subdomain
    PROP(&e,
        FMT(
            &am->fulldomain,
            "%x.user.splintermail.com",
            FD(am->inst.subdomain)
        )
    );

    // success!
    *ok = true;

    return e;
}

static derr_t load_account(acme_manager_t *am, bool *ok){
    derr_t e = E_OK;

    *ok = false;

    string_builder_t acct = am_acct(am);

    // check if the file exists
    bool exists;
    PROP(&e, exists_path(&acct, &exists) );
    if(!exists) return e;

    // read the file
    derr_t e2 = acme_account_from_path(&am->acct, acct);
    CATCH(&e2, E_PARAM){
        LOG_ERROR("corrupted account file @%x:\n%x", FSB(acct), FD(e2.msg));
        DROP_VAR(&e2);
        // delete the corrupted file
        DROP_CMD( dunlink_path(&acct) );
        return e;
    }else PROP_VAR(&e, &e2);

    // success!
    *ok = true;

    // delete the intermediate file if it is still around
    string_builder_t jwk = am_jwk(am);
    DROP_CMD( dunlink_path(&jwk) );

    return e;
}

void am_advance_state(acme_manager_t *am){
    acme_manager_i *ami = am->ami;

    if(is_error(am->e)) goto cu;

    derr_t e = E_OK;

    // waiting to be configured still?
    if(!am->configured){
        if(in_backoff(am)) return;
        bool ok;
        PROP_GO(&e, load_installation(am, &ok), fail);
        if(!ok){
            // still not configured, check again later
            backoff(am, 5);
            return;
        }
        LOG_INFO("installation is now configured\n");
        am->configured = true;
        am->want_cert = true;
    }

    // creating a new account?
    if(!am->accounted){
        // are we in retry backoff?
        if(in_backoff(am)) return;
        // retry forever; should only need to finish once per installation
        ONCE(am->new_acct_sent){
            // generate or load a key from file
            key_i *key;
            PROP_GO(&e, jwk_gen_or_load(am_jwk(am), &key), fail);
            // use the key to request an account
            ami->new_account(ami, &key, am->inst.email);
        }
        if(!am->new_acct_done) return;
        // persist to file
        PROP_GO(&e, acme_account_to_path(am->acct, am_acct(am)), fail);
        am->accounted = true;
    }

    // did the cert just expire?
    if(am->cert_active && ami->now(ami) >= am->expiry){
        // cert just expired
        am->update_cb(am->cb_data, NULL);
        am->cert_active = false;
        am->want_cert = true;
        // did the caller close us?
        if(is_error(am->e)) goto cu;
    }

    // is it now time to renew?
    if(!am->want_cert && ami->now(ami) >= am->renewal){
        am->want_cert = true;
        ami->deadline_cert(ami, am->expiry);
    }

    // do we need a new cert?
    if(am->want_cert){
        // wait for an in-flight unprepare to finish
        if(am->unprepare_sent && !am->unprepare_done) return;

        // are we backing off still?
        if(in_backoff(am)) return;

        /* advance_new_cert() does not do any retries on its own.

           The reason is that the complexity cost outweights the potential
           benefits.  Each ACME call needs to be evaluated for idempotency.
           For example, list_orders is safe, but new_order is obviously not,
           and neither are challenge or finalize.  Also, the logic to recover
           from those failures adds lots of complexity; new_order failures
           require re-running the list/check logic, challenge failures require
           rerunning get_authz, finalize requires get_order in a new location
           that didn't previously exist.

           So instead, we just configure one restart loop outside of
           advance_new_cert, and any failure causes the reload logic to
           retrigger. */
        bool ok;
        derr_t e2 = advance_new_cert(am, &ok);
        if(is_network_or_resp(e2.type)){
            // ACME client failed
            LOG_ERROR("Failure in ACME client:\n");
            DUMP(e2);
            DROP_VAR(&e2);
            time_t delay = get_backoff(
                ++am->failures, 15, 30, 60, 5*60, 15*60, 30*60
            );
            backoff(am, delay);
            time_t display;
            char *unit;
            if(delay < 60){
                unit = "seconds";
                display = delay;
            }else if(delay < 120){
                unit = "minute";
                display = delay/60;
            }else{
                unit = "minutes";
                display = delay/60;
            }
            LOG_ERROR("trying again in %x %x...\n", FI(display), FS(unit));
            // we'll re-enter the reload logic, since we had a failure
            am->reloaded = false;
            return;
        }else PROP_VAR_GO(&e, &e2, fail);
        if(!ok) return;

        LOG_ERROR("obtained new ACME cert\n");
        am->want_cert = false;
        am->failures = 0;

        // report the new certificate
        ssl_context_t ctx;
        PROP_GO(&e,
            ssl_context_new_server_path(&ctx, am_cert(am), am_key(am)),
        fail);
        am->update_cb(am->cb_data, ctx.ctx);
        am->cert_active = true;
        // did the caller close us?
        if(is_error(am->e)) goto cu;

        // configure our timer to wake us up when the cert expires
        PROP_GO(&e, check_expiry(am_cert(am), &am->expiry), fail);
        am->renewal = get_renewal(am->expiry);
        if(ami->now(ami) >= am->renewal){
            ORIG_GO(&e,
                E_INTERNAL,
                "have fresh cert with expiry time in < 15 days",
                fail
            );
        }
        ami->deadline_cert(ami, am->renewal);
    }

    // have we cleaned up after the last prepare?  (even from an old process?)
    if(!am->unprepare_done){
        if(ami->now(ami) < am->unprepare_after) return;
        ONCE(am->unprepare_sent){
            PROP_GO(&e, pre_apic_call(am), fail);
            ami->unprepare(ami, am->inst.token, &am->json);
        }
        return;
    }

    return;

fail:
    TRACE_PROP_VAR(&am->e, &e);

cu:
    // close our resources and cancel inflight requests
    if(!am->close_done){
        ONCE(am->close_sent) ami->close(ami);
        return;
    }

    // prep for done_cb
    derr_t err = am->e;
    acme_manager_done_cb done_cb = am->done_cb;
    void *cb_data = am->cb_data;

    // free memory
    am_free(am);

    // make done_cb
    done_cb(cb_data, err);
}

derr_t acme_manager_init(
    acme_manager_t *am,
    acme_manager_i *ami,
    string_builder_t acme_dir,
    acme_manager_update_cb update_cb,
    acme_manager_done_cb done_cb,
    void *cb_data,
    SSL_CTX **initial_ctx
){
    derr_t e = E_OK;

    *am = (acme_manager_t){
        .ami = ami,
        .acme_dir = acme_dir,
        .update_cb = update_cb,
        .done_cb = done_cb,
        .cb_data = cb_data,
    };

    // do we have an installation?
    bool ok;
    PROP_GO(&e, load_installation(am, &ok), fail);
    if(!ok){
        // configure for filesystem polling
        LOG_INFO("installation is not configured, cannot generate tls cert\n");
        backoff(am, 5);
        return e;
    }

    am->configured = true;

    // do we have an acme account yet?
    PROP_GO(&e, load_account(am, &ok), fail);
    if(!ok){
        return e;
    }

    am->accounted = true;

    // finish renaming if we crashed mid-rename
    PROP_GO(&e, rename_new_key_and_cert(am, true), fail);

    // do we have cert and key yet?
    string_builder_t cert = am_cert(am);
    string_builder_t key = am_key(am);
    PROP_GO(&e, exists_path(&cert, &ok), fail);
    if(!ok) goto want_cert;
    PROP_GO(&e, exists_path(&key, &ok), fail);
    if(!ok) goto want_cert;

    // check the expiration on the cert
    bool ours;
    PROP_GO(&e, check_cert(cert, &am->expiry, am->fulldomain, &ours), fail);
    if(!ours) goto want_cert;
    am->renewal = get_renewal(am->expiry);
    // certificate already expired?
    if(ami->now(ami) >= am->expiry) goto want_cert;

    // certificate is valid, load initial cert and key
    ssl_context_t ctx;
    PROP_GO(&e, ssl_context_new_server_path(&ctx, cert, key), fail);
    *initial_ctx = ctx.ctx;
    am->cert_active = true;

    // is our cert due for renewal?
    if(ami->now(ami) >= am->renewal){
        // start renewal process, as well as expiry timer
        ami->deadline_cert(ami, am->expiry);
        goto want_cert;
    }

    // configure our timer to wake us up when we should renew
    ami->deadline_cert(ami, am->renewal);

    return e;

want_cert:
    am->want_cert = true;
    return e;

fail:
    am_free(am);
    return e;
}

void am_close(acme_manager_t *am){
    if(is_error(am->e)) return;
    am->e.type = E_CANCELED;
}
