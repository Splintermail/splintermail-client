#include "libcitm/libcitm.h"

#include "test/test_utils.h"

#include <openssl/ssl.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#define DAY 86400

static char *inst1 =
    "{"
    "  \"email\": \"me@yo.com\","
    "  \"secret\": \"shhh\","
    "  \"subdomain\": \"yomamma\","
    "  \"token\": 777"
    "}";

static char *fulldomain = "yomamma.user.splintermail.com";

static char *k1 =
    "-----BEGIN RSA PRIVATE KEY-----\n"
    "MIICXAIBAAKBgQClSBGeRBYAqTXg7a7sMCEfDgd8IceNWgahGVMNdrXy6KuhyZpD\n"
    "Ektchn1X+bvBxBLTIASKQu6+/qrIG09O5WID8iUnmUBPRXw2Nkq4M0Bl8nEpA4yA\n"
    "/OulzvbOC/lChW6l4avBOFsUgOPS8TTXB1lz48LceivlTlzAmryj0k1SvwIDAQAB\n"
    "AoGAJvtddGd4jQVbuMY2SGx9k1Z6u3M4HoWKJLrVVMqX1Ue9GzZyVLle3t61d2dm\n"
    "OigljHfzuixZJODppLKUPPZq7EQbCFMf/JfydIIS+TAOfkTumU+sfpOT61IFFJy1\n"
    "F4j4nURU/ugNd0/G9bRQkyN1IelbigDEqY23WRY5l4erkIECQQDa8nkQK2s+TXMt\n"
    "zjXDBCe+j9fAVX4g2bub6VPC0KH/l0bzs13Pt/cbMh1UNawYW324oEJ3Qm7Vo3J5\n"
    "0Fcd/H/BAkEAwUCesxPvTtRTS++LnQIFml5S7kmsJ+7BV3/HVghG/LitZs2FjxaQ\n"
    "IqE9Hp+dFC+bzDXft0/HJ96SuB+hKqZyfwJAID+4rzYF/M5BmcyWxJu3Eu1y9ORi\n"
    "yInphURvzvEV+2SX0ctolPDe18Wg5kdlisznDqukPpatZpBFi0WxnzwSwQJAJV9b\n"
    "0YHva61AFpVSyKBVeQ74tsBknKxdk5BQemDLaaBFWHYyeAODCE9qRHvVosu6M3nP\n"
    "s+GnwBwY/6aOQnNB/QJBAIv+QX1FTiwlAIWO7rVln4KyOnn46jKcuABmUN4+eH1r\n"
    "VitlzVsFr0gvm/vvY2/PP5NmlbJONos1+MCz4HszN90=\n"
    "-----END RSA PRIVATE KEY-----\n";

static char *acct1 =
    "{"
    "  \"key\": {"
    "    \"crv\": \"P-256\","
    "    \"kty\": \"EC\","
    "    \"x\": \"ld3hMB2e_JD8Yn8u_FS76pjX3uRenrcWut-CKVi33bw\","
    "    \"y\": \"uL4CozKllAT0eTmGdpGQ2u5FQdu49K_QjMVywMOrifY\","
    "    \"d\": \"y2deb3RTFPTaU_7T-uTwds_mddZu7wiwelLMRNYA7oU\""
    "  },"
    "  \"kid\": \"kid1\","
    "  \"orders\": \"orders1\""
    "}";

static char *jwk1 =
    "{"
    "  \"crv\": \"P-256\","
    "  \"kty\": \"EC\","
    "  \"x\": \"ld3hMB2e_JD8Yn8u_FS76pjX3uRenrcWut-CKVi33bw\","
    "  \"y\": \"uL4CozKllAT0eTmGdpGQ2u5FQdu49K_QjMVywMOrifY\","
    "  \"d\": \"y2deb3RTFPTaU_7T-uTwds_mddZu7wiwelLMRNYA7oU\""
    "}";

static char *thumb1 = "tJZ4TbWuh3ceHFD74n9nxAzMFvVjVULRLwpa1WN7Sd4";

// returns a cert which is loadable, but otherwise totally bogus
static derr_t mkcert(char *key, dstr_t domain, time_t expiry, dstr_t *out){
    derr_t e = E_OK;

    EVP_PKEY *pkey = NULL;
    X509 *cert = NULL;
    ASN1_TIME *asn1time = NULL;
    ASN1_INTEGER *asn1int = NULL;
    BIO* bio = NULL;

    // read the key
    PROP_GO(&e, read_pem_encoded_privkey(dstr_from_cstr(key), &pkey), cu);

    cert = X509_new();
    if(!cert) ORIG_GO(&e, E_NOMEM, "nomem", cu);

    int ret = X509_set_version(cert, 0);
    if(ret != 1) ORIG_GO(&e, E_SSL, "X509_set_version: %x\n", cu, FSSL);

    // set begin time to Jan 1, 1970
    asn1time = ASN1_TIME_set(NULL, 0);
    if(!asn1time) ORIG_GO(&e, E_NOMEM, "nomem", cu);
    ret = X509_set1_notBefore(cert, asn1time);
    if(ret != 1) ORIG_GO(&e, E_SSL, "X509_set1_notBefore: %x\n", cu, FSSL);
    ASN1_TIME_free(asn1time);
    asn1time = NULL;

    // set expiry time
    asn1time = ASN1_TIME_set(NULL, expiry);
    if(!asn1time) ORIG_GO(&e, E_NOMEM, "nomem", cu);
    ret = X509_set1_notAfter(cert, asn1time);
    if(ret != 1) ORIG_GO(&e, E_SSL, "X509_set1_notAfter: %x\n", cu, FSSL);

    // configure the subject name
    // (name is still owned by the X509)
    X509_NAME *name = X509_get_subject_name(cert);
    unsigned char *udomain = (unsigned char*)domain.data;
    if(domain.len > INT_MAX){
        ORIG_GO(&e, E_INTERNAL, "domain is way too long", cu);
    }
    int udomainlen = (int)domain.len;
    ret = X509_NAME_add_entry_by_txt(
        name, "CN", MBSTRING_ASC, udomain, udomainlen, -1, 0
    );
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "X509_NAME_add_entry_by_txt: %x", cu, FSSL);
    }

    // we'll self-sign, so issuer matches subject
    ret = X509_set_issuer_name(cert, name);
    if(ret != 1) ORIG_GO(&e, E_SSL, "X509_set_issuer_name: %x", cu, FSSL);

    // set pubkey
    ret = X509_set_pubkey(cert, pkey);
    if(ret != 1) ORIG_GO(&e, E_SSL, "X509_set_pubkey: %x", cu, FSSL);

    // set serial
    asn1int = ASN1_INTEGER_new();
    if(!asn1int) ORIG_GO(&e, E_NOMEM, "nomem", cu);
    ret = ASN1_INTEGER_set(asn1int, 1);
    if(ret != 1) ORIG_GO(&e, E_SSL, "ASN1_INTEGER_set: %x", cu, FSSL);
    ret = X509_set_serialNumber(cert, asn1int);
    if(ret != 1) ORIG_GO(&e, E_SSL, "X509_set_serialNumber: %x", cu, FSSL);

    // sign the cert
    ret = X509_sign(cert, pkey, EVP_sha256());
    if(ret == 0) ORIG_GO(&e, E_SSL, "X509_sign: %x", cu, FSSL);

    // now get the PEM encoded version of the cert

    bio = BIO_new(BIO_s_mem());
    if(!bio) ORIG_GO(&e, E_NOMEM, "nomem", cu);

    ret = PEM_write_bio_X509(bio, cert);
    if(!ret) ORIG_GO(&e, E_SSL, "PEM_write_bio_X509: %x", cu, FSSL);

    char* ptr;
    long bio_len = BIO_get_mem_data(bio, &ptr);
    if(bio_len < 1) ORIG_GO(&e, E_SSL, "BIO_get_mem_data: %x", cu, FSSL);

    dstr_t dptr = dstr_from_cstrn(ptr, (size_t)bio_len, false);
    PROP_GO(&e, dstr_copy2(dptr, out), cu);

cu:
    EVP_PKEY_free(pkey);
    X509_free(cert);
    ASN1_TIME_free(asn1time);
    ASN1_INTEGER_free(asn1int);
    BIO_free(bio);
    return e;
}

typedef enum {
    DEADLINE_CERT,
    DEADLINE_BACKOFF,
    DEADLINE_UNPREPARE,
    PREPARE,
    UNPREPARE,
    KEYGEN,
    NEW_ACCOUNT,
    NEW_ORDER,
    GET_ORDER,
    LIST_ORDERS,
    GET_AUTHZ,
    CHALLENGE,
    CHALLENGE_FINISH,
    FINALIZE,
    FINALIZE_FROM_PROCESSING,
    FINALIZE_FROM_VALID,
    CLOSE,
    // not part of acme_manager_i
    UPDATE,
    DONE,
} call_e;

