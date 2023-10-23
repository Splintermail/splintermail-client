#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "libdstr/libdstr.h"
#include "libcrypto/libcrypto.h"
#include "api_client.h"

REGISTER_ERROR_TYPE(E_TOKEN, "TOKEN", "invalid api token");
REGISTER_ERROR_TYPE(E_PASSWORD, "PASSWORD", "incorrect password");

void api_token_free0(api_token_t *token){
    dstr_free0(&token->secret);
    *token = (api_token_t){0};
}

derr_t api_token_read(const char *path, api_token_t *token){
    derr_t e = E_OK;
    derr_t e2;

    DSTR_VAR(creds, 1024);
    DSTR_VAR(jmem, 1024);
    json_node_t jnodes[32];
    size_t njnodes = sizeof(jnodes) / sizeof(*jnodes);

    // read the file into memory
    e2 = dstr_read_file(path, &creds);
    // if we got a fixedsize error it is not a valid file
    CATCH(&e2, E_FIXEDSIZE){
        LOG_WARN("api credential file seems too long, ignoring\n");
        RETHROW_GO(&e, &e2, E_PARAM, cu);
    }else PROP_VAR_GO(&e, &e2, cu);

    // try to parse the file contents as json
    // should just be token, secret, and nonce
    json_t json;
    json_prep_preallocated(&json, &jmem, jnodes, njnodes, true);

    e2 = json_parse(creds, &json);
    // if we got a fixedsize error it is not a valid file
    CATCH(&e2, E_FIXEDSIZE){
        LOG_WARN("api creds contain way too much json\n");
        RETHROW_GO(&e, &e2, E_PARAM, cu);
    }else PROP_VAR_GO(&e, &e2, cu);

    // now we can dereference things
    jspec_t *jspec = JOBJ(false,
        JKEY("nonce", JU64(&token->nonce)),
        JKEY("secret", JDCPY(&token->secret)),
        JKEY("token", JU(&token->key)),
    );
    PROP_GO(&e, jspec_read(jspec, json.root), cu);

cu:
    dstr_zeroize(&creds);
    dstr_zeroize(&jmem);
    api_token_free0(token);

    return e;
}

derr_t api_token_read_path(const string_builder_t *sb, api_token_t* token){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &stack, &heap, &path) );

    PROP_GO(&e, api_token_read(path->data, token), cu);

cu:
    dstr_free(&heap);
    return e;
}

derr_t api_token_write(api_token_t token, const char* path){
    derr_t e = E_OK;
    FILE *f = NULL;
    DSTR_VAR(tempstack, 256);
    dstr_t tempheap = {0};

    // build a tempfile path
    const char *temp;
    derr_type_t etype = FMT_QUIET(&tempstack, "%x.tmp", FS(path));
    if(etype == E_NONE){
        temp = tempstack.data;
    }else{
        PROP_GO(&e, FMT(&tempheap, "%x.tmp", FS(path)), cu);
        temp = tempheap.data;
    }

    // open the tempfile for writing
    PROP_GO(&e, dfopen(temp, "w", &f), cu);

    // write the file
    jdump_i *obj = DOBJ(
        DKEY("token", DU(token.key)),
        DKEY("secret", DD(token.secret)),
        DKEY("nonce", DU(token.nonce)),
    );
    PROP_GO(&e, jdump(obj, WF(f), 2), cu);

    // check error when closing writable file descriptor
    PROP_GO(&e, dfclose2(&f), cu);

    // rename temp file into place
    PROP_GO(&e, drename_atomic(temp, path), cu);

cu:
    if(f) fclose(f);
    dstr_free(&tempheap);
    return e;
}

derr_t api_token_write_path(api_token_t token, const string_builder_t *sb){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &stack, &heap, &path) );

    PROP_GO(&e, api_token_write(token, path->data), cu);

cu:
    dstr_free(&heap);
    return e;
}

derr_t api_token_read_increment_write(
    const char *path, api_token_t *token, bool *ok
){
    derr_t e = E_OK;
    *ok = true;

    // try reading the path
    derr_t e2 = api_token_read(path, token);
    CATCH(&e2, E_PARAM){
        LOG_DEBUG("corrupted token file @%x:\n%x", FS(path), FD(e2.msg));
        DROP_VAR(&e2);
        DROP_CMD( dunlink(path) );
        *ok = false;
        return e;
    }else PROP_VAR_GO(&e, &e2, fail);

    // increment nonce
    token->nonce++;

    // write back to path
    PROP_GO(&e, api_token_write(*token, path), fail);

    return e;

fail:
    dstr_zeroize(&token->secret);
    return e;
}
derr_t api_token_read_increment_write_path(
    const string_builder_t *sb, api_token_t *token, bool *ok
){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &stack, &heap, &path) );

    PROP_GO(&e, api_token_read_increment_write(path->data, token, ok), cu);

