/* We separated the different stages of the server into different objects:
    - imap_server_t handles greeting and pre-starttls
    - anon_t handles login
    - sf_pair_t handles the rest

   But since all stages of the imap server have to handle a small set of common
   responses, we refactored those into this separate file. */

derr_t respond_st(
    ie_status_t st,
    ie_dstr_t **tagp,
    ie_st_code_t **codep,
    dstr_t msg,
    link_t *out
);
#define RESP_OK(tagp, text, out) \
    respond_st(IE_ST_OK, tagp, NULL, DSTR_LIT(text), (out))
#define RESP_NO(tagp, text, out) \
    respond_st(IE_ST_NO, tagp, NULL, DSTR_LIT(text), (out))
#define RESP_BAD(tagp, text, out) \
    respond_st(IE_ST_BAD, tagp, NULL, DSTR_LIT(text), (out))
#define RESP_BYE(tagp, text, out) \
    respond_st(IE_ST_BYE, tagp, NULL, DSTR_LIT(text), (out))

derr_t respond_error(ie_dstr_t **tagp, ie_dstr_t **errorp, link_t *out);
derr_t respond_plus(link_t *out);
derr_t respond_noop(ie_dstr_t **tagp, link_t *out);
derr_t respond_capas(
    ie_dstr_t **tagp, ie_dstr_t*(*capas_fn)(derr_t*), link_t *out
);
derr_t respond_bad_starttls(ie_dstr_t **tagp, bool insec, link_t *out);
derr_t respond_logout(ie_dstr_t **tagp, link_t *out);
derr_t respond_too_early(ie_dstr_t **tagp, link_t *out);
derr_t respond_not_supported(ie_dstr_t **tagp, link_t *out);

ie_dstr_t *build_capas_prestarttls(derr_t *e);
ie_dstr_t *build_capas_prelogin(derr_t *e);
ie_dstr_t *build_capas(derr_t *e);

/* Meanwhile, we did the same thing to imap clients and face a similar problem
   with processing server responses. */

ie_st_resp_t *match_info(const imap_resp_t *resp);

ie_st_resp_t *match_tagged(
    const imap_resp_t *resp, const dstr_t prefix, size_t idx
);