static char *call_type_names[] = {
    "DEADLINE_CERT",
    "DEADLINE_BACKOFF",
    "DEADLINE_UNPREPARE",
    "PREPARE",
    "UNPREPARE",
    "KEYGEN",
    "NEW_ACCOUNT",
    "NEW_ORDER",
    "GET_ORDER",
    "LIST_ORDERS",
    "GET_AUTHZ",
    "CHALLENGE",
    "CHALLENGE_FINISH",
    "FINALIZE",
    "FINALIZE_FROM_PROCESSING",
    "FINALIZE_FROM_VALID",
    "CLOSE",
    // not part of acme_manager_i
    "UPDATE",
    "DONE",
};

typedef union {
    time_t deadline_cert;  // when
    time_t deadline_backoff;  // when
    time_t deadline_unprepare;  // when
    char *prepare;  // proof
    struct {
        char *key_thumb;  // NULL for "don't care"
        char *email;
    } new_account;
    char *new_order;  // domain
    char *get_order;  // order
    char *get_authz;  // authz
    struct {
        char *authz;
        char *challenge;
    } challenge;
    struct {
        char *authz;
        time_t retry_after;
    } challenge_finish;
    struct {
        char *order;
        char *finalize;
        char *domain;
    } finalize;
    struct {
        char *order;
        time_t retry_after;
    } finalize_from_processing;
    char *finalize_from_valid; // certurl
    bool update;               // ctx != NULL
} call_u;

typedef struct {
    call_e type;
    call_u u;
} call_t;

typedef struct {
    acme_manager_i iface;
    acme_manager_t am;
    derr_t e;

    call_t calls[8];
    size_t nexp;
    size_t ngot;

    char buf[256];
    string_builder_t tmp;
    time_t now;

    // capture json from prepare() and unprepare()
    json_t *json;

    // capture key from new_account()
    key_i *key;

    // automatically track what calls are pending before a close_done()
    bool want_prepare;
    bool want_unprepare;
    bool want_keygen;
    bool want_new_account;
    bool want_new_order;
    bool want_get_order;
    bool want_list_orders;
    bool want_get_authz;
    bool want_challenge;
    bool want_finalize;

    bool alive;
    bool close_sent;
    bool closed_by_am;
    bool unexpected_close;
    bool no_close_check;
    derr_t done_err;
} globals_t;

DEF_CONTAINER_OF(globals_t, iface, acme_manager_i)

// return bool ok
static bool expect_call(globals_t *g, call_e type, call_t *out){
    *out = (call_t){0};
    if(is_error(g->e)) return false;
    if(g->ngot >= g->nexp){
        for(size_t i = 0; i < g->nexp; i++){
            TRACE(&g->e,
                "after expected call to %x,\n",
                FS(call_type_names[g->calls[i].type])
            );
        }
        TRACE(&g->e,
            "got unexpected expected call to %x!\n",
            FS(call_type_names[type])
        );
        TRACE_ORIG(&g->e, E_VALUE, "too many calls");
        LOG_ERROR("failure occured\n");
        return false;
    }
    size_t i = g->ngot++;
    call_t call = g->calls[i];
    if(call.type != type){
        TRACE_ORIG(&g->e,
            E_VALUE,
            "expected call[%x] to be %x but got %x",
            FU(i),
            FS(call_type_names[call.type]),
            FS(call_type_names[type])
        );
        LOG_ERROR("failure occured\n");
        return false;
    }
    *out = call;
    return true;
}

static time_t g_now(acme_manager_i *iface){
    globals_t *g = CONTAINER_OF(iface, globals_t, iface);
    return g->now;
}

static void g_deadline_cert(acme_manager_i *iface, time_t when){
    globals_t *g = CONTAINER_OF(iface, globals_t, iface);
    call_t call;
    if(!expect_call(g, DEADLINE_CERT, &call)) return;
    EXPECT_I_GO(&g->e,
        "deadline_cert(when=)", when, call.u.deadline_cert, done
    );
done:
    if(is_error(g->e)) LOG_ERROR("failure occured\n");
    return;
}

static void g_deadline_backoff(acme_manager_i *iface, time_t when){
    globals_t *g = CONTAINER_OF(iface, globals_t, iface);
    call_t call;
    if(!expect_call(g, DEADLINE_BACKOFF, &call)) return;
    EXPECT_I_GO(&g->e,
        "deadline_backoff(when=)", when, call.u.deadline_backoff, done
    );
done:
    if(is_error(g->e)) LOG_ERROR("failure occured\n");
    return;
}

static void g_deadline_unprepare(acme_manager_i *iface, time_t when){
    globals_t *g = CONTAINER_OF(iface, globals_t, iface);
    call_t call;
    if(!expect_call(g, DEADLINE_UNPREPARE, &call)) return;
    EXPECT_I_GO(&g->e,
        "deadline_unprepare(when=)", when, call.u.deadline_unprepare, done
    );
done:
    if(is_error(g->e)) LOG_ERROR("failure occured\n");
    return;
}


// splintermail API calls
static void g_prepare(
    acme_manager_i *iface, api_token_t token, json_t *json, dstr_t proof
){
    globals_t *g = CONTAINER_OF(iface, globals_t, iface);
    g->want_prepare = true;
    g->json = json;
    (void)token;
    (void)json;
    call_t call;
    if(!expect_call(g, PREPARE, &call)) return;
    EXPECT_D_GO(&g->e,
        "prepare(proof=)", proof, dstr_from_cstr(call.u.prepare), done
    );
done:
    if(is_error(g->e)) LOG_ERROR("failure occured\n");
    return;
}

static void g_unprepare(acme_manager_i *iface, api_token_t token, json_t *json){
    globals_t *g = CONTAINER_OF(iface, globals_t, iface);
    g->want_unprepare = true;
    g->json = json;
    (void)token;
    call_t call;
    if(!expect_call(g, UNPREPARE, &call)) return;
}


// off-thread work
static derr_t g_keygen(acme_manager_i *iface, string_builder_t path){
    globals_t *g = CONTAINER_OF(iface, globals_t, iface);
    g->want_keygen = true;
    call_t call;
    if(!expect_call(g, KEYGEN, &call)) return E_OK;
    DSTR_VAR(got, 256);
    PROP_GO(&g->e, FMT(&got, "%x", FSB(path)), done);
    DSTR_VAR(exp, 256);
    PROP_GO(&g->e, FMT(&exp, "%x/keynew.pem", FSB(g->tmp)), done);
    EXPECT_D_GO(&g->e, "keygen(path=)", got, exp, done);
done:
    if(is_error(g->e)) LOG_ERROR("failure occured\n");
    return E_OK;
}

// acme calls
static void g_new_account(
    acme_manager_i *iface, key_i **key, const dstr_t email
){
    globals_t *g = CONTAINER_OF(iface, globals_t, iface);
    g->want_new_account = true;
    g->key = *key;
    *key = NULL;
    call_t call;
    if(!expect_call(g, NEW_ACCOUNT, &call)) return;
    if(call.u.new_account.key_thumb != NULL){
        DSTR_VAR(bin, 256);
        PROP_GO(&g->e, jwk_thumbprint(g->key, &bin), done);
        DSTR_VAR(got, 256);
        PROP_GO(&g->e, bin2b64url(bin, &got), done);
        dstr_t exp = dstr_from_cstr(call.u.new_account.key_thumb);
        EXPECT_D3_GO(&g->e, "new_account(key=)", got, exp, done);
    }
    dstr_t exp_email = dstr_from_cstr(call.u.new_account.email);
    EXPECT_D_GO(&g->e, "new_order(email=)", email, exp_email, done);
done:
    if(is_error(g->e)) LOG_ERROR("failure occured\n");
    return;
}

static void g_new_order(
    acme_manager_i *iface, const acme_account_t acct, const dstr_t domain
){
    globals_t *g = CONTAINER_OF(iface, globals_t, iface);
    g->want_new_order = true;
    (void)acct;
    call_t call;
    if(!expect_call(g, NEW_ORDER, &call)) return;
    EXPECT_D_GO(&g->e,
        "new_order(domain=)", domain, dstr_from_cstr(call.u.new_order), done
    );
done:
    if(is_error(g->e)) LOG_ERROR("failure occured\n");
    return;
}

static void g_get_order(
    acme_manager_i *iface, const acme_account_t acct, const dstr_t order
){
    globals_t *g = CONTAINER_OF(iface, globals_t, iface);
    g->want_get_order = true;
    (void)acct;
    call_t call;
    if(!expect_call(g, GET_ORDER, &call)) return;
    EXPECT_D_GO(&g->e,
        "get_order(order=)", order, dstr_from_cstr(call.u.get_order), done
    );
done:
    if(is_error(g->e)) LOG_ERROR("failure occured\n");
    return;
}

static void g_list_orders(acme_manager_i *iface, const acme_account_t acct){
    globals_t *g = CONTAINER_OF(iface, globals_t, iface);
    g->want_list_orders = true;
    (void)acct;
    call_t call;
    expect_call(g, LIST_ORDERS, &call);
}