cu:
    dstr_free(&heap);
    return e;
}

derr_t nonce_read_increment_write(const char *path, uint64_t *nonce){
    derr_t e = E_OK;

    *nonce = 0;

    // read file for initial value, if it exists
    bool ok;
    PROP(&e, dexists(path, &ok) );
    if(ok){
        DSTR_VAR(buf, 32);
        PROP(&e, dstr_read_file(path, &buf) );
        dstr_t stripped = dstr_strip_chars(buf, ' ', '\t', '\r', '\n');
        PROP(&e, dstr_tou64(&stripped, nonce, 10) );
    }

    // increment
    (*nonce)++;

    // write updated file
    FILE *f = NULL;
    DSTR_VAR(tempstack, 256);
    dstr_t tempheap = {0};

    // get a temp file path
    const char *temp;
    derr_type_t etype = FMT_QUIET(&tempstack, "%x.tmp", FS(path));
    if(etype == E_NONE){
        temp = tempstack.data;
    }else{
        PROP_GO(&e, FMT(&tempheap, "%x.tmp", FS(path)), cu);
        temp = tempheap.data;
    }

    // write the temp file
    PROP_GO(&e, dfopen(temp, "w", &f), cu);
    PROP_GO(&e, FFMT(f, "%x\n", FU(*nonce)), cu);
    PROP_GO(&e, dfclose2(&f), cu);

    // rename temp file into place
    PROP_GO(&e, drename_atomic(temp, path), cu);

cu:
    if(f) fclose(f);
    dstr_free(&tempheap);
    return e;
}
derr_t nonce_read_increment_write_path(string_builder_t *sb, uint64_t *nonce){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &stack, &heap, &path) );

    PROP_GO(&e, nonce_read_increment_write(path->data, nonce), cu);

cu:
    dstr_free(&heap);
    return e;
}

// zeroizes and frees secret
void installation_free0(installation_t *inst){
    api_token_free0(&inst->token);
    dstr_free(&inst->subdomain);
    dstr_free(&inst->email);
    *inst = (installation_t){0};
}

DEF_CONTAINER_OF(jspec_inst_t, jspec, jspec_t)

derr_t jspec_inst_read(jspec_t *jspec, jctx_t *ctx){
    derr_t e = E_OK;

    installation_t *inst = CONTAINER_OF(jspec, jspec_inst_t, jspec)->inst;

    jspec_t *j = JOBJ(false,
        JKEY("email", JDCPY(&inst->email)),
        JKEY("secret", JDCPY(&inst->token.secret)),
        JKEY("subdomain", JDCPY(&inst->subdomain)),
        JKEY("token", JU(&inst->token.key)),
    );

    /* note: there's no point in catching errors here and free0-ing inst,
       because there could could be errors above we can't catch, so the caller
       must catch errors themselves */
    PROP(&e, jctx_read(ctx, j) );

    return e;
}

// note that api_token should be zeroized or freed before calling this
derr_t installation_read_dstr(dstr_t dstr, installation_t *inst){
    derr_t e = E_OK;
    derr_t e2;

    DSTR_VAR(jmem, 1024);
    json_node_t jnodes[32];
    size_t njnodes = sizeof(jnodes) / sizeof(*jnodes);

    // parse json
    json_t json;
    json_prep_preallocated(&json, &jmem, jnodes, njnodes, true);
    e2 = json_parse(dstr, &json);
    // if we got a fixedsize error it is not a valid file
    CATCH(&e2, E_FIXEDSIZE){
        LOG_WARN("installation file contains way too much json\n");
        RETHROW_GO(&e, &e2, E_PARAM, cu);
    }else PROP_VAR_GO(&e, &e2, cu);

    // read the json
    PROP_GO(&e, jspec_read(JINST(inst), json.root), cu);

cu:
    dstr_zeroize(&jmem);
    if(is_error(e)) installation_free0(inst);

    return e;
}

derr_t installation_read_file(const char *path, installation_t *inst){
    derr_t e = E_OK;
    derr_t e2;

    DSTR_VAR(creds, 1024);

    // read the file into memory
    e2 = dstr_read_file(path, &creds);
    // if we got a fixedsize error it is not a valid file
    CATCH(&e2, E_FIXEDSIZE){
        LOG_WARN("installation file seems too long, ignoring\n");
        RETHROW_GO(&e, &e2, E_PARAM, cu);
    }else PROP_VAR_GO(&e, &e2, cu);

    PROP_GO(&e, installation_read_dstr(creds, inst), cu);

cu:
    dstr_zeroize(&creds);
    if(is_error(e)) installation_free0(inst);

    return e;
}

derr_t installation_read_path(const string_builder_t sb, installation_t *inst){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(&sb, &stack, &heap, &path) );

    PROP_GO(&e, installation_read_file(path->data, inst), cu);

cu:
    dstr_free(&heap);
    return e;
}

derr_t installation_write(installation_t inst, const char *path){
    derr_t e = E_OK;
    FILE *f = NULL;
    DSTR_VAR(tempstack, 256);
    dstr_t tempheap = {0};

    // build a tempfile path
    const char *temp;
    derr_type_t etype = FMT_QUIET(&tempstack, "%x.tmp", FS(path));
    if(etype == E_NONE){
        temp = tempstack.data;
    }else{
        PROP_GO(&e, FMT(&tempheap, "%x.tmp", FS(path)), cu);
        temp = tempheap.data;
    }

    // open the tempfile for writing
    PROP_GO(&e, dfopen(temp, "w", &f), cu);

    // write the file
    jdump_i *obj = DOBJ(
        DKEY("email", DD(inst.email)),
        DKEY("subdomain", DD(inst.subdomain)),
        DKEY("secret", DD(inst.token.secret)),
        DKEY("token", DU(inst.token.key)),
    );
    PROP_GO(&e, jdump(obj, WF(f), 2), cu);

    // check error when closing writable file descriptor
    PROP_GO(&e, dfclose2(&f), cu);

    // rename temp file into place
    PROP_GO(&e, drename_atomic(temp, path), cu);

cu:
    if(f) fclose(f);
    dstr_free(&tempheap);
    return e;
}
derr_t installation_write_path(installation_t inst, const string_builder_t sb){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(&sb, &stack, &heap, &path) );

    PROP_GO(&e, installation_write(inst, path->data), cu);

cu:
    dstr_free(&heap);
    return e;
}

//

DEF_CONTAINER_OF(jspec_api_t, jspec, jspec_t)

derr_t jspec_api_read(jspec_t *jspec, jctx_t *ctx){
    derr_t e = E_OK;

    // capture error and ok independently of what we report upwards
    DSTR_VAR(errbuf, 1024);
    bool ok = true;
    jctx_t outerctx = jctx_fork(ctx, ctx->node, &ok, &errbuf);

    // first read the outer layer
    bool contents_ok;
    dstr_t status;
    json_ptr_t jcontents;
    jspec_t *outerspec = JOBJ(true,
        JKEYOPT("contents", &contents_ok, JPTR(&jcontents)),
        JKEY("status", JDREF(&status)),
    );
    PROP(&e, outerspec->read(outerspec, &outerctx) );
    if(!ok){
        ORIG(&e, E_RESPONSE, "invalid api server response:\n%x", FD(errbuf));
    }

    // now make sure that the request was a success
    if(!dstr_ieq(status, DSTR_LIT("success"))){
        // contents should be a string failure message
        if(contents_ok){
            dstr_t dcontents;
            jctx_t ignctx = jctx_fork(ctx, jcontents.node, &ok, NULL);
            jspec_t *subspec = JDREF(&dcontents);
            derr_t e2 = subspec->read(subspec, &ignctx);
            if(is_error(e2)){
                DROP_VAR(&e2);
            }else if(ok){
                ORIG(&e, E_RESPONSE, "api call failed: %x", FD(dcontents));
            }
        }
        ORIG(&e, E_RESPONSE, "api call failed");
    }

    // now call the content jspec on the inner layer
    jctx_t innerctx = jctx_sub_key(ctx, DSTR_LIT("contents"), jcontents.node);
    jspec_t *content_spec = CONTAINER_OF(jspec, jspec_api_t, jspec)->content;
    PROP(&e, jctx_read(&innerctx, content_spec) );

    return e;
}

//