static void g_get_authz(
    acme_manager_i *iface, const acme_account_t acct, const dstr_t authz
){
    globals_t *g = CONTAINER_OF(iface, globals_t, iface);
    g->want_get_authz = true;
    (void)acct;
    call_t call;
    if(!expect_call(g, GET_AUTHZ, &call)) return;
    EXPECT_D_GO(&g->e,
        "get_authz(authz=)", authz, dstr_from_cstr(call.u.get_authz), done
    );
done:
    if(is_error(g->e)) LOG_ERROR("failure occured\n");
    return;
}

static void g_challenge(
    acme_manager_i *iface,
    const acme_account_t acct,
    const dstr_t authz,
    const dstr_t challenge
){
    globals_t *g = CONTAINER_OF(iface, globals_t, iface);
    g->want_challenge = true;
    (void)acct;
    call_t call;
    if(!expect_call(g, CHALLENGE, &call)) return;
    EXPECT_D_GO(&g->e,
        "challenge(authz=)",
        authz,
        dstr_from_cstr(call.u.challenge.authz),
        done
    );
    EXPECT_D_GO(&g->e,
        "challenge(challenge=)",
        challenge,
        dstr_from_cstr(call.u.challenge.challenge),
        done
    );
done:
    if(is_error(g->e)) LOG_ERROR("failure occured\n");
    return;
}

static void g_challenge_finish(
    acme_manager_i *iface,
    const acme_account_t acct,
    const dstr_t authz,
    time_t retry_after
){
    globals_t *g = CONTAINER_OF(iface, globals_t, iface);
    g->want_challenge = true;
    (void)acct;
    call_t call;
    if(!expect_call(g, CHALLENGE_FINISH, &call)) return;
    EXPECT_D_GO(&g->e,
        "challenge_finish(authz=)",
        authz,
        dstr_from_cstr(call.u.challenge_finish.authz),
        done
    );
    EXPECT_I_GO(&g->e,
        "challenge_finish(retry_after=)",
        retry_after,
        call.u.challenge_finish.retry_after,
        done
    );
done:
    if(is_error(g->e)) LOG_ERROR("failure occured\n");
    return;
}

static void g_finalize(
    acme_manager_i *iface,
    const acme_account_t acct,
    const dstr_t order,
    const dstr_t finalize,
    const dstr_t domain,
    EVP_PKEY *pkey  // increments the refcount
){
    globals_t *g = CONTAINER_OF(iface, globals_t, iface);
    g->want_finalize = true;
    (void)acct;
    call_t call;
    if(!expect_call(g, FINALIZE, &call)) return;
    EXPECT_D_GO(&g->e,
        "finalize(order=)",
        order,
        dstr_from_cstr(call.u.finalize.order),
        done
    );
    EXPECT_D_GO(&g->e,
        "finalize(finalize=)",
        finalize,
        dstr_from_cstr(call.u.finalize.finalize),
        done
    );
    EXPECT_D_GO(&g->e,
        "finalize(domain=)",
        domain,
        dstr_from_cstr(call.u.finalize.domain),
        done
    );
    EXPECT_NOT_NULL_GO(&g->e, "finalize(pkey=)", pkey, done);
done:
    if(is_error(g->e)) LOG_ERROR("failure occured\n");
    return;
}

static void g_finalize_from_processing(
    acme_manager_i *iface,
    const acme_account_t acct,
    const dstr_t order,
    time_t retry_after
){
    globals_t *g = CONTAINER_OF(iface, globals_t, iface);
    g->want_finalize = true;
    call_t call;
    (void)acct;
    if(!expect_call(g, FINALIZE_FROM_PROCESSING, &call)) return;
    EXPECT_D_GO(&g->e,
        "finalize_from_processing(order=)",
        order,
        dstr_from_cstr(call.u.finalize_from_processing.order),
        done
    );
    EXPECT_I_GO(&g->e,
        "finalize_from_processing(retry_after=)",
        retry_after,
        call.u.finalize_from_processing.retry_after,
        done
    );
done:
    if(is_error(g->e)) LOG_ERROR("failure occured\n");
    return;
}

static void g_finalize_from_valid(
    acme_manager_i *iface, const acme_account_t acct, const dstr_t certurl
){
    globals_t *g = CONTAINER_OF(iface, globals_t, iface);
    g->want_finalize = true;
    (void)acct;
    call_t call;
    if(!expect_call(g, FINALIZE_FROM_VALID, &call)) return;
    EXPECT_D_GO(&g->e,
        "finalize_from_valid(certurl=)",
        certurl,
        dstr_from_cstr(call.u.finalize_from_valid),
        done
    );
done:
    if(is_error(g->e)) LOG_ERROR("failure occured\n");
    return;
}

// will trigger an E_CANCELED callback for any action that is in-flight
static void g_closed_by_am(acme_manager_i *iface){
    globals_t *g = CONTAINER_OF(iface, globals_t, iface);
    g->closed_by_am = true;
    if(g->no_close_check) return;
    call_t call;
    bool g_was_failing = is_error(g->e);
    expect_call(g, CLOSE, &call);
    if(!g->close_sent && !g_was_failing && is_error(g->e)){
        // an unexpected close; prefer the done_cb error when it arrives
        g->unexpected_close = true;
    }
}

static void g_update_cb(void *data, SSL_CTX *ctx){
    globals_t *g = data;
    call_t call;
    if(!expect_call(g, UPDATE, &call)) goto done;
    if(call.u.update){
        EXPECT_NOT_NULL_GO(&g->e, "update_cb(ctx=)", ctx, done);
    }else{
        EXPECT_NULL_GO(&g->e, "update_cb(ctx=)", ctx, done);
    }
done:
    if(is_error(g->e)) LOG_ERROR("failure occured\n");
    SSL_CTX_free(ctx);
    return;
}

static void g_done_cb(void *data, derr_t err){
    globals_t *g = data;
    g->alive = false;
    g->done_err = err;
    if(g->no_close_check) return;
    call_t call;
    expect_call(g, DONE, &call);
}

static void add_call(globals_t *g, call_e type, call_u u){
    size_t cap = sizeof(g->calls) / sizeof(*g->calls);
    if(g->nexp >= cap){
        for(size_t i = 0; i < g->nexp; i++){
            LOG_ERROR("- %x\n", FS(call_type_names[g->calls[i].type]));
        }
        LOG_FATAL("too many expected calls (%x)!", FS(call_type_names[type]));
    }
    g->calls[g->nexp++] = (call_t){type, u};
}
#define CALL_U(...) (call_u){__VA_ARGS__}

#define EXPECT_DEADLINE_CERT(g, when) \
    add_call(g, DEADLINE_CERT, CALL_U(.deadline_cert = when))
#define EXPECT_DEADLINE_BACKOFF(g, when) \
    add_call(g, DEADLINE_BACKOFF, CALL_U(.deadline_backoff = when))
#define EXPECT_DEADLINE_UNPREPARE(g, when) \
    add_call(g, DEADLINE_UNPREPARE, CALL_U(.deadline_unprepare = when))
#define EXPECT_PREPARE(g, proof) \
    add_call(g, PREPARE, CALL_U(.prepare = proof))
#define EXPECT_UNPREPARE(g) \
    add_call(g, UNPREPARE, CALL_U(0))
#define EXPECT_KEYGEN(g) \
    add_call(g, KEYGEN, CALL_U(0))
#define EXPECT_NEW_ACCOUNT(g, key, email) \
    add_call(g, NEW_ACCOUNT, CALL_U(.new_account = {key, email}))
#define EXPECT_NEW_ORDER(g, domain) \
    add_call(g, NEW_ORDER, CALL_U(.new_order = domain))
#define EXPECT_GET_ORDER(g, order) \
    add_call(g, GET_ORDER, CALL_U(.get_order = order))
#define EXPECT_LIST_ORDERS(g) \
    add_call(g, LIST_ORDERS, CALL_U(0))
#define EXPECT_GET_AUTHZ(g, authz) \
    add_call(g, GET_AUTHZ, CALL_U(.get_authz = authz))
#define EXPECT_CHALLENGE(g, authz, chlng) \
    add_call(g, CHALLENGE, CALL_U(.challenge = {authz, chlng}))
#define EXPECT_CHALLENGE_FINISH(g, authz, retry_after) \
    add_call(g, \
        CHALLENGE_FINISH, CALL_U(.challenge_finish = {authz, retry_after}) \
    )
#define EXPECT_FINALIZE(g, order, fnlz, domain) \
    add_call(g, FINALIZE, CALL_U(.finalize = {order, fnlz, domain}))
#define EXPECT_FINALIZE_FROM_PROCESSING(g, order, retry_after) \
    add_call(g, \
        FINALIZE_FROM_PROCESSING, \
        CALL_U(.finalize_from_processing = {order, retry_after}) \
    )
#define EXPECT_FINALIZE_FROM_VALID(g, certurl) \
    add_call(g, FINALIZE_FROM_VALID, CALL_U(.finalize_from_valid = certurl))
#define EXPECT_CLOSE(g) \
    add_call(g, CLOSE, CALL_U(0))
#define EXPECT_UPDATE(g, nonnull_cert) \
    add_call(g, UPDATE, CALL_U(.update = nonnull_cert))