derr_t api_client_init(
    api_client_t *apic, duv_http_t *http, const dstr_t baseurl
){
    derr_t e = E_OK;

    *apic = (api_client_t){ .http = http };

    PROP(&e, dstr_copy(&baseurl, &apic->baseurl) );

    return e;
}

// must have no request in flight
void api_client_free(api_client_t *apic){
    dstr_free(&apic->baseurl);
    dstr_free0(&apic->d1);
    dstr_free0(&apic->d2);
    dstr_free(&apic->url);
    dstr_free0(&apic->reqbody);
    dstr_free0(&apic->respbody);
    *apic = (api_client_t){0};
}

bool api_client_cancel(api_client_t *apic){
    // just cancel whatever we have in-flight
    return stream_reader_cancel(&apic->reader);
}

static void apic_finish(api_client_t *apic){
    api_client_cb cb = apic->cb;
    void *cb_data = apic->cb_data;
    derr_t e = apic->e;
    json_t *json = apic->json;

    apic->cb = NULL;
    apic->cb_data = NULL;
    apic->e = (derr_t){0};
    apic->json = NULL;

    if(is_error(e)) json_free(json);

    dstr_zeroize(&apic->d1);
    dstr_zeroize(&apic->d2);

    cb(cb_data, e, json);
}

DEF_CONTAINER_OF(api_client_t, schedulable, schedulable_t)

// delayed_error is a scheduler_schedule_cb
static void delayed_error(schedulable_t *schedulable){
    api_client_t *apic = CONTAINER_OF(schedulable, api_client_t, schedulable);
    apic_finish(apic);
}

static derr_t read_resp(
    bool using_password,
    int status,
    dstr_t reason,
    const dstr_t respbody,
    json_t *json
){
    derr_t e = E_OK;

    if(status == 401 || status == 403){
        if(using_password){
            ORIG(&e,
                E_PASSWORD,
                "Failed password authentication: %x %x",
                FI(status),
                FD(reason)
            );
        }else{
            ORIG(&e,
                E_TOKEN,
                "Failed token authentication: %x %x",
                FI(status),
                FD(reason)
            );
        }
    }

    if(status < 200 || status > 299){
        ORIG(&e,
            E_RESPONSE,
            "REST API server returned %x: %x",
            FI(status),
            FD(reason)
        );
    }

    // 2xx status means we have a json response
    derr_t e2 = json_parse(respbody, json);
    CATCH(&e2, E_PARAM){
        // invalid json is not allowed from the server
        json_free(json);
        RETHROW(&e, &e2, E_RESPONSE);
    }else PROP_VAR(&e, &e2);

    return e;
}

DEF_CONTAINER_OF(api_client_t, reader, stream_reader_t)

static void apic_reader_cb(stream_reader_t *reader, derr_t err){
    api_client_t *apic = CONTAINER_OF(reader, api_client_t, reader);

    // capture errors
    PROP_VAR_GO(&apic->e, &err, done);

    PROP_GO(&apic->e,
        read_resp(
            apic->using_password,
            apic->req.status,
            apic->req.reason,
            apic->respbody,
            apic->json
        ),
    done);

done:
    apic_finish(apic);
}

static derr_t apic_body(
    const dstr_t path, const dstr_t arg, const uint64_t *nonce, dstr_t *out
){
    derr_t e = E_OK;

    jdump_i *obj = DOBJ(
        // path is required
        DKEY("path", DD(path)),
        // arg may be null
        DKEY("arg", arg.len ? DD(arg) : DNULL),
        // nonce may be missing
        DKEY("nonce", nonce ? DU(*nonce) : NULL),
    );

    // encode as b64 for easy signature verification
    PROP(&e, jdump(obj, WB64(WD(out)), 0) );

    return e;
}

// appropriate body and headers are configured by token or password entrypoint
static derr_t apic_mkreq(api_client_t *apic, dstr_t path, http_pairs_t *hdrs){
    derr_t e = E_OK;

    // compose full url
    apic->url.len = 0;
    PROP(&e, FMT(&apic->url, "%x%x", FD(apic->baseurl), FD(path)) );

    url_t url;
    PROP(&e, parse_url(&apic->url, &url) );

    // queue the request
    rstream_i *rstream = duv_http_req(
        &apic->req,
        apic->http,
        HTTP_METHOD_POST,
        url,
        NULL, // params
        hdrs,
        apic->reqbody,
        NULL // hdr_cb
    );

    // queue reading the request
    apic->respbody.len = 0;
    json_free(apic->json);
    stream_read_all(&apic->reader, rstream, &apic->respbody, apic_reader_cb);

    return e;
}