#define EXPECT_DONE(g) \
    add_call(g, DONE, CALL_U(0))

static derr_t run(globals_t *g){
    derr_t e = E_OK;

    am_advance_state(&g->am);

    // check for existing error
    PROP_VAR(&e, &g->e);
    // ensure we made the right number of calls
    if(g->ngot < g->nexp){
        for(size_t i = g->ngot; i < g->nexp; i++){
            TRACE(&e,
                "expected %x call but didn't see it\n",
                FS(call_type_names[g->calls[i].type])
            );
        }
        ORIG(&e, E_VALUE, "missing expected calls");
    }
    // reset for next call
    g->ngot = 0;
    g->nexp = 0;

    return e;
}

static derr_t g_prepare_done(globals_t *g, derr_type_t etype, jdump_i *resp){
    derr_t e = E_OK;

    if(resp){
        DSTR_VAR(buf, 4096);
        PROP(&e, jdump(resp, WD(&buf), 2) );
        PROP(&e, json_parse(buf, g->json) );
    }

    derr_t err = { .type = etype };
    am_prepare_done(&g->am, err, g->json);
    g->want_prepare = false;

    PROP(&e, run(g) );

    return e;
}

static derr_t g_unprepare_done(globals_t *g, derr_type_t etype, jdump_i *resp){
    derr_t e = E_OK;

    if(resp){
        DSTR_VAR(buf, 4096);
        PROP(&e, jdump(resp, WD(&buf), 2) );
        PROP(&e, json_parse(buf, g->json) );
    }

    derr_t err = { .type = etype };
    am_unprepare_done(&g->am, err, g->json);
    g->want_unprepare = false;

    PROP(&e, run(g) );

    return e;
}

static derr_t g_keygen_done(globals_t *g, derr_type_t etype, char *key){
    derr_t e = E_OK;

    EVP_PKEY *pkey = NULL;
    if(key != NULL){
        dstr_t dkey = dstr_from_cstr(key);
        // write the key to a file
        string_builder_t path = sb_append(&g->tmp, SBS("keynew.pem"));
        PROP(&e, dstr_write_path(&path, &dkey) );
        // also load the key
        PROP(&e, read_pem_encoded_privkey(dkey, &pkey) );
    }

    derr_t err = { .type = etype };
    am_keygen_done(&g->am, err, pkey);
    g->want_keygen = false;

    PROP(&e, run(g) );

    return e;
}

static derr_t g_new_account_done(
    globals_t *g, derr_type_t etype, const char *kid, const char *orders
){
    derr_t e = E_OK;

    dstr_t dkid = {0};
    dstr_t dorders = {0};
    key_i *key = NULL;

    if(!etype){
        key = g->key;
    }else if(g->key){
        g->key->free(g->key);
        g->key = NULL;
    }

    derr_t err = { .type = etype };
    PROP_GO(&e, dstr_copystr(kid, &dkid), fail);
    PROP_GO(&e, dstr_copystr(orders, &dorders), fail);

    acme_account_t acct = {key, dkid, dorders};
    key = NULL;
    am_new_account_done(&g->am, err, acct);
    g->want_new_account = false;

    PROP(&e, run(g) );

    return e;

fail:
    dstr_free(&dkid);
    dstr_free(&dorders);
    if(key) key->free(key);
    return e;
}

static derr_t g_new_order_done(globals_t *g,
    derr_type_t etype,
    // allocated strings are returned
    const char *order,
    const char *expires,
    const char *authorization,
    const char *finalize
){
    derr_t e = E_OK;

    dstr_t dorder = {0};
    dstr_t dexpires = {0};
    dstr_t dauthz = {0};
    dstr_t dfinalize = {0};

    derr_t err = { .type = etype };
    PROP_GO(&e, dstr_copystr(order, &dorder), fail);
    PROP_GO(&e, dstr_copystr(expires, &dexpires), fail);
    PROP_GO(&e, dstr_copystr(authorization, &dauthz), fail);
    PROP_GO(&e, dstr_copystr(finalize, &dfinalize), fail);
    am_new_order_done(&g->am, err, dorder, dexpires, dauthz, dfinalize);
    g->want_new_order = false;

    PROP(&e, run(g) );

    return e;

fail:
    dstr_free(&dorder);
    dstr_free(&dexpires);
    dstr_free(&dauthz);
    dstr_free(&dfinalize);
    return e;
}

static derr_t g_get_order_done(globals_t *g,
    derr_type_t etype,
    // allocated strings are returned
    acme_status_e status,
    const char *domain,
    const char *expires,
    const char *authorization,
    const char *finalize,
    const char *certurl,     // might be empty
    time_t retry_after  // might be zero
){
    derr_t e = E_OK;

    dstr_t ddomain = {0};
    dstr_t dexpires = {0};
    dstr_t dauthz = {0};
    dstr_t dfinalize = {0};
    dstr_t dcerturl = {0};

    derr_t err = { .type = etype };
    PROP_GO(&e, dstr_copystr(domain, &ddomain), fail);
    PROP_GO(&e, dstr_copystr(expires, &dexpires), fail);
    PROP_GO(&e, dstr_copystr(authorization, &dauthz), fail);
    PROP_GO(&e, dstr_copystr(finalize, &dfinalize), fail);
    PROP_GO(&e, dstr_copystr(certurl, &dcerturl), fail);
    am_get_order_done(
        &g->am,
        err,
        status,
        ddomain,
        dexpires,
        dauthz,
        dfinalize,
        dcerturl,
        retry_after
    );
    g->want_get_order = false;

    PROP(&e, run(g) );

    return e;

fail:
    dstr_free(&ddomain);
    dstr_free(&dexpires);
    dstr_free(&dauthz);
    dstr_free(&dfinalize);
    dstr_free(&dcerturl);
    return e;
}

// allocated list of allocated strings is returned
static derr_t _g_list_orders_done(
    globals_t *g, derr_type_t etype, char **strs, size_t n
){
    derr_t e = E_OK;

    LIST(dstr_t) dlist = {0};

    derr_t err = { .type = etype };
    PROP_GO(&e, LIST_NEW(dstr_t, &dlist, n), fail);
    for(size_t i = 0; i < n; i++){
        PROP_GO(&e, dstr_copystr(strs[i], &dlist.data[dlist.len++]), fail);
    }
    am_list_orders_done(&g->am, err, dlist);
    g->want_list_orders = false;

    PROP(&e, run(g) );

    return e;

fail:
    for(size_t i = 0; i < dlist.len; i++){
        dstr_free(&dlist.data[i]);
    }
    LIST_FREE(dstr_t, &dlist);
    return e;
}

#define g_list_orders_done(g, etype, ...) \
    _g_list_orders_done((g), (etype), \
        &(char*[]){NULL, __VA_ARGS__}[1], \
        sizeof((char*[]){NULL, __VA_ARGS__})/sizeof(char*) - 1 \
    )

static derr_t g_get_authz_done(globals_t *g,
    derr_type_t etype,
    acme_status_e status,
    acme_status_e challenge_status,
    // allocated strings are returned
    const char *domain,
    const char *expires,
    const char *challenge,   // only the dns challenge is returned
    const char *token,       // only the dns challenge is returned
    time_t retry_after       // might be zero
){
    derr_t e = E_OK;

    dstr_t ddomain = {0};
    dstr_t dexpires = {0};
    dstr_t dchallenge = {0};
    dstr_t dtoken = {0};

    derr_t err = { .type = etype };
    PROP_GO(&e, dstr_copystr(domain, &ddomain), fail);
    PROP_GO(&e, dstr_copystr(expires, &dexpires), fail);
    PROP_GO(&e, dstr_copystr(challenge, &dchallenge), fail);
    PROP_GO(&e, dstr_copystr(token, &dtoken), fail);
    am_get_authz_done(
        &g->am,
        err,
        status,
        challenge_status,
        ddomain,
        dexpires,
        dchallenge,
        dtoken,
        retry_after
    );
    g->want_get_authz = false;

    PROP(&e, run(g) );

    return e;

fail:
    dstr_free(&ddomain);
    dstr_free(&dexpires);
    dstr_free(&dchallenge);
    dstr_free(&dtoken);
    return e;
}

static derr_t g_challenge_done(globals_t *g, derr_type_t etype){
    derr_t e = E_OK;

    derr_t err = { .type = etype };
    am_challenge_done(&g->am, err);
    g->want_challenge = false;

    PROP(&e, run(g) );

    return e;
}

static derr_t g_finalize_done(
    globals_t *g, derr_type_t etype, const dstr_t cert
){
    derr_t e = E_OK;

    dstr_t certcpy = {0};

    derr_t err = { .type = etype };
    PROP(&e, dstr_copy2(cert, &certcpy) );
    am_finalize_done(&g->am, err, certcpy);
    g->want_finalize = false;

    PROP(&e, run(g) );

    return e;
}

//

static derr_t g_close(globals_t *g, derr_type_t etype){
    derr_t e = E_OK;

    if(!g->closed_by_am) EXPECT_CLOSE(g);
    am_close(&g->am);
    g->close_sent = true;
    PROP(&e, run(g) );

    derr_type_t cncl = E_CANCELED;

    #define MAYBE_DONE(x, ...) \
        if(g->want_##x) PROP(&e, g_##x##_done(g, __VA_ARGS__) )

    MAYBE_DONE(prepare, cncl, NULL);
    MAYBE_DONE(unprepare, cncl, NULL);
    MAYBE_DONE(keygen, cncl, NULL);
    MAYBE_DONE(new_account, cncl, "", "");
    MAYBE_DONE(new_order, cncl, "", "", "", "");
    MAYBE_DONE(get_order, cncl, 0, "", "", "", "", "", 0);
    MAYBE_DONE(get_authz, cncl, 0, 0, "", "", "", "", 0);
    MAYBE_DONE(challenge, cncl);
    MAYBE_DONE(finalize, cncl, (dstr_t){0});
    // list_orders is special because of the nested __VA_ARGS__
    if(g->want_list_orders) PROP(&e, g_list_orders_done(g, cncl) );

    #undef MAYBE_DONE

    EXPECT_DONE(g);
    am_close_done(&g->am);
    PROP(&e, run(g) );

    // check we got the expected exit type
    EXPECT_E_VAR(&e, "done(err=)", &g->done_err, etype);

    return e;
}

static void forceclose(globals_t *g, derr_t *E){
    if(!g->alive) goto done;

    g->no_close_check = true;
    g->close_sent = true;
    am_close(&g->am);
    am_advance_state(&g->am);

    derr_type_t cncl = E_CANCELED;

    #define MAYBE_DONE(x, ...) \
        if(g->want_##x) DROP_CMD( g_##x##_done(g, __VA_ARGS__) )

    MAYBE_DONE(prepare, cncl, NULL);
    MAYBE_DONE(unprepare, cncl, NULL);
    MAYBE_DONE(keygen, cncl, NULL);
    MAYBE_DONE(new_account, cncl, "", "");
    MAYBE_DONE(new_order, cncl, "", "", "", "");
    MAYBE_DONE(get_order, cncl, 0, "", "", "", "", "", 0);
    MAYBE_DONE(get_authz, cncl, 0, 0, "", "", "", "", 0);
    MAYBE_DONE(challenge, cncl);
    MAYBE_DONE(finalize, cncl, (dstr_t){0});
    // list_orders is special because of the nested __VA_ARGS__
    if(g->want_list_orders) DROP_CMD( g_list_orders_done(g, cncl) );

    #undef MAYBE_DONE

    am_close_done(&g->am);
    am_advance_state(&g->am);

    if(g->alive) LOG_ERROR("forceclose didn't see a done_cb!\n");

done:
    // choose acme_manager's error if it closed us, otherwise prefer our error
    if(g->unexpected_close){
        derr_t e2 = E_OK;
        TRACE_MULTIPROP_VAR(&e2, &g->done_err, E);
        *E = e2;
    }else{
        TRACE_MULTIPROP_VAR(E, &g->done_err);
    }
    // if g->e is set, we must have hit something more important
    DROP_VAR(&g->e);

    return;
}

static derr_t globals_init(globals_t *g){
    derr_t e = E_OK;

    *g = (globals_t){
        .iface = (acme_manager_i){
            .now = g_now,
            .deadline_cert = g_deadline_cert,
            .deadline_backoff = g_deadline_backoff,
            .deadline_unprepare = g_deadline_unprepare,
            .prepare = g_prepare,
            .unprepare = g_unprepare,
            .keygen = g_keygen,
            .new_account = g_new_account,
            .new_order = g_new_order,
            .get_order = g_get_order,
            .list_orders = g_list_orders,
            .get_authz = g_get_authz,
            .challenge = g_challenge,
            .challenge_finish = g_challenge_finish,
            .finalize = g_finalize,
            .finalize_from_processing = g_finalize_from_processing,
            .finalize_from_valid = g_finalize_from_valid,
            .close = g_closed_by_am,
        },
        .now = 1,
    };

    // create temporary directory
    dstr_t dtmp;
    DSTR_WRAP_ARRAY(dtmp, g->buf);
    PROP(&e, mkdir_temp("test_acme_manager", &dtmp) );
    g->tmp = SBD(dtmp);

    return e;
}

static derr_t start(globals_t *g, bool exp_initial_ctx){
    derr_t e = E_OK;

    // if this is a second start, reset shutdown logic
    g->unexpected_close = false;
    g->no_close_check = false;
    g->close_sent = false;
    g->closed_by_am = false;
    // well... don't drop the error; assert it is clear instead
    PROP_VAR(&e, &g->done_err);

    SSL_CTX *initial_ctx = NULL;

    PROP_GO(&e,
        acme_manager_init(&g->am,
            &g->iface,
            g->tmp,
            g_update_cb,
            g_done_cb,
            g,
            &initial_ctx
        ),
    cu);

    g->alive = true;

    if(exp_initial_ctx){
        EXPECT_NOT_NULL_GO(&e, "initial_ctx", initial_ctx, cu);
    }else{
        EXPECT_NULL_GO(&e, "initial_ctx", initial_ctx, cu);
    }

    PROP_GO(&e, run(g), cu);

cu:
    SSL_CTX_free(initial_ctx);

    return e;
}

/*
   Test each of:
    - clean-start-to-finish cert creation
    - polling filesystem for installation
    - expiry and renewal timers under various conditions
    - unprepare under various conditions
*/
static derr_t test_am_advance_state(void){
    derr_t e = E_OK;

    DSTR_VAR(c1, 4096);

    globals_t g;
    PROP_GO(&e, globals_init(&g), cu);

    // no installation configured
    EXPECT_DEADLINE_BACKOFF(&g, g.now+5);
    PROP_GO(&e, start(&g, false), cu);

    // still nothing configured
    g.now += 5;
    EXPECT_DEADLINE_BACKOFF(&g, g.now+5);
    PROP_GO(&e, run(&g), cu);

    // now configure something
    PROP_GO(&e,
        dstr_write_path2(
            dstr_from_cstr(inst1), sb_append(&g.tmp, SBS("installation.json"))
        ),
    cu);

    // then configure an account
    g.now += 5;
    EXPECT_NEW_ACCOUNT(&g, NULL, "me@yo.com");
    PROP_GO(&e, run(&g), cu);

    // expect reload logic
    EXPECT_KEYGEN(&g);
    EXPECT_LIST_ORDERS(&g);
    PROP_GO(&e, g_new_account_done(&g, E_NONE, "k1", "orders"), cu);

    // respond as if account were fresh
    EXPECT_NEW_ORDER(&g, fulldomain);
    PROP_GO(&e, g_list_orders_done(&g, E_NONE), cu);

    EXPECT_GET_AUTHZ(&g, "z1");
    PROP_GO(&e, g_new_order_done(&g, E_NONE, "o1", "x", "z1", "f1"), cu);

    EXPECT_PREPARE(&g, "t1");
    PROP_GO(&e,
        g_get_authz_done(&g,
            E_NONE,
            ACME_PENDING,
            ACME_PENDING,
            fulldomain,
            "x",
            "c1",
            "t1",  // note libacme would add a thumbprint
            0
        ),
    cu);

    EXPECT_CHALLENGE(&g, "z1", "c1");
    jdump_i *prep_resp = DOBJ(
        DKEY("status", DS("success")),
        DKEY("contents", DOBJ(
            DKEY("result", DS("ok")),
        )),
    );
    PROP_GO(&e, g_prepare_done(&g, E_NONE, prep_resp), cu);

    // noop while we wait for keygen to finish
    PROP_GO(&e, g_challenge_done(&g, E_NONE), cu);

    EXPECT_FINALIZE(&g, "o1", "f1", fulldomain);
    PROP_GO(&e, g_keygen_done(&g, E_NONE, k1), cu);

    time_t expiry = g.now + 90*DAY;
    time_t renewal = g.now + 75*DAY;
    PROP_GO(&e, mkcert(k1, dstr_from_cstr(fulldomain), expiry, &c1), cu);
    EXPECT_UPDATE(&g, true);
    EXPECT_DEADLINE_CERT(&g, renewal);
    EXPECT_UNPREPARE(&g);
    PROP_GO(&e, g_finalize_done(&g, E_NONE, c1), cu);

    jdump_i *unprep_resp = DOBJ(
        DKEY("status", DS("success")),
        DKEY("contents", DS("ok")),
    );
    PROP_GO(&e, g_unprepare_done(&g, E_NONE, unprep_resp), cu);

    // shut it down and restart it
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);
    EXPECT_DEADLINE_CERT(&g, renewal);
    EXPECT_UNPREPARE(&g);
    PROP_GO(&e, start(&g, true), cu);

    PROP_GO(&e, g_unprepare_done(&g, E_NONE, unprep_resp), cu);

    // wait for renewal time, expect new deadline and reload logic
    g.now = renewal;
    EXPECT_DEADLINE_CERT(&g, expiry);
    EXPECT_KEYGEN(&g);
    EXPECT_LIST_ORDERS(&g);
    PROP_GO(&e, run(&g), cu);

    // pretend expiry occurs
    g.now = expiry;
    EXPECT_UPDATE(&g, false);
    EXPECT_NEW_ORDER(&g, fulldomain);
    PROP_GO(&e, g_list_orders_done(&g, E_NONE), cu);

    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

cu:
    // close the acme_manager if it isn't closed already
    forceclose(&g, &e);

    // cleanup our tempdir
    DROP_CMD( rm_rf_path(&g.tmp) );

    return e;
}

/*
   Test each of:
    - clean start
    - start with installation but no account
    - start with installation and jwk but no account
    - start with installation and account but no key/cert
    - start with installation and account and key/cert
    - start with installation and account and renewable key/cert
    - start with installation and account and expired key/cert
    - start with installation and account and key/cert which are not ours
    - start with installation and account and keynew but no certnew
    - start with installation and account and keynew and half-written certnew
    - start with installation and account and keynew and certnew
    - start with installation and account and certnew but no keynew
*/
static derr_t test_acme_manager_init(void){
    derr_t e = E_OK;

    time_t expiry = 90*DAY;
    time_t renewal = 75*DAY;
    DSTR_VAR(mycert, 4096);
    DSTR_VAR(notmycert, 4096);
    PROP(&e, mkcert(k1, dstr_from_cstr(fulldomain), expiry, &mycert) );
    PROP(&e, mkcert(k1, DSTR_LIT("wrong"), expiry, &notmycert) );

    globals_t g;
    PROP_GO(&e, globals_init(&g), cu);

    string_builder_t inst = sb_append(&g.tmp, SBS("installation.json"));
    string_builder_t jwk = sb_append(&g.tmp, SBS("jwk.json"));
    string_builder_t acct = sb_append(&g.tmp, SBS("account.json"));
    string_builder_t key = sb_append(&g.tmp, SBS("key.pem"));
    string_builder_t keynew = sb_append(&g.tmp, SBS("keynew.pem"));
    string_builder_t cert = sb_append(&g.tmp, SBS("cert.pem"));
    string_builder_t certnew = sb_append(&g.tmp, SBS("certnew.pem"));

    // clean start
    EXPECT_DEADLINE_BACKOFF(&g, g.now+5);
    PROP_GO(&e, start(&g, false), cu);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

    // start with installation but no account
    PROP_GO(&e, dstr_write_path2(dstr_from_cstr(inst1), inst), cu);
    EXPECT_NEW_ACCOUNT(&g, NULL, "me@yo.com");
    PROP_GO(&e, start(&g, false), cu);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

    // start with installation and jwk but no account
    PROP_GO(&e, dstr_write_path2(dstr_from_cstr(jwk1), jwk), cu);
    EXPECT_NEW_ACCOUNT(&g, thumb1, "me@yo.com");
    PROP_GO(&e, start(&g, false), cu);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

    // start with installation and account but no key/cert
    PROP_GO(&e, dstr_write_path2(dstr_from_cstr(acct1), acct), cu);
    EXPECT_KEYGEN(&g);
    EXPECT_LIST_ORDERS(&g);
    PROP_GO(&e, start(&g, false), cu);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

    // start with installation and account and key/cert
    PROP_GO(&e, dstr_write_path2(dstr_from_cstr(k1), key), cu);
    PROP_GO(&e, dstr_write_path2(mycert, cert), cu);
    g.now = 0;
    EXPECT_DEADLINE_CERT(&g, renewal);
    EXPECT_UNPREPARE(&g);
    PROP_GO(&e, start(&g, true), cu);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

    // start with installation and account and renewable key/cert
    g.now = renewal;
    EXPECT_DEADLINE_CERT(&g, expiry);
    EXPECT_KEYGEN(&g);
    EXPECT_LIST_ORDERS(&g);
    PROP_GO(&e, start(&g, true), cu);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

    // start with installation and account and expired key/cert
    g.now = expiry;
    EXPECT_KEYGEN(&g);
    EXPECT_LIST_ORDERS(&g);
    PROP_GO(&e, start(&g, false), cu);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);
    g.now = 0;

    // start with installation and account and key/cert which are not ours
    PROP_GO(&e, dstr_write_path2(notmycert, cert), cu);
    EXPECT_KEYGEN(&g);
    EXPECT_LIST_ORDERS(&g);
    PROP_GO(&e, start(&g, false), cu);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

    // start with installation and account and keynew but no certnew
    g.now = renewal;
    PROP_GO(&e, dstr_write_path2(mycert, cert), cu);
    PROP_GO(&e, dstr_write_path2(DSTR_LIT("badkey"), keynew), cu);
    EXPECT_DEADLINE_CERT(&g, expiry);
    EXPECT_KEYGEN(&g);
    EXPECT_LIST_ORDERS(&g);
    PROP_GO(&e, start(&g, true), cu);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

    // start with installation and account and keynew and half-written certnew
    g.now = renewal;
    dstr_t halfmycert = dstr_sub2(mycert, 0, mycert.len/2);
    PROP_GO(&e, dstr_write_path2(DSTR_LIT("badkey"), keynew), cu);
    PROP_GO(&e, dstr_write_path2(halfmycert, certnew), cu);
    EXPECT_DEADLINE_CERT(&g, expiry);
    EXPECT_KEYGEN(&g);
    EXPECT_LIST_ORDERS(&g);
    PROP_GO(&e, start(&g, true), cu);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

    // start with installation and account and keynew and certnew
    g.now = 0;
    PROP_GO(&e, dstr_write_path2(DSTR_LIT("badkey"), key), cu);
    PROP_GO(&e, dstr_write_path2(DSTR_LIT("badcert"), cert), cu);
    PROP_GO(&e, dstr_write_path2(dstr_from_cstr(k1), keynew), cu);
    PROP_GO(&e, dstr_write_path2(mycert, certnew), cu);
    EXPECT_DEADLINE_CERT(&g, renewal);
    EXPECT_UNPREPARE(&g);
    PROP_GO(&e, start(&g, true), cu);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

    // start with installation and account and certnew but no keynew
    g.now = 0;
    PROP_GO(&e, dstr_write_path2(dstr_from_cstr(k1), key), cu);
    PROP_GO(&e, dstr_write_path2(DSTR_LIT("badcert"), cert), cu);
    DROP_CMD( dunlink_path(&keynew) );
    PROP_GO(&e, dstr_write_path2(mycert, certnew), cu);
    EXPECT_DEADLINE_CERT(&g, renewal);
    EXPECT_UNPREPARE(&g);
    PROP_GO(&e, start(&g, true), cu);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

cu:
    // close the acme_manager if it isn't closed already
    forceclose(&g, &e);

    // cleanup our tempdir
    DROP_CMD( rm_rf_path(&g.tmp) );

    return e;
}