static derr_t apic_pass_hdrs(
    dstr_t user, dstr_t pass, dstr_t *d1, http_pairs_t *h1, http_pairs_t **out
){
    derr_t e = E_OK;

    *out = NULL;

    // write Authorization header
    d1->len = 0;
    PROP(&e, FMT(d1, "Basic %x", FB64F("%x:%x", FD(user), FD(pass))) );
    *h1 = (http_pairs_t){{DSTR_LIT("Authorization"), *d1}};

    *out = h1;

    return e;
}

void apic_pass(
    api_client_t *apic,
    dstr_t path,
    dstr_t arg,
    dstr_t user,
    dstr_t pass,
    json_t *json,
    api_client_cb cb,
    void *cb_data
){
    derr_t e = E_OK;

    apic->json = json;
    apic->cb = cb;
    apic->cb_data = cb_data;
    apic->using_password = true;

    // write request body
    apic->reqbody.len = 0;
    PROP_GO(&e, apic_body(path, arg, NULL, &apic->reqbody), fail);

    // write headers
    http_pairs_t *hdrs;
    PROP_GO(&e, apic_pass_hdrs(user, pass, &apic->d1, &apic->h1, &hdrs), fail);

    PROP_GO(&e, apic_mkreq(apic, path, hdrs), fail);

    return;

fail:
    apic->e = e;
    // schedule ourselves for later (this is the only time we need that)
    schedulable_prep(&apic->schedulable, delayed_error);
    scheduler_i *sched = &apic->http->scheduler->iface;
    sched->schedule(sched, &apic->schedulable);
}

static bool is_param_or_fixedsize(derr_type_t etype){
    return etype == E_PARAM || etype == E_FIXEDSIZE;
}

// reqbody must already be written
static derr_t apic_token_hdrs(
    api_token_t token,
    const dstr_t reqbody,
    dstr_t *d1,
    dstr_t *d2,
    http_pairs_t *h1,
    http_pairs_t *h2,
    http_pairs_t **out
){
    derr_t e = E_OK;

    *out = NULL;

    // sign the request
    DSTR_VAR(signature, 128);
    derr_t e2 = hmac(token.secret, reqbody, &signature);
    // token->secret shouldn't be too long, signature shouldn't be too short
    CATCH_EX(&e2, is_param_or_fixedsize){
        RETHROW(&e, &e2, E_INTERNAL);
    }else PROP_VAR(&e, &e2);

    // write signature header
    d1->len = 0;
    PROP(&e, bin2hex(&signature, d1) );
    *h1 = (http_pairs_t){{DSTR_LIT("X-AUTH-SIGNATURE"), *d1}};

    // write token key header
    d2->len = 0;
    PROP(&e, FMT(d2, "%x", FU(token.key)) );
    *h2 = (http_pairs_t){{DSTR_LIT("X-AUTH-TOKEN"), *d2}, h1};

    *out = h2;

    return e;
}

void apic_token(
    api_client_t *apic,
    dstr_t path,
    dstr_t arg,
    api_token_t token,
    json_t *json,
    api_client_cb cb,
    void *cb_data
){
    derr_t e = E_OK;

    apic->json = json;
    apic->cb = cb;
    apic->cb_data = cb_data;
    apic->using_password = false;

    // write request body
    apic->reqbody.len = 0;
    PROP_GO(&e, apic_body(path, arg, &token.nonce, &apic->reqbody), fail);

    // write headers
    http_pairs_t *hdrs;
    PROP_GO(&e,
        apic_token_hdrs(
            token,
            apic->reqbody,
            &apic->d1,
            &apic->d2,
            &apic->h1,
            &apic->h2,
            &hdrs
        ),
    fail);

    // send the request
    PROP_GO(&e, apic_mkreq(apic, path, hdrs), fail);

    return;

fail:
    apic->e = e;
    // schedule ourselves for later (this is the only time we need that)
    schedulable_prep(&apic->schedulable, delayed_error);
}