/*
   Test each of:
    - ignoring existing orders which don't match the domain
    - reject all invalid orders
    - stop listing when finding VALID order
    - list_orders finds an ACME_PROCESSING order
    - list_orders finds an ACME_READY order
    - list_orders finds a PENDING order, get_authz finds PENDING
    - get_authz finds an ACME_PROCESSING status
    - get_authz finds an ACME_VALID status
    - get_authz finds an ACME_INVALID status
*/
static derr_t test_advance_new_cert(void){
    derr_t e = E_OK;

    globals_t g;
    PROP_GO(&e, globals_init(&g), cu);

    string_builder_t inst = sb_append(&g.tmp, SBS("installation.json"));
    string_builder_t acct = sb_append(&g.tmp, SBS("account.json"));

    // start by configuring an installation and an account
    PROP_GO(&e, dstr_write_path2(dstr_from_cstr(inst1), inst), cu);
    PROP_GO(&e, dstr_write_path2(dstr_from_cstr(acct1), acct), cu);

    #define WRONG_ORDER_DONE(status) \
        PROP_GO(&e, \
            g_get_order_done( \
                &g, E_NONE, status, "wrongdomain", "x", "a", "f", "c", 0 \
            ), \
        cu)

    #define GET_ORDER_DONE(status, suffix, retry_after) \
        PROP_GO(&e, \
            g_get_order_done( \
                &g, \
                E_NONE, \
                status, \
                fulldomain, \
                "x" suffix, \
                "a" suffix, \
                "f" suffix, \
                "c" suffix, \
                retry_after \
            ), \
        cu)

    // ignoring existing orders which don't match the domain
    EXPECT_KEYGEN(&g);
    EXPECT_LIST_ORDERS(&g);
    PROP_GO(&e, start(&g, false), cu);
    EXPECT_GET_ORDER(&g, "o1");
    PROP_GO(&e, g_list_orders_done(&g, E_NONE, "o1", "o2", "o3", "o4", "o5"), cu);
    EXPECT_GET_ORDER(&g, "o2");
    WRONG_ORDER_DONE(ACME_INVALID);
    EXPECT_GET_ORDER(&g, "o3");
    WRONG_ORDER_DONE(ACME_PENDING);
    EXPECT_GET_ORDER(&g, "o4");
    WRONG_ORDER_DONE(ACME_READY);
    EXPECT_GET_ORDER(&g, "o5");
    WRONG_ORDER_DONE(ACME_VALID);
    EXPECT_NEW_ORDER(&g, fulldomain);
    WRONG_ORDER_DONE(ACME_PROCESSING);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

    // reject all invalid orders
    EXPECT_KEYGEN(&g);
    EXPECT_LIST_ORDERS(&g);
    PROP_GO(&e, start(&g, false), cu);
    EXPECT_GET_ORDER(&g, "o1");
    PROP_GO(&e, g_list_orders_done(&g, E_NONE, "o1", "o2", "o3", "o4"), cu);
    EXPECT_GET_ORDER(&g, "o2");
    GET_ORDER_DONE(ACME_INVALID, "1", 0);
    EXPECT_GET_ORDER(&g, "o3");
    GET_ORDER_DONE(ACME_REVOKED, "2", 0);
    EXPECT_GET_ORDER(&g, "o4");
    GET_ORDER_DONE(ACME_DEACTIVATED, "3", 0);
    EXPECT_NEW_ORDER(&g, fulldomain);
    GET_ORDER_DONE(ACME_EXPIRED, "4", 0);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

    // stop listing when finding VALID order
    EXPECT_KEYGEN(&g);
    EXPECT_LIST_ORDERS(&g);
    PROP_GO(&e, start(&g, false), cu);
    EXPECT_GET_ORDER(&g, "o1");
    PROP_GO(&e, g_list_orders_done(&g, E_NONE, "o1", "o2"), cu);
    EXPECT_FINALIZE_FROM_VALID(&g, "c1");
    GET_ORDER_DONE(ACME_VALID, "1", 0);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

    // list_orders finds an ACME_PROCESSING order
    EXPECT_KEYGEN(&g);
    EXPECT_LIST_ORDERS(&g);
    PROP_GO(&e, start(&g, false), cu);
    EXPECT_GET_ORDER(&g, "o1");
    PROP_GO(&e, g_list_orders_done(&g, E_NONE, "o1", "o2", "o3"), cu);
    EXPECT_GET_ORDER(&g, "o2");
    GET_ORDER_DONE(ACME_PENDING, "1", 0);
    EXPECT_GET_ORDER(&g, "o3");
    GET_ORDER_DONE(ACME_READY, "2", 0);
    EXPECT_FINALIZE_FROM_PROCESSING(&g, "o3", 9);
    GET_ORDER_DONE(ACME_PROCESSING, "3", 9);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

    // list_orders finds an ACME_READY order
    EXPECT_KEYGEN(&g);
    EXPECT_LIST_ORDERS(&g);
    PROP_GO(&e, start(&g, false), cu);
    EXPECT_GET_ORDER(&g, "o1");
    PROP_GO(&e, g_list_orders_done(&g, E_NONE, "o1", "o2", "o3"), cu);
    EXPECT_GET_ORDER(&g, "o2");
    GET_ORDER_DONE(ACME_PENDING, "1", 0);
    EXPECT_GET_ORDER(&g, "o3");
    GET_ORDER_DONE(ACME_READY, "2", 7);
    GET_ORDER_DONE(ACME_INVALID, "3", 0);
    EXPECT_FINALIZE(&g, "o2", "f2", fulldomain);
    PROP_GO(&e, g_keygen_done(&g, E_NONE, k1), cu);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

    #define GET_AUTHZ_DONE(status, challenge_status, suffix, retry_after) \
        PROP_GO(&e, \
            g_get_authz_done( \
                &g, \
                E_NONE, \
                status, \
                challenge_status, \
                fulldomain, \
                "x" suffix, \
                "c" suffix, \
                "t" suffix, \
                retry_after \
            ), \
        cu)

    // list_orders finds a PENDING order, get_authz finds PENDING
    EXPECT_KEYGEN(&g);
    EXPECT_LIST_ORDERS(&g);
    PROP_GO(&e, start(&g, false), cu);
    EXPECT_GET_ORDER(&g, "o1");
    PROP_GO(&e, g_list_orders_done(&g, E_NONE, "o1", "o2", "o3"), cu);
    EXPECT_GET_ORDER(&g, "o2");
    GET_ORDER_DONE(ACME_EXPIRED, "1", 0);
    EXPECT_GET_ORDER(&g, "o3");
    GET_ORDER_DONE(ACME_PENDING, "2", 7);
    EXPECT_GET_AUTHZ(&g, "a2");
    GET_ORDER_DONE(ACME_INVALID, "3", 0);
    EXPECT_PREPARE(&g, "t2");
    GET_AUTHZ_DONE(ACME_PENDING, ACME_PENDING, "2", 0);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

    // get_authz finds an ACME_PROCESSING status
    EXPECT_KEYGEN(&g);
    EXPECT_LIST_ORDERS(&g);
    PROP_GO(&e, start(&g, false), cu);
    EXPECT_GET_ORDER(&g, "o1");
    PROP_GO(&e, g_list_orders_done(&g, E_NONE, "o1"), cu);
    EXPECT_GET_AUTHZ(&g, "a1");
    GET_ORDER_DONE(ACME_PENDING, "1", 0);
    EXPECT_CHALLENGE_FINISH(&g, "a1", 5);
    GET_AUTHZ_DONE(ACME_PENDING, ACME_PROCESSING, "2", 5);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

    // get_authz finds an ACME_VALID status
    EXPECT_KEYGEN(&g);
    EXPECT_LIST_ORDERS(&g);
    PROP_GO(&e, start(&g, false), cu);
    EXPECT_GET_ORDER(&g, "o1");
    PROP_GO(&e, g_list_orders_done(&g, E_NONE, "o1"), cu);
    EXPECT_GET_AUTHZ(&g, "a1");
    GET_ORDER_DONE(ACME_PENDING, "1", 0);
    GET_AUTHZ_DONE(ACME_PENDING, ACME_VALID, "2", 5);
    EXPECT_FINALIZE(&g, "o1", "f1", fulldomain);
    PROP_GO(&e, g_keygen_done(&g, E_NONE, k1), cu);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

    // get_authz finds an ACME_INVALID status
    EXPECT_KEYGEN(&g);
    EXPECT_LIST_ORDERS(&g);
    PROP_GO(&e, start(&g, false), cu);
    EXPECT_GET_ORDER(&g, "o1");
    PROP_GO(&e, g_list_orders_done(&g, E_NONE, "o1"), cu);
    EXPECT_GET_AUTHZ(&g, "a1");
    GET_ORDER_DONE(ACME_PENDING, "1", 0);
    EXPECT_DEADLINE_BACKOFF(&g, g.now + 15);
    GET_AUTHZ_DONE(ACME_INVALID, ACME_INVALID, "2", 5);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

cu:
    // close the acme_manager if it isn't closed already
    forceclose(&g, &e);

    // cleanup our tempdir
    DROP_CMD( rm_rf_path(&g.tmp) );

    return e;
}

/* Test each of:
   - get_backoff() full behavior
   - new account retries
   - prepare retries in timeout scenarios
   - unprepare has independent timeout
   - unprepare timeout is reset after new cert
   - some non-retryable failure in new_cert causes acme_manager to crash
*/
static derr_t test_retries(void){
    derr_t e = E_OK;

    globals_t g;
    PROP_GO(&e, globals_init(&g), cu);

    string_builder_t inst = sb_append(&g.tmp, SBS("installation.json"));
    string_builder_t acct = sb_append(&g.tmp, SBS("account.json"));
    string_builder_t key = sb_append(&g.tmp, SBS("key.pem"));
    string_builder_t cert = sb_append(&g.tmp, SBS("cert.pem"));

    // start by configuring an installation
    PROP_GO(&e, dstr_write_path2(dstr_from_cstr(inst1), inst), cu);

    // walk through full backoff sequence, testing new account retries
    EXPECT_NEW_ACCOUNT(&g, NULL, "me@yo.com");
    PROP_GO(&e, start(&g, false), cu);
    EXPECT_DEADLINE_BACKOFF(&g, g.now + 1);
    PROP_GO(&e, g_new_account_done(&g, E_SOCK, NULL, NULL), cu);
    g.now += 1;
    EXPECT_DEADLINE_BACKOFF(&g, g.now + 5);
    PROP_GO(&e, g_new_account_done(&g, E_CONN, NULL, NULL), cu);
    g.now += 5;
    EXPECT_DEADLINE_BACKOFF(&g, g.now + 15);
    PROP_GO(&e, g_new_account_done(&g, E_CONN, NULL, NULL), cu);
    g.now += 15;
    EXPECT_DEADLINE_BACKOFF(&g, g.now + 30);
    PROP_GO(&e, g_new_account_done(&g, E_CONN, NULL, NULL), cu);
    g.now += 30;
    EXPECT_DEADLINE_BACKOFF(&g, g.now + 45);
    PROP_GO(&e, g_new_account_done(&g, E_CONN, NULL, NULL), cu);
    g.now += 45;
    EXPECT_DEADLINE_BACKOFF(&g, g.now + 60);
    PROP_GO(&e, g_new_account_done(&g, E_CONN, NULL, NULL), cu);
    g.now += 60;
    EXPECT_DEADLINE_BACKOFF(&g, g.now + 60);
    PROP_GO(&e, g_new_account_done(&g, E_CONN, NULL, NULL), cu);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

    // reconfigure with an account
    PROP_GO(&e, dstr_write_path2(dstr_from_cstr(acct1), acct), cu);

    // test prepare retries (to timeout, not to failure)
    jdump_i *prep_timeout = DOBJ(
        DKEY("status", DS("success")),
        DKEY("contents", DOBJ(
            DKEY("result", DS("timeout")),
        )),
    );
    jdump_i *bad_resp = DOBJ(
        DKEY("status", DS("error")),
        DKEY("contents", DS("i died")),
    );
    EXPECT_KEYGEN(&g);
    EXPECT_LIST_ORDERS(&g);
    PROP_GO(&e, start(&g, false), cu);
    EXPECT_NEW_ORDER(&g, fulldomain);
    PROP_GO(&e, g_list_orders_done(&g, E_NONE), cu);
    EXPECT_GET_AUTHZ(&g, "z1");
    PROP_GO(&e, g_new_order_done(&g, E_NONE, "o1", "x", "z1", "f1"), cu);
    EXPECT_PREPARE(&g, "t1");
    PROP_GO(&e,
        g_get_authz_done(&g,
            E_NONE,
            ACME_PENDING,
            ACME_PENDING,
            fulldomain,
            "x",
            "c1",
            "t1",  // note libacme would add a thumbprint
            0
        ),
    cu);
    // emit timeout
    EXPECT_PREPARE(&g, "t1");
    PROP_GO(&e, g_prepare_done(&g, E_NONE, prep_timeout), cu);
    EXPECT_PREPARE(&g, "t1");
    PROP_GO(&e, g_prepare_done(&g, E_NONE, prep_timeout), cu);
    // emit failure response
    EXPECT_DEADLINE_BACKOFF(&g, g.now + 15);
    PROP_GO(&e, g_prepare_done(&g, E_NONE, bad_resp), cu);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

    // test unprepare has independent timeout
    time_t expiry = 90*DAY;
    time_t renewal = 75*DAY;
    DSTR_VAR(mycert, 4096);
    PROP(&e, mkcert(k1, dstr_from_cstr(fulldomain), expiry, &mycert) );
    PROP_GO(&e, dstr_write_path2(dstr_from_cstr(k1), key), cu);
    PROP_GO(&e, dstr_write_path2(mycert, cert), cu);

    // unprepare retries work
    g.now = renewal - 10*60 - 100;  // 100 seconds before 10 min before renewal
    EXPECT_DEADLINE_CERT(&g, renewal);
    EXPECT_UNPREPARE(&g);
    PROP_GO(&e, start(&g, true), cu);
    EXPECT_DEADLINE_UNPREPARE(&g, g.now + 10*60);
    PROP_GO(&e, g_unprepare_done(&g, E_NOMEM, NULL), cu); // any failure at all
    g.now += 10*60;
    EXPECT_DEADLINE_UNPREPARE(&g, g.now + 10*60);
    PROP_GO(&e, g_unprepare_done(&g, E_NONE, bad_resp), cu);

    // unprepare timeout is independent of other timers
    g.now += 100;
    EXPECT_DEADLINE_CERT(&g, expiry);
    EXPECT_KEYGEN(&g);
    EXPECT_LIST_ORDERS(&g);
    PROP_GO(&e, run(&g), cu);

    // unprepare timeout is independent
    EXPECT_NEW_ORDER(&g, fulldomain);
    PROP_GO(&e, g_list_orders_done(&g, E_NONE), cu);
    EXPECT_GET_AUTHZ(&g, "z1");
    PROP_GO(&e, g_new_order_done(&g, E_NONE, "o1", "x", "z1", "f1"), cu);
    EXPECT_PREPARE(&g, "t1");
    PROP_GO(&e,
        g_get_authz_done(&g,
            E_NONE,
            ACME_PENDING,
            ACME_PENDING,
            fulldomain,
            "x",
            "c1",
            "t1",  // note libacme would add a thumbprint
            0
        ),
    cu);
    EXPECT_CHALLENGE(&g, "z1", "c1");
    jdump_i *prep_resp = DOBJ(
        DKEY("status", DS("success")),
        DKEY("contents", DOBJ(
            DKEY("result", DS("ok")),
        )),
    );
    PROP_GO(&e, g_prepare_done(&g, E_NONE, prep_resp), cu);
    PROP_GO(&e, g_challenge_done(&g, E_NONE), cu);
    EXPECT_FINALIZE(&g, "o1", "f1", fulldomain);
    PROP_GO(&e, g_keygen_done(&g, E_NONE, k1), cu);
    DSTR_VAR(c1, 4096);
    PROP_GO(&e, mkcert(k1, dstr_from_cstr(fulldomain), g.now + 90*DAY, &c1), cu);
    EXPECT_UPDATE(&g, true);
    EXPECT_DEADLINE_CERT(&g, g.now + 75*DAY);
    EXPECT_UNPREPARE(&g);  // this is the key!
    PROP_GO(&e, g_finalize_done(&g, E_NONE, c1), cu);
    PROP_GO(&e, g_close(&g, E_CANCELED), cu);

    // some non-retryable failure in new_cert causes acme_manager to crash
    g.now += 90*DAY;
    EXPECT_KEYGEN(&g);
    EXPECT_LIST_ORDERS(&g);
    PROP_GO(&e, start(&g, false), cu);
    // E_CONN is retryable
    EXPECT_DEADLINE_BACKOFF(&g, g.now + 15);
    PROP_GO(&e, g_list_orders_done(&g, E_CONN), cu);
    g.now += 15;
    // E_SOCK is retryable
    EXPECT_DEADLINE_BACKOFF(&g, g.now + 30);
    PROP_GO(&e, g_list_orders_done(&g, E_SOCK), cu);
    g.now += 30;
    // E_RESPONSE is retryable
    EXPECT_DEADLINE_BACKOFF(&g, g.now + 60);
    PROP_GO(&e, g_list_orders_done(&g, E_RESPONSE), cu);
    g.now += 60;
    // but nothing else is retryable
    EXPECT_CLOSE(&g);
    PROP_GO(&e, g_list_orders_done(&g, E_NOMEM), cu);
    PROP_GO(&e, g_close(&g, E_NOMEM), cu);

cu:
    // close the acme_manager if it isn't closed already
    forceclose(&g, &e);

    // cleanup our tempdir
    DROP_CMD( rm_rf_path(&g.tmp) );

    return e;
}