static derr_t mkreq_sync(
    http_sync_t *sync,
    dstr_t baseurl,
    dstr_t path,
    bool using_password,
    http_pairs_t *hdrs,
    dstr_t reqbody,
    json_t *json
){
    derr_t e = E_OK;

    dstr_t resp = {0};

    DSTR_VAR(durl, 256);

    // compose full url
    PROP_GO(&e, FMT(&durl, "%x%x", FD(baseurl), FD(path)), cu);

    url_t url;
    PROP_GO(&e, parse_url(&durl, &url), cu);

    int status;
    DSTR_VAR(reason, 256);

    PROP_GO(&e,
        http_sync_req(
            sync,
            HTTP_METHOD_POST,
            url,
            NULL, // params
            hdrs,
            reqbody,
            NULL, // selectors
            &status,
            &reason,
            &resp
        ),
    cu);

    PROP_GO(&e, read_resp(using_password, status, reason, resp, json), cu);

cu:
    dstr_free0(&resp);
    return e;
}

derr_t api_pass_sync(
    http_sync_t *sync,
    const dstr_t baseurl,
    const dstr_t path,
    const dstr_t arg,
    const dstr_t user,
    const dstr_t pass,
    json_t *json
){
    derr_t e = E_OK;

    DSTR_VAR(d1, 256);
    dstr_t reqbody = {0};

    // write request body
    PROP_GO(&e, apic_body(path, arg, NULL, &reqbody), cu);

    // write headers
    http_pairs_t h1, *hdrs;
    PROP_GO(&e, apic_pass_hdrs(user, pass, &d1, &h1, &hdrs), cu);

    // make request
    PROP_GO(&e,
        mkreq_sync(sync, baseurl, path, true, hdrs, reqbody, json),
    cu);

cu:
    dstr_zeroize(&d1);
    dstr_free0(&reqbody);

    return e;
}

derr_t api_token_sync(
    http_sync_t *sync,
    const dstr_t baseurl,
    const dstr_t path,
    const dstr_t arg,
    api_token_t token,
    json_t *json
){
    derr_t e = E_OK;

    DSTR_VAR(d1, 256);
    DSTR_VAR(d2, 256);
    dstr_t reqbody = {0};

    // write request body
    PROP_GO(&e, apic_body(path, arg, &token.nonce, &reqbody), cu);

    // write headers
    http_pairs_t h1, h2, *hdrs;
    PROP_GO(&e,
        apic_token_hdrs(token, reqbody, &d1, &d2, &h1, &h2, &hdrs),
    cu);

    // make request
    PROP_GO(&e,
        mkreq_sync(sync, baseurl, path, false, hdrs, reqbody, json),
    cu);

cu:
    dstr_zeroize(&d1);
    dstr_zeroize(&d2);
    dstr_free0(&reqbody);

    return e;
}

derr_t register_api_token_sync(
    http_sync_t *sync,
    const dstr_t baseurl,
    const dstr_t user,
    const dstr_t pass,
    const char* creds_path
){
    derr_t e = E_OK;
    api_token_t token = {0};
    DSTR_VAR(jmem, 1024);
    json_node_t jnodes[32];
    size_t njnodes = sizeof(jnodes) / sizeof(*jnodes);

    LOG_INFO("attempting to register a new token\n");

    // response is bounded to just an api token
    json_t json;
    json_prep_preallocated(&json, &jmem, jnodes, njnodes, true);

    DSTR_STATIC(path, "/api/add_token");
    dstr_t arg = {0};
    PROP_GO(&e,
        api_pass_sync(sync, baseurl, path, arg, user, pass, &json),
    cu);

    // read the secret and token from contents
    jspec_t *jspec = JAPI(
        JOBJ(true,
            JKEY("secret", JDCPY(&token.secret)),
            JKEY("token", JU(&token.key)),
        )
    );
    bool ok;
    DSTR_VAR(errbuf, 1024);
    PROP_GO(&e, jspec_read_ex(jspec, json.root, &ok, &errbuf), cu);
    if(!ok){
        ORIG_GO(&e,
            E_RESPONSE, "invalid server response:\n%x", cu, FD(errbuf)
        );
    }

    // nonce has to be at least 1, since the nonce starts at 0 on the server
    token.nonce = 1;

    // now write the token to a file
    PROP_GO(&e, api_token_write(token, creds_path), cu);

    LOG_INFO("sucessfully registered API token\n");

cu:
    dstr_zeroize(&jmem);
    api_token_free0(&token);

    return e;
}

derr_t register_api_token_path_sync(
    http_sync_t *sync,
    const dstr_t baseurl,
    const dstr_t user,
    const dstr_t pass,
    const string_builder_t *sb
){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(sb, &stack, &heap, &path) );

    PROP_GO(&e,
        register_api_token_sync(sync, baseurl, user, pass, path->data),
    cu);

cu:
    dstr_free(&heap);
    return e;
}