static derr_t test_keygen_or_load(void){
    derr_t e = E_OK;

    EVP_PKEY *pkey = NULL;
    EVP_PKEY *pkey_exp = NULL;

    dstr_t dk1 = dstr_from_cstr(k1);

    // create temporary directory
    DSTR_VAR(dtmp, 256);
    PROP(&e, mkdir_temp("test_acme_manager", &dtmp) );
    string_builder_t tmp = SBD(dtmp);
    string_builder_t path = sb_append(&tmp, SBS("key.pem"));

    // load a good key
    PROP_GO(&e, dstr_write_path2(dk1, path), cu);
    PROP_GO(&e, keygen_or_load(path, &pkey), cu);

    // make sure it's the right key
    PROP_GO(&e, read_pem_encoded_privkey(dk1, &pkey_exp), cu);
    EXPECT_I_GO(&e, "EVP_PKEY_eq", EVP_PKEY_eq(pkey, pkey_exp), 1, cu);

    EVP_PKEY_free(pkey);
    pkey = NULL;

    // ignore a bad key
    PROP_GO(&e, dstr_write_path2(DSTR_LIT("badkey"), path), cu);
    PROP_GO(&e, keygen_or_load(path, &pkey), cu);
    EXPECT_I_GO(&e, "EVP_PKEY_eq", EVP_PKEY_eq(pkey, pkey_exp), 0, cu);

    EVP_PKEY_free(pkey);
    pkey = NULL;

    // generate a new key
    PROP_GO(&e, dunlink_path(&path), cu);
    PROP_GO(&e, keygen_or_load(path, &pkey), cu);
    EXPECT_I_GO(&e, "EVP_PKEY_eq", EVP_PKEY_eq(pkey, pkey_exp), 0, cu);

cu:
    // cleanup our tempdir
    DROP_CMD( rm_rf_path(&tmp) );
    EVP_PKEY_free(pkey);
    EVP_PKEY_free(pkey_exp);
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    int exit_code = 0;

    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    PROP_GO(&e, ssl_library_init(), cu);

    PROP_GO(&e, test_am_advance_state(), cu);
    PROP_GO(&e, test_acme_manager_init(), cu);
    PROP_GO(&e, test_advance_new_cert(), cu);
    PROP_GO(&e, test_retries(), cu);
    PROP_GO(&e, test_keygen_or_load(), cu);

cu:
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
        LOG_ERROR("FAIL\n");
        exit_code = 1;
    }else{
        LOG_ERROR("PASS\n");
    }

    ssl_library_close();

    return exit_code;
}
