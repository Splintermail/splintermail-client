#include "libimaildir.h"

#include "libuvthread/libuvthread.h"

// forward declarations
static derr_t get_uids_up_to_expunge(dn_t *dn, ie_seq_set_t **out);

typedef struct {
    unsigned int uid_dn;
    msg_flags_t flags;
    jsw_anode_t node;  // dn_t->store->tree
} exp_flags_t;
DEF_CONTAINER_OF(exp_flags_t, node, jsw_anode_t);

static const void *exp_flags_jsw_get_uid_dn(const jsw_anode_t *node){
    exp_flags_t *exp_flags = CONTAINER_OF(node, exp_flags_t, node);
    return &exp_flags->uid_dn;
}

static derr_t exp_flags_new(exp_flags_t **out, unsigned int uid_dn,
        msg_flags_t flags){
    derr_t e = E_OK;
    *out = NULL;

    exp_flags_t *exp_flags = malloc(sizeof(*exp_flags));
    if(!exp_flags) ORIG(&e, E_NOMEM, "nomem");
    *exp_flags = (exp_flags_t){.uid_dn = uid_dn, .flags = flags};

    *out = exp_flags;

    return e;
}

static void exp_flags_free(exp_flags_t **exp_flags){
    if(!*exp_flags) return;
    free(*exp_flags);
    *exp_flags = NULL;
}

// free everything related to dn.store
static void dn_free_store(dn_t *dn){
    dn->store.state = DN_WAIT_NONE;
    ie_dstr_free(STEAL(ie_dstr_t, &dn->store.tag));

    jsw_anode_t *node;
    while((node = jsw_apop(&dn->store.tree))){
        exp_flags_t *exp_flags = CONTAINER_OF(node, exp_flags_t, node);
        exp_flags_free(&exp_flags);
    }
}

// free everything related to dn.expunge
static void dn_free_expunge(dn_t *dn){
    dn->expunge.state = DN_WAIT_NONE;
    ie_dstr_free(STEAL(ie_dstr_t, &dn->expunge.tag));
}

// free everything related to dn.copy
static void dn_free_copy(dn_t *dn){
    dn->copy.state = DN_WAIT_NONE;
    ie_dstr_free(STEAL(ie_dstr_t, &dn->copy.tag));
}

// free everything related to dn.disconnect
static void dn_free_disconnect(dn_t *dn){
    dn->disconnect.state = DN_WAIT_NONE;
}

void dn_free(dn_t *dn){
    if(!dn) return;
    // actually there's nothing to free...
}

derr_t dn_init(dn_t *dn, dn_cb_i *cb, extensions_t *exts, bool examine){
    derr_t e = E_OK;

    *dn = (dn_t){
        .cb = cb,
        .examine = examine,
        .selected = false,
        .exts = exts,
    };

    link_init(&dn->link);
    link_init(&dn->pending_updates);

    /* the view gets built during processing of the SELECT command, so that the
       CONDSTORE/QRESYNC extensions can be handled efficiently */
    jsw_ainit(&dn->views, jsw_cmp_uint, msg_view_jsw_get_uid_dn);

    jsw_ainit(&dn->store.tree, jsw_cmp_uint, exp_flags_jsw_get_uid_dn);

    return e;
};

//// TODO: unify these send_* functions with the ones in sm_serve_logic?

static derr_t send_st_resp(dn_t *dn, const ie_dstr_t *tag, const dstr_t *msg,
        ie_status_t status){
    derr_t e = E_OK;

    // copy tag
    ie_dstr_t *tag_copy = ie_dstr_copy(&e, tag);

    // build text
    ie_dstr_t *text = ie_dstr_new(&e, msg, KEEP_RAW);

    // build response
    ie_st_resp_t *st_resp = ie_st_resp_new(&e, tag_copy, status, NULL, text);
    imap_resp_arg_t arg = {.status_type=st_resp};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);
    resp = imap_resp_assert_writable(&e, resp, dn->exts);
    CHECK(&e);

    PROP(&e, dn->cb->resp(dn->cb, resp) );

    return e;
}

static derr_t send_ok(dn_t *dn, const ie_dstr_t *tag,
        const dstr_t *msg){
    derr_t e = E_OK;
    PROP(&e, send_st_resp(dn, tag, msg, IE_ST_OK) );
    return e;
}

// static derr_t send_no(dn_t *dn, const ie_dstr_t *tag,
//         const dstr_t *msg){
//     derr_t e = E_OK;
//     PROP(&e, send_st_resp(dn, tag, msg, IE_ST_NO) );
//     return e;
// }

static derr_t send_bad(dn_t *dn, const ie_dstr_t *tag,
        const dstr_t *msg){
    derr_t e = E_OK;
    PROP(&e, send_st_resp(dn, tag, msg, IE_ST_BAD) );
    return e;
}

////

static derr_t send_flags_resp(dn_t *dn){
    derr_t e = E_OK;

    ie_flags_t *flags = ie_flags_new(&e);
    flags = ie_flags_add_simple(&e, flags, IE_FLAG_ANSWERED);
    flags = ie_flags_add_simple(&e, flags, IE_FLAG_FLAGGED);
    flags = ie_flags_add_simple(&e, flags, IE_FLAG_DELETED);
    flags = ie_flags_add_simple(&e, flags, IE_FLAG_SEEN);
    // TODO: is this one supposed to be used in general?
    // flags = ie_flags_add_simple(&e, flags, IE_FLAG_DRAFT);
    imap_resp_arg_t arg = {.flags=flags};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_FLAGS, arg);
    resp = imap_resp_assert_writable(&e, resp, dn->exts);
    CHECK(&e);

    PROP(&e, dn->cb->resp(dn->cb, resp) );

    return e;
}

static derr_t send_exists_resp(dn_t *dn){
    derr_t e = E_OK;

    if(dn->views.size > UINT_MAX){
        ORIG(&e, E_VALUE, "too many messages for exists response");
    }
    unsigned int exists = (unsigned int)dn->views.size;

    imap_resp_arg_t arg = {.exists=exists};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_EXISTS, arg);
    resp = imap_resp_assert_writable(&e, resp, dn->exts);
    CHECK(&e);

    PROP(&e, dn->cb->resp(dn->cb, resp) );

    return e;
}

static derr_t send_recent_resp(dn_t *dn){
    derr_t e = E_OK;

    // TODO: support \Recent flag

    unsigned int recent = 0;
    imap_resp_arg_t arg = {.recent=recent};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_RECENT, arg);
    resp = imap_resp_assert_writable(&e, resp, dn->exts);
    CHECK(&e);

    PROP(&e, dn->cb->resp(dn->cb, resp) );

    return e;
}

static derr_t send_unseen_resp(dn_t *dn){
    derr_t e = E_OK;

    // TODO: what technically is "unseen"?  And what if nothing is unseen?
    (void)dn;

    return e;
}

static derr_t send_pflags_resp(dn_t *dn){
    derr_t e = E_OK;

    ie_pflags_t *pflags = ie_pflags_new(&e);
    pflags = ie_pflags_add_simple(&e, pflags, IE_PFLAG_ANSWERED);
    pflags = ie_pflags_add_simple(&e, pflags, IE_PFLAG_FLAGGED);
    pflags = ie_pflags_add_simple(&e, pflags, IE_PFLAG_DELETED);
    pflags = ie_pflags_add_simple(&e, pflags, IE_PFLAG_SEEN);
    // TODO: is this one supposed to be used in general?
    // pflags = ie_pflags_add_simple(&e, pflags, IE_PFLAG_DRAFT);

    ie_st_code_arg_t code_arg = {.pflags=pflags};
    ie_st_code_t *code = ie_st_code_new(&e, IE_ST_CODE_PERMFLAGS, code_arg);

    DSTR_STATIC(msg, "here, have some permanentflags.");
    ie_dstr_t *text = ie_dstr_new(&e, &msg, KEEP_RAW);

    ie_st_resp_t *st_resp = ie_st_resp_new(&e, NULL, IE_ST_OK, code, text);
    imap_resp_arg_t arg = {.status_type=st_resp};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);
    resp = imap_resp_assert_writable(&e, resp, dn->exts);
    CHECK(&e);

    PROP(&e, dn->cb->resp(dn->cb, resp) );

    return e;
}

static derr_t send_uidnext_resp(dn_t *dn, unsigned int max_uid_dn){
    derr_t e = E_OK;

    ie_st_code_arg_t code_arg = {.uidnext = max_uid_dn + 1};
    ie_st_code_t *code = ie_st_code_new(&e, IE_ST_CODE_UIDNEXT, code_arg);

    DSTR_STATIC(msg, "get ready, it's coming");
    ie_dstr_t *text = ie_dstr_new(&e, &msg, KEEP_RAW);

    ie_st_resp_t *st_resp = ie_st_resp_new(&e, NULL, IE_ST_OK, code, text);
    imap_resp_arg_t arg = {.status_type=st_resp};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);
    resp = imap_resp_assert_writable(&e, resp, dn->exts);
    CHECK(&e);

    PROP(&e, dn->cb->resp(dn->cb, resp) );

    return e;
}

static derr_t send_uidvld_resp(dn_t *dn, unsigned int uidvld_dn){
    derr_t e = E_OK;

    ie_st_code_arg_t code_arg = { .uidvld = uidvld_dn };
    ie_st_code_t *code = ie_st_code_new(&e, IE_ST_CODE_UIDVLD, code_arg);

    DSTR_STATIC(msg, "ride or die");
    ie_dstr_t *text = ie_dstr_new(&e, &msg, KEEP_RAW);

    ie_st_resp_t *st_resp = ie_st_resp_new(&e, NULL, IE_ST_OK, code, text);
    imap_resp_arg_t arg = { .status_type = st_resp };
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);
    resp = imap_resp_assert_writable(&e, resp, dn->exts);
    CHECK(&e);

    PROP(&e, dn->cb->resp(dn->cb, resp) );

    return e;
}

static derr_t select_cmd(dn_t *dn, const ie_dstr_t *tag,
        const ie_select_cmd_t *select){
    derr_t e = E_OK;

    // make sure the select did not include QRESYNC or CONDSTORE
    if(select->params){
        ORIG(&e, E_PARAM, "QRESYNC and CONDSTORE not supported");
    }

    unsigned int max_uid_dn;
    unsigned int uidvld_dn;
    PROP(&e,
        imaildir_dn_build_views(dn->m, &dn->views, &max_uid_dn, &uidvld_dn)
    );

    // generate/send required SELECT responses
    PROP(&e, send_flags_resp(dn) );
    PROP(&e, send_exists_resp(dn) );
    PROP(&e, send_recent_resp(dn) );
    PROP(&e, send_unseen_resp(dn) );
    PROP(&e, send_pflags_resp(dn) );
    PROP(&e, send_uidnext_resp(dn, max_uid_dn) );
    PROP(&e, send_uidvld_resp(dn, uidvld_dn) );

    PROP(&e, send_ok(dn, tag, &DSTR_LIT("welcome in")) );

    return e;
}

// nums will be consumed
static derr_t send_search_resp(dn_t *dn, ie_nums_t *nums){
    derr_t e = E_OK;

    // TODO: support modseq here
    bool modseq_present = false;
    unsigned long modseqnum = 0;
    ie_search_resp_t *search = ie_search_resp_new(&e, nums, modseq_present,
            modseqnum);
    imap_resp_arg_t arg = {.search=search};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_SEARCH, arg);
    resp = imap_resp_assert_writable(&e, resp, dn->exts);
    CHECK(&e);

    PROP(&e, dn->cb->resp(dn->cb, resp) );

    return e;
}

static derr_t search_cmd(dn_t *dn, const ie_dstr_t *tag,
        const ie_search_cmd_t *search){
    derr_t e = E_OK;

    // handle the empty maildir case
    if(dn->views.size == 0){
        PROP(&e, send_search_resp(dn, NULL) );
        PROP(&e, send_ok(dn, tag, &DSTR_LIT("don't waste my time!")) );
        return e;
    }

    // now figure out some constants to do the search
    unsigned int seq_max;
    PROP(&e, index_to_seq_num(dn->views.size - 1, &seq_max) );

    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atlast(&trav, &dn->views);
    msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);

    unsigned int uid_dn_max = view->uid_dn;

    // we'll calculate the sequence number as we go
    unsigned int seq = seq_max;

    ie_nums_t *nums = NULL;

    // check every message in the view in reverse order
    for(; node != NULL; node = jsw_atprev(&trav)){
        view = CONTAINER_OF(node, msg_view_t, node);

        bool match;
        PROP_GO(&e, search_key_eval(search->search_key, view, seq, seq_max,
                    uid_dn_max, &match), fail);

        if(match){
            unsigned int val = search->uid_mode ? view->uid_dn : seq;
            if(!nums){
                nums = ie_nums_new(&e, val);
                CHECK_GO(&e, fail);
            }else{
                ie_nums_t *next = ie_nums_new(&e, val);
                // build the list backwards, it's more efficient
                nums = ie_nums_append(&e, next, nums);
                CHECK_GO(&e, fail);
            }
        }

        seq--;
    }

    // finally, send the responses (nums will be consumed)
    PROP(&e, send_search_resp(dn, nums) );
    PROP(&e, send_ok(dn, tag, &DSTR_LIT("too easy!")) );

    return e;

fail:
    ie_nums_free(nums);
    return e;
}

// take a sequence set and create a UID seq set
static derr_t copy_seq_to_uids(dn_t *dn, bool uid_mode,
        const ie_seq_set_t *old, bool up, ie_seq_set_t **out){
    derr_t e = E_OK;

    *out = NULL;

    // nothing in the tree
    if(dn->views.size == 0){
        return e;
    }

    jsw_anode_t *node;

    // get the last UID or last index, for replacing 0's we see in the seq_set
    // also get the starting inde
    unsigned int first;
    unsigned int last;
    if(uid_mode){
        /* first and last will always be uid_dn's, since they are used to
           iterate through the ie_seq_set_t *old, which is in terms of uid_dn */
        jsw_atrav_t trav;
        jsw_anode_t *node = jsw_atfirst(&trav, &dn->views);
        msg_view_t *first_view = CONTAINER_OF(node, msg_view_t, node);
        first = first_view->uid_dn;
        node = jsw_atlast(&trav, &dn->views);
        msg_view_t *last_view = CONTAINER_OF(node, msg_view_t, node);
        last = last_view->uid_dn;
    } else {
        // first sequence number is always 1
        first = 1;
        size_t last_index = dn->views.size - 1;
        PROP(&e, index_to_seq_num(last_index, &last) );
    }

    seq_set_builder_t ssb;
    seq_set_builder_prep(&ssb);

    ie_seq_set_trav_t trav;
    unsigned int i = ie_seq_set_iter(&trav, old, first, last);
    for(; i != 0; i = ie_seq_set_next(&trav)){
        if(uid_mode){
            // uid_dn in mailbox?
            node = jsw_afind(&dn->views, &i, NULL);
        }else{
            // sequence number in mailbox?
            node = jsw_aindex(&dn->views, (size_t)i - 1);
        }
        if(!node) continue;
        msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);
        unsigned int uid_out = up ? view->uid_up : view->uid_dn;
        PROP_GO(&e, seq_set_builder_add_val(&ssb, uid_out), fail_ssb);
    }

    *out = seq_set_builder_extract(&e, &ssb);
    CHECK(&e);

    return e;

fail_ssb:
    seq_set_builder_free(&ssb);
    return e;
}


static derr_t store_cmd(dn_t *dn, const ie_dstr_t *tag,
        const ie_store_cmd_t *store){
    derr_t e = E_OK;

    // we need each uid_up for relaying the store command upwards
    ie_seq_set_t *uid_up_seq;
    PROP(&e,
        copy_seq_to_uids(
            dn, store->uid_mode, store->seq_set, true, &uid_up_seq
        )
    );

    // detect noop STOREs
    if(!uid_up_seq){
        PROP(&e, dn_gather_updates(dn, store->uid_mode, NULL) );
        PROP(&e, send_ok(dn, tag, &DSTR_LIT("noop STORE command")) );
        return e;
    }

    // reset the dn.store state
    dn_free_store(dn);
    dn->store.uid_mode = store->uid_mode;
    dn->store.silent = store->silent;

    dn->store.tag = ie_dstr_copy(&e, tag);
    CHECK_GO(&e, fail_dn_store);

    // we need each uid_dn for building expected flags
    ie_seq_set_t *uid_dn_seq;
    PROP_GO(&e,
        copy_seq_to_uids(
            dn, store->uid_mode, store->seq_set, false, &uid_dn_seq
        ),
    fail_dn_store);

    // figure out what all of the flags we expect to see are
    msg_flags_t cmd_flags = msg_flags_from_flags(store->flags);
    ie_seq_set_trav_t trav;
    unsigned int uid_dn = ie_seq_set_iter(&trav, uid_dn_seq, 0, 0);
    for(; uid_dn != 0; uid_dn = ie_seq_set_next(&trav)){
        jsw_anode_t *node = jsw_afind(&dn->views, &uid_dn, NULL);
        if(!node){
            ORIG_GO(&e, E_INTERNAL, "uid_dn not found", fail_uids_dn);
        }

        msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);

        msg_flags_t new_flags;
        switch(store->sign){
            case 0:
                // set flags exactly (new = cmd)
                new_flags = cmd_flags;
                break;
            case 1:
                // add the marked flags (new = old | cmd)
                new_flags = msg_flags_or(view->flags, cmd_flags);
                break;
            case -1:
                // remove the marked flags (new = old & (~cmd))
                new_flags = msg_flags_and(view->flags,
                        msg_flags_not(cmd_flags));
                break;
            default:
                ORIG_GO(&e, E_INTERNAL, "invalid store->sign", fail_uids_dn);
        }

        // if we expect no difference in flags, omit this uid_dn
        if(msg_flags_eq(new_flags, view->flags)) continue;

        exp_flags_t *exp_flags;
        PROP_GO(&e,
            exp_flags_new(&exp_flags, uid_dn, new_flags),
        fail_uids_dn);
        jsw_ainsert(&dn->store.tree, &exp_flags->node);
    }

    // done with uids_dn
    ie_seq_set_free(STEAL(ie_seq_set_t, &uid_dn_seq));

    ie_store_cmd_t *uid_store = ie_store_cmd_new(&e,
        true,
        STEAL(ie_seq_set_t, &uid_up_seq),
        ie_store_mods_copy(&e, store->mods),
        store->sign,
        store->silent,
        ie_flags_copy(&e, store->flags)
    );

    // now send the uid_store to the imaildir_t
    update_req_t *req = update_req_store_new(&e, uid_store, dn);

    CHECK_GO(&e, fail_dn_store);

    // at this point, it may be better to leave the dn_store in-tact
    dn->store.state = DN_WAIT_WAITING;
    PROP(&e, imaildir_dn_request_update(dn->m, req) );

    return e;

fail_uids_dn:
    ie_seq_set_free(uid_dn_seq);
fail_dn_store:
    dn_free_store(dn);
    ie_seq_set_free(uid_up_seq);
    return e;
}


// a helper struct to lazily load the message body
typedef struct {
    imaildir_t *m;
    unsigned int uid_up;
    ie_dstr_t *content;
    // the first taker gets *content; following takers get a copy
    bool taken;
    imf_t *imf;
} loader_t;

static loader_t loader_prep(imaildir_t *m, unsigned int uid_up){
    return (loader_t){ .m = m, .uid_up = uid_up };
}

static void loader_load(derr_t *e, loader_t *loader){
    if(is_error(*e)) goto fail;
    if(loader->content) return;

    int fd;
    PROP_GO(e, imaildir_dn_open_msg(loader->m, loader->uid_up, &fd), fail);
    loader->content = ie_dstr_new_from_fd(e, fd);

    // if imalidir fails in this call, this will overwrite e with E_IMAILDIR
    PROP_GO(e,
        imaildir_dn_close_msg(loader->m, loader->uid_up, &fd),
    fail);

fail:
    return;
}

// get the content but don't take ownership
static ie_dstr_t *loader_get(derr_t *e, loader_t *loader){
    if(is_error(*e)) goto fail;

    loader_load(e, loader);
    return loader->content;

fail:
    return NULL;
}

// take ownership of the content (or a copy if the original is spoken for)
static ie_dstr_t *loader_take(derr_t *e, loader_t *loader){
    if(is_error(*e)) goto fail;

    loader_load(e, loader);
    if(loader->taken){
        return ie_dstr_copy(e, loader->content);
    }

    loader->taken = true;
    return loader->content;

fail:
    return NULL;
}

// parse the content into an imf_t
static imf_t *loader_parse(derr_t *e, loader_t *loader){
    if(is_error(*e)) goto fail;
    if(loader->imf) return loader->imf;

    loader->imf = imf_parse_builder(e, loader_get(e, loader));
    return loader->imf;

fail:
    return NULL;
}

static void loader_free(loader_t *loader){
    imf_free(loader->imf);
    loader->imf = NULL;
    if(!loader->taken){
        ie_dstr_free(loader->content);
    }
    loader->content = NULL;
}


static ie_dstr_t *imf_copy_body(derr_t *e, imf_t *imf){
    if(is_error(*e)) return NULL;
    if(!imf->body) return ie_dstr_new_empty(e);
    return ie_dstr_new(e, &imf->body->bytes, KEEP_RAW);
}


static ie_dstr_t *imf_copy_hdrs(derr_t *e, imf_t *imf){
    if(is_error(*e)) return NULL;
    return ie_dstr_new(e, &imf->hdr_bytes, KEEP_RAW);
}


static const dstr_t *imf_posthdr_empty_line(derr_t *e, imf_t *imf){
    /* for some odd reason the IMAP standard says that HEADER.FIELDS and
       HEADER.FIELDS.NOT responses need to include the empty line after the
       headers, if one exists */
    static dstr_t none = (dstr_t){0};
    static dstr_t crlf = DSTR_LIT("\r\n");
    static dstr_t lf = DSTR_LIT("\n");
    if(is_error(*e)) return &none;

    // find the last hdr
    imf_hdr_t *hdr = imf->hdr;
    while(hdr->next) hdr = hdr->next;

    // get a dstr of just the separator bytes
    dstr_t last_hdr_and_sep = token_extend(hdr->bytes, imf->hdr_bytes);
    dstr_t sep =
        dstr_sub(&last_hdr_and_sep, hdr->bytes.len, last_hdr_and_sep.len);
    if(sep.len == 0){
        return &none;
    }
    if(dstr_cmp(&sep, &lf) == 0){
        return &lf;
    }
    return &crlf;
}


static ie_dstr_t *imf_copy_hdr_fields(derr_t *e, imf_t *imf,
        const ie_dstr_t *names){
    if(is_error(*e)) return NULL;

    ie_dstr_t *out = ie_dstr_new_empty(e);
    const imf_hdr_t *hdr = imf->hdr;
    for( ; hdr; hdr = hdr->next){
        const ie_dstr_t *name = names;
        for( ; name; name = name->next){
            if(dstr_icmp(&hdr->name, &name->dstr) == 0){
                out = ie_dstr_append(e, out, &hdr->bytes, KEEP_RAW);
                break;
            }
        }
    }
    // include any post-header separator line
    out = ie_dstr_append(e, out, imf_posthdr_empty_line(e, imf), KEEP_RAW);
    return out;
}


static ie_dstr_t *imf_copy_hdr_fields_not(derr_t *e, imf_t *imf,
        const ie_dstr_t *names){
    if(is_error(*e)) return NULL;

    ie_dstr_t *out = ie_dstr_new_empty(e);
    const imf_hdr_t *hdr = imf->hdr;
    for( ; hdr; hdr = hdr->next){
        const ie_dstr_t *name = names;
        bool keep = true;
        for( ; name; name = name->next){
            if(dstr_icmp(&hdr->name, &name->dstr) == 0){
                keep = false;
                break;
            }
        }
        if(keep){
            out = ie_dstr_append(e, out, &hdr->bytes, KEEP_RAW);
        }
    }
    // include any post-header separator line
    out = ie_dstr_append(e, out, imf_posthdr_empty_line(e, imf), KEEP_RAW);
    return out;
}


static derr_t send_fetch_resp(dn_t *dn, const ie_fetch_cmd_t *fetch,
        unsigned int uid_dn){
    derr_t e = E_OK;

    // get the view
    size_t index;
    jsw_anode_t *node = jsw_afind(&dn->views, &uid_dn, &index);
    if(!node) ORIG(&e, E_INTERNAL, "uid_dn missing");
    msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);

    // handle lazy loading of the content
    loader_t loader = loader_prep(dn->m, view->uid_up);

    unsigned int seq_num;
    PROP(&e, index_to_seq_num(index, &seq_num) );

    // build a fetch response
    ie_fetch_resp_t *f = ie_fetch_resp_new(&e);

    // the num is always a sequence number, even in case of a UID FETCH
    f = ie_fetch_resp_num(&e, f, seq_num);

    if(fetch->attr->envelope) TRACE_ORIG(&e, E_INTERNAL, "not implemented");

    if(fetch->attr->flags){
        ie_fflags_t *ff = ie_fflags_new(&e);
        if(view->flags.answered)
            ff = ie_fflags_add_simple(&e, ff, IE_FFLAG_ANSWERED);
        if(view->flags.flagged)
            ff = ie_fflags_add_simple(&e, ff, IE_FFLAG_FLAGGED);
        if(view->flags.seen)
            ff = ie_fflags_add_simple(&e, ff, IE_FFLAG_SEEN);
        if(view->flags.draft)
            ff = ie_fflags_add_simple(&e, ff, IE_FFLAG_DRAFT);
        if(view->flags.deleted)
            ff = ie_fflags_add_simple(&e, ff, IE_FFLAG_DELETED);
        if(view->recent)
            ff = ie_fflags_add_simple(&e, ff, IE_FFLAG_RECENT);
        f = ie_fetch_resp_flags(&e, f, ff);
    }

    if(fetch->attr->intdate){
        f = ie_fetch_resp_intdate(&e, f, view->internaldate);
    }

    // UID is implcitly requested by all UID_FETCH commands
    if(fetch->attr->uid || fetch->uid_mode){
        f = ie_fetch_resp_uid(&e, f, uid_dn);
    }

    if(fetch->attr->rfc822){
        // take ownership of the content
        f = ie_fetch_resp_rfc822(&e, f, loader_take(&e, &loader));
    }

    if(fetch->attr->rfc822_header){
        imf_t *imf = loader_parse(&e, &loader);
        f = ie_fetch_resp_rfc822_hdr(&e, f, imf_copy_hdrs(&e, imf));
    }

    if(fetch->attr->rfc822_text){
        imf_t *imf = loader_parse(&e, &loader);
        f = ie_fetch_resp_rfc822_text(&e, f, imf_copy_body(&e, imf));
    }

    if(fetch->attr->rfc822_size){
        if(view->length > UINT_MAX){
            TRACE_ORIG(&e, E_INTERNAL, "msg length exceeds UINT_MAX");
        }else{
            unsigned int size = (unsigned int)view->length;
            f = ie_fetch_resp_rfc822_size(&e, f, ie_nums_new(&e, size));
        }
    }

    if(fetch->attr->body) TRACE_ORIG(&e, E_INTERNAL, "not implemented"); // means BODY, not BODY[]
    if(fetch->attr->bodystruct) TRACE_ORIG(&e, E_INTERNAL, "not implemented");
    if(fetch->attr->modseq) TRACE_ORIG(&e, E_INTERNAL, "not implemented");

    ie_fetch_extra_t *extra = fetch->attr->extras;
    for( ; extra; extra = extra->next){
        ie_dstr_t *content_resp = NULL;
        ie_sect_t *sect = extra->sect;
        if(!sect){
            // BODY[] by itself
            content_resp = loader_take(&e, &loader);
        }else{
            // first parse the whole message
            imf_t *root_imf = loader_parse(&e, &loader);

            imf_t *imf = root_imf;
            if(sect->sect_part){
                // we don't parse MIME parts yet
                TRACE_ORIG(&e, E_INTERNAL, "not implemented");
                // TODO: point *imf at whatever relevant submessage
            }

            if(sect->sect_txt){
                switch(sect->sect_txt->type){
                    case IE_SECT_MIME:
                        // if MIME is used, then ie_sect_t.sect_part != NULL
                        TRACE_ORIG(&e, E_INTERNAL, "not implemented");
                        break;

                    case IE_SECT_TEXT:
                        // just the body of the message
                        content_resp = imf_copy_body(&e, imf);
                        break;

                    case IE_SECT_HEADER:
                        // just the headers of the message
                        content_resp = imf_copy_hdrs(&e, imf);
                        break;

                    case IE_SECT_HDR_FLDS:
                        // just the headers of the message
                        content_resp = imf_copy_hdr_fields(
                            &e, imf, sect->sect_txt->headers
                        );
                        break;

                    case IE_SECT_HDR_FLDS_NOT:
                        // just the headers of the message
                        content_resp = imf_copy_hdr_fields_not(
                            &e, imf, sect->sect_txt->headers
                        );
                        break;
                }
            }else{
                /* TODO: for now, since we don't support sect->sect_part, this
                   can't happen non-null.

                   But I don't know what to put here when we do support it. */
                TRACE_ORIG(&e, E_INTERNAL, "not implemented");
            }
        }

        ie_nums_t *offset = NULL;
        if(extra->partial){
            // replace content_resp with a substring of the content_resp
            dstr_t sub = ie_dstr_sub(content_resp,
                    extra->partial->a, extra->partial->b);
            ie_dstr_t *temp = content_resp;
            content_resp = ie_dstr_new(&e, &sub, KEEP_RAW);
            ie_dstr_free(temp);
            offset = ie_nums_new(&e, extra->partial->a);
        }

        // extend the fetch_resp
        ie_sect_t *sect_resp = ie_sect_copy(&e, sect);
        ie_fetch_resp_extra_t *extra_resp =
            ie_fetch_resp_extra_new(&e, sect_resp, offset, content_resp);
        f = ie_fetch_resp_add_extra(&e, f, extra_resp);
    }

    // finally, send the fetch response
    imap_resp_arg_t arg = {.fetch=f};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_FETCH, arg);

    resp = imap_resp_assert_writable(&e, resp, dn->exts);

    loader_free(&loader);

    CHECK(&e);

    PROP(&e, dn->cb->resp(dn->cb, resp) );

    return e;
}


static derr_t fetch_cmd(dn_t *dn, const ie_dstr_t *tag,
        const ie_fetch_cmd_t *fetch){
    derr_t e = E_OK;

    ie_seq_set_t *uid_dn_seq;
    bool up = false;
    PROP(&e,
        copy_seq_to_uids(dn, fetch->uid_mode, fetch->seq_set, up, &uid_dn_seq)
    );

    // TODO: support PEEK properly

    // build a response for every uid_dn requested
    ie_seq_set_trav_t trav;
    unsigned int uid_dn = ie_seq_set_iter(&trav, uid_dn_seq, 0, 0);
    for(; uid_dn != 0; uid_dn = ie_seq_set_next(&trav)){
        PROP_GO(&e, send_fetch_resp(dn, fetch, uid_dn), cu);
    }

    DSTR_STATIC(msg, "you didn't hear it from me");
    PROP_GO(&e, send_ok(dn, tag, &msg), cu);

cu:
    ie_seq_set_free(uid_dn_seq);

    return e;
}


static derr_t expunge_cmd(dn_t *dn, const ie_dstr_t *tag){
    derr_t e = E_OK;

    ie_seq_set_t *uids_up;
    PROP(&e, get_uids_up_to_expunge(dn, &uids_up) );
    // detect noop EXPUNGEs
    if(!uids_up){
        PROP(&e, dn_gather_updates(dn, true, NULL) );
        PROP(&e, send_ok(dn, tag, &DSTR_LIT("noop EXPUNGE command")) );
        return e;
    }

    // reset the dn.expunge state
    dn_free_expunge(dn);
    dn->expunge.tag = ie_dstr_copy(&e, tag);
    dn->expunge.state = DN_WAIT_WAITING;
    CHECK_GO(&e, fail);

    // request an expunge update
    update_req_t *req =
        update_req_expunge_new(&e, STEAL(ie_seq_set_t, &uids_up), dn);
    CHECK_GO(&e, fail);
    PROP_GO(&e, imaildir_dn_request_update(dn->m, req), fail);

    return e;

fail:
    dn_free_expunge(dn);
    ie_seq_set_free(uids_up);
    return e;
}


static derr_t copy_cmd(dn_t *dn, const ie_dstr_t *tag,
        const ie_copy_cmd_t *copy){
    derr_t e = E_OK;

    ie_seq_set_t *uids_up;
    PROP(&e,
        copy_seq_to_uids(dn, copy->uid_mode, copy->seq_set, true, &uids_up)
    );

    // detect noop COPYs
    if(!uids_up){
        PROP(&e, dn_gather_updates(dn, true, NULL) );
        PROP(&e, send_ok(dn, tag, &DSTR_LIT("noop COPY command")) );
        return e;
    }

    // reset the dn.expunge state
    dn_free_copy(dn);
    dn->copy.tag = ie_dstr_copy(&e, tag);
    dn->copy.state = DN_WAIT_WAITING;
    CHECK_GO(&e, fail);

    // request a copy update
    bool uid_mode = true;
    ie_copy_cmd_t *copy_up = ie_copy_cmd_new(&e,
        uid_mode,
        STEAL(ie_seq_set_t, &uids_up),
        ie_mailbox_copy(&e, copy->m)
    );
    update_req_t *req = update_req_copy_new(&e, copy_up, dn);
    CHECK_GO(&e, fail);

    PROP_GO(&e, imaildir_dn_request_update(dn->m, req), fail);

    return e;

fail:
    dn_free_copy(dn);
    ie_seq_set_free(uids_up);
    return e;
}


// we either need to consume the cmd or free it
derr_t dn_cmd(dn_t *dn, imap_cmd_t *cmd){
    derr_t e = E_OK;

    const ie_dstr_t *tag = cmd->tag;
    const imap_cmd_arg_t *arg = &cmd->arg;
    bool select_like =
        cmd->type == IMAP_CMD_SELECT || cmd->type == IMAP_CMD_EXAMINE;

    if(select_like && dn->selected){
        // A dn_t is not able to
        ORIG_GO(&e, E_INTERNAL, "SELECT sent to selected dn_t", cu_cmd);
    }
    if(!select_like && !dn->selected){
        ORIG_GO(&e, E_INTERNAL, "non-SELECT sent to unselected dn_t", cu_cmd);
    }
    if(select_like && dn->examine != (cmd->type == IMAP_CMD_EXAMINE)){
        if(dn->examine){
            ORIG_GO(&e, E_INTERNAL, "dn_t got SELECT, not EXAMINE", cu_cmd);
        }else{
            ORIG_GO(&e, E_INTERNAL, "dn_t got EXAMINE, not SELECT", cu_cmd);
        }
    }

    switch(cmd->type){
        case IMAP_CMD_EXAMINE:
        case IMAP_CMD_SELECT:
            PROP_GO(&e, select_cmd(dn, tag, arg->select), cu_cmd);
            dn->selected = true;
            break;

        case IMAP_CMD_SEARCH:
            PROP_GO(&e,
                dn_gather_updates(dn, arg->search->uid_mode, NULL),
            cu_cmd);
            PROP_GO(&e, search_cmd(dn, tag, arg->search), cu_cmd);
            break;

        case IMAP_CMD_CHECK:
        case IMAP_CMD_NOOP:
            PROP_GO(&e, dn_gather_updates(dn, true, NULL), cu_cmd);
            PROP_GO(&e, send_ok(dn, tag, &DSTR_LIT("zzzzz...")), cu_cmd);
            break;

        case IMAP_CMD_STORE:
            // store_cmd has its own handling of updates
            PROP_GO(&e, store_cmd(dn, tag, arg->store), cu_cmd);
            break;

        case IMAP_CMD_FETCH:
            PROP_GO(&e,
                dn_gather_updates(dn, arg->fetch->uid_mode, NULL),
            cu_cmd);
            PROP_GO(&e, fetch_cmd(dn, tag, arg->fetch), cu_cmd);
            break;

        case IMAP_CMD_EXPUNGE:
            // updates are handled after an UPDATE_SYNC
            PROP_GO(&e, expunge_cmd(dn, tag), cu_cmd);
            break;

        case IMAP_CMD_COPY:
            // updates are handled after an UPDATE_SYNC
            PROP_GO(&e, copy_cmd(dn, tag, arg->copy), cu_cmd);
            break;

        // commands which must be handled externally
        case IMAP_CMD_ERROR:
        case IMAP_CMD_PLUS_REQ:
        case IMAP_CMD_CAPA:
        case IMAP_CMD_LOGOUT:
        case IMAP_CMD_CLOSE:
        case IMAP_CMD_STARTTLS:
        case IMAP_CMD_AUTH:
        case IMAP_CMD_LOGIN:
        case IMAP_CMD_CREATE:
        case IMAP_CMD_DELETE:
        case IMAP_CMD_RENAME:
        case IMAP_CMD_SUB:
        case IMAP_CMD_UNSUB:
        case IMAP_CMD_LIST:
        case IMAP_CMD_LSUB:
        case IMAP_CMD_STATUS:
        case IMAP_CMD_APPEND:
        case IMAP_CMD_XKEYSYNC:
        case IMAP_CMD_XKEYSYNC_DONE:
        case IMAP_CMD_XKEYADD:
            ORIG_GO(&e, E_INTERNAL, "unexpected command in dn_t", cu_cmd);

        // not yet supported
        case IMAP_CMD_IDLE:
        case IMAP_CMD_IDLE_DONE:
            ORIG_GO(&e, E_INTERNAL, "not yet supported in dn_t", cu_cmd);
            break;

        // unsupported extensions
        case IMAP_CMD_ENABLE:
        case IMAP_CMD_UNSELECT:
            PROP_GO(&e, send_bad(
                dn, tag, &DSTR_LIT("extension not supported")
            ), cu_cmd);
            break;
    }

cu_cmd:
    imap_cmd_free(cmd);

    return e;
}

// send an expunge for every message that we know of that is \Deleted
static derr_t get_uids_up_to_expunge(dn_t *dn, ie_seq_set_t **out){
    derr_t e = E_OK;
    *out = NULL;

    seq_set_builder_t ssb;
    seq_set_builder_prep(&ssb);

    jsw_atrav_t atrav;
    jsw_anode_t *node = jsw_atfirst(&atrav, &dn->views);
    for(; node; node = jsw_atnext(&atrav)){
        msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);
        if(view->flags.deleted){
            unsigned int uid_up = view->uid_up;
            PROP_GO(&e, seq_set_builder_add_val(&ssb, uid_up), fail_ssb);
        }
    }

    *out = seq_set_builder_extract(&e, &ssb);
    CHECK(&e);

    return e;

fail_ssb:
    seq_set_builder_free(&ssb);
    return e;
}

derr_t dn_disconnect(dn_t *dn, bool expunge){
    derr_t e = E_OK;

    if(expunge){
        /* calculate a UID EXPUNGE command that we would push to the server,
           and do not disconnect until that response comes in */
        ie_seq_set_t *uids_up;
        PROP(&e, get_uids_up_to_expunge(dn, &uids_up) );
        if(!uids_up){
            // nothing to expunge, disconnect immediately
            PROP(&e, dn->cb->disconnected(dn->cb, NULL) );
        }else{
            // request an expunge update first
            update_req_t *req = update_req_expunge_new(&e, uids_up, dn);
            CHECK(&e);
            dn->disconnect.state = DN_WAIT_WAITING;
            PROP(&e, imaildir_dn_request_update(dn->m, req) );
        }
    }else{
        /* since the server_t is not allowed to process commands while it is
           waiting for the dn_t to respond to a command, there's no additional
           need to synchronize at this point */
        PROP(&e, dn->cb->disconnected(dn->cb, NULL) );
    }

    return e;
}

static derr_t send_flags_update(dn_t *dn, unsigned int seq_num,
        msg_flags_t flags, bool recent){
    derr_t e = E_OK;

    ie_fflags_t *ff = ie_fflags_new(&e);
    if(flags.answered) ff = ie_fflags_add_simple(&e, ff, IE_FFLAG_ANSWERED);
    if(flags.flagged)  ff = ie_fflags_add_simple(&e, ff, IE_FFLAG_FLAGGED);
    if(flags.seen)     ff = ie_fflags_add_simple(&e, ff, IE_FFLAG_SEEN);
    if(flags.draft)    ff = ie_fflags_add_simple(&e, ff, IE_FFLAG_DRAFT);
    if(flags.deleted)  ff = ie_fflags_add_simple(&e, ff, IE_FFLAG_DELETED);
    if(recent)         ff = ie_fflags_add_simple(&e, ff, IE_FFLAG_RECENT);

    // TODO: support modseq here too
    ie_fetch_resp_t *fetch = ie_fetch_resp_new(&e);
    fetch = ie_fetch_resp_num(&e, fetch, seq_num);
    fetch = ie_fetch_resp_flags(&e, fetch, ff);

    imap_resp_arg_t arg = {.fetch=fetch};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_FETCH, arg);
    resp = imap_resp_assert_writable(&e, resp, dn->exts);
    CHECK(&e);

    PROP(&e, dn->cb->resp(dn->cb, resp) );

    return e;
}

static derr_t send_expunge(dn_t *dn, unsigned int seq_num){
    derr_t e = E_OK;

    imap_resp_arg_t arg = { .expunge = seq_num };
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_EXPUNGE, arg);
    resp = imap_resp_assert_writable(&e, resp, dn->exts);
    CHECK(&e);

    PROP(&e, dn->cb->resp(dn->cb, resp) );

    return e;
}


// state for gathering updates
typedef struct {
    jsw_atree_t news;  // gathered_t->node
    jsw_atree_t metas;  // gathered_t->node
    jsw_atree_t expunges;  // gathered_t->node
    /* expunge updates can't be freed until after we have emitted messages for
       them, due to the fact that they have to be removed from the view at the
       time of emitting messages, and that act will alter the seq num of other
       messages, which we need to delay */
    link_t updates_to_free;
    /* Choose not to keep track of what the original flags were.  This means in
       the case of add/remove a flag we will report a FETCH which could be a
       noop, but that's what dovecot does.  It also saves us another struct. */
} gather_t;

typedef struct {
    unsigned int uid_dn;
    jsw_anode_t node;
} gathered_t;
DEF_CONTAINER_OF(gathered_t, node, jsw_anode_t);

static const void *gathered_jsw_get_uid_dn(const jsw_anode_t *node){
    const gathered_t *gathered = CONTAINER_OF(node, gathered_t, node);
    return (void*)&gathered->uid_dn;
}

static void gather_prep(gather_t *gather){
    jsw_ainit(&gather->news, jsw_cmp_uint, gathered_jsw_get_uid_dn);
    jsw_ainit(&gather->metas, jsw_cmp_uint, gathered_jsw_get_uid_dn);
    jsw_ainit(&gather->expunges, jsw_cmp_uint, gathered_jsw_get_uid_dn);
    link_init(&gather->updates_to_free);
}

static void gather_free(gather_t *gather){
    jsw_anode_t *node;
    while((node = jsw_apop(&gather->news))){
        gathered_t *gathered = CONTAINER_OF(node, gathered_t, node);
        free(gathered);
    }
    while((node = jsw_apop(&gather->metas))){
        gathered_t *gathered = CONTAINER_OF(node, gathered_t, node);
        free(gathered);
    }
    while((node = jsw_apop(&gather->expunges))){
        gathered_t *gathered = CONTAINER_OF(node, gathered_t, node);
        free(gathered);
    }
    link_t *link;
    while((link = link_list_pop_first(&gather->updates_to_free))){
        update_t *update = CONTAINER_OF(link, update_t, link);
        update_free(&update);
    }
}

static derr_t gathered_new(gathered_t **out, unsigned int uid_dn){
    derr_t e = E_OK;
    *out = NULL;

    gathered_t *gathered = malloc(sizeof(*gathered));
    if(!out) ORIG(&e, E_NOMEM, "nomem");
    *gathered = (gathered_t){ .uid_dn = uid_dn };

    *out = gathered;

    return e;
}

static void gathered_free(gathered_t *gathered){
    if(!gathered) return;
    free(gathered);
}

static derr_t gather_update_new(dn_t *dn, gather_t *gather, update_t *update){
    derr_t e = E_OK;

    msg_view_t *view = update->arg.new;

    // remember that this uid_dn is new
    gathered_t *gathered;
    PROP_GO(&e, gathered_new(&gathered, view->uid_dn), cu);
    jsw_ainsert(&gather->news, &gathered->node);

    // add the view to our views
    jsw_ainsert(&dn->views, &view->node);
    update->arg.new = NULL;

cu:
    update_free(&update);
    return e;
}

static derr_t gather_update_meta(dn_t *dn, gather_t *gather, update_t *update){
    derr_t e = E_OK;

    msg_view_t *view = update->arg.meta;

    // remember that this uid_dn is modified
    if(!jsw_afind(&gather->metas, &view->uid_dn, NULL)){
        gathered_t *gathered;
        PROP_GO(&e, gathered_new(&gathered, view->uid_dn), cu);
        jsw_ainsert(&gather->metas, &gathered->node);
    }

    // remove the old view
    jsw_anode_t *node = jsw_aerase(&dn->views, &view->uid_dn);
    if(!node){
        // This should never happen since we process messages in order
        ORIG_GO(&e, E_INTERNAL, "missing uid_dn", cu);
    }
    msg_view_t *old_view = CONTAINER_OF(node, msg_view_t, node);
    msg_view_free(&old_view);

    // add the new view
    jsw_ainsert(&dn->views, &view->node);
    update->arg.new = NULL;

cu:
    update_free(&update);
    return e;
}

static derr_t gather_update_expunge(
    dn_t *dn, gather_t *gather, update_t *update
){
    derr_t e = E_OK;
    (void)dn;

    const msg_expunge_t *expunge = update->arg.expunge;

    // remember that this uid is new
    gathered_t *gathered;
    PROP_GO(&e, gathered_new(&gathered, expunge->uid_dn), cu);
    jsw_ainsert(&gather->expunges, &gathered->node);

    // removing the view happens later, so we don't affect the the seq numbers

cu:
    // delay freeing the update until we remove the message from the view
    link_list_append(&gather->updates_to_free, &update->link);
    return e;
}

static derr_t gather_send_responses(dn_t *dn, gather_t *gather){
    derr_t e = E_OK;
    jsw_atrav_t trav;
    jsw_anode_t *node;

    // step 1: any EXPUNGEs don't get FETCHes or count for EXISTS
    node = jsw_atfirst(&trav, &gather->expunges);
    for(; node; node = jsw_atnext(&trav)){
        gathered_t *gathered = CONTAINER_OF(node, gathered_t, node);

        node = jsw_aerase(&gather->news, &gathered->uid_dn);
        if(node) gathered_free(CONTAINER_OF(node, gathered_t, node));

        node = jsw_aerase(&gather->metas, &gathered->uid_dn);
        if(node) gathered_free(CONTAINER_OF(node, gathered_t, node));
    }

    // step 2: any new messages don't get FETCHes
    node = jsw_atfirst(&trav, &gather->news);
    for(; node; node = jsw_atnext(&trav)){
        gathered_t *gathered = CONTAINER_OF(node, gathered_t, node);

        node = jsw_aerase(&gather->metas, &gathered->uid_dn);
        if(node) gathered_free(CONTAINER_OF(node, gathered_t, node));
    }

    // step 3: send flag updates
    node = jsw_atfirst(&trav, &gather->metas);
    for(; node; node = jsw_atnext(&trav)){
        gathered_t *gathered = CONTAINER_OF(node, gathered_t, node);

        // find our view of this uid_dn
        size_t index;
        node = jsw_afind(&dn->views, &gathered->uid_dn, &index);
        if(!node) ORIG(&e, E_INTERNAL, "missing uid_dn");

        msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);

        unsigned int seq_num;
        PROP(&e, index_to_seq_num(index, &seq_num) );
        bool recent = false;
        PROP(&e, send_flags_update(dn, seq_num, view->flags, recent) );
    }

    // step 4: send expunge updates (in reverse order)
    node = jsw_atlast(&trav, &gather->expunges);
    for(; node; node = jsw_atprev(&trav)){
        gathered_t *gathered = CONTAINER_OF(node, gathered_t, node);

        // find our view of this uid_dn
        size_t index;
        jsw_anode_t *node = jsw_afind(&dn->views, &gathered->uid_dn, &index);
        if(!node) ORIG(&e, E_INTERNAL, "missing uid_dn");

        // just remove and delete the view
        msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);
        jsw_aerase(&dn->views, &gathered->uid_dn);
        msg_view_free(&view);

        unsigned int seq_num;
        PROP(&e, index_to_seq_num(index, &seq_num) );
        PROP(&e, send_expunge(dn, seq_num) );
    }

    // step 5: send a new EXISTS response
    if(gather->news.size){
        PROP(&e, send_exists_resp(dn) );
    }

    return e;
}

// send unsolicited untagged responses for external updates to mailbox
derr_t dn_gather_updates(dn_t *dn, bool allow_expunge, ie_st_resp_t **st_resp){
    derr_t e = E_OK;

    /* updates need to be processed in batches:
         - no meta updates should be passed for newly created messages
         - only one FETCH is allowed per UID
         - EXPUNGEs should come next and be reverse-ordered
         - EXISTS should come last, to make a delete-one-create-another
           situation a little less ambiguous */

    gather_t gather;
    gather_prep(&gather);

    update_t *update, *temp;
    LINK_FOR_EACH_SAFE(update, temp, &dn->pending_updates, update_t, link){
        switch(update->type){
            // ignore everything but the status-type response
            case UPDATE_NEW:
                link_remove(&update->link);
                PROP_GO(&e, gather_update_new(dn, &gather, update), cu);
                break;

            case UPDATE_META:
                link_remove(&update->link);
                PROP_GO(&e, gather_update_meta(dn, &gather, update), cu);
                break;

            case UPDATE_EXPUNGE:
                if(!allow_expunge) break;
                link_remove(&update->link);
                PROP_GO(&e, gather_update_expunge(dn, &gather, update), cu);
                break;

            case UPDATE_SYNC:
                // this must be the point up to which we are emitting messages
                if(!st_resp){
                    ORIG_GO(&e, E_INTERNAL, "got unexpected UPDATE_SYNC",  cu);
                }
                *st_resp = STEAL(ie_st_resp_t, &update->arg.sync);
                link_remove(&update->link);
                update_free(&update);
                goto got_update_sync;
        }
    }
    // if we are here, ensure that no UPDATE_SYNC was expected
    if(st_resp != NULL){
        ORIG_GO(&e, E_INTERNAL, "did not find expected UPDATE_SYNC", cu);
    }

got_update_sync:

    PROP_GO(&e, gather_send_responses(dn, &gather), cu);

cu:
    gather_free(&gather);

    // only return a st_resp if we didn't hit an error
    if(is_error(e)){
        ie_st_resp_free(STEAL(ie_st_resp_t, st_resp));
    }

    return e;
}


static derr_t send_store_resp_noupdate(dn_t *dn, const exp_flags_t *exp_flags){
    derr_t e = E_OK;

    size_t index;
    jsw_anode_t *node = jsw_afind(&dn->views, &exp_flags->uid_dn, &index);
    if(!node){
        /* pretty sure this can't happen because this uid would have to appear
           as an update for it to have disappeared */
        ORIG(&e, E_INTERNAL, "missing uid_dn");
    }

    msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);
    if(!msg_flags_eq(exp_flags->flags, view->flags)){
        /* we expected an update but none happened.  I'm pretty sure this only
           happens if the thing got deleted; if it was updated and canceled it
           should have appeared as an update still, and since updates are
           serialized in the imaildir_t and we are only processing the updates
           up to our own update, there's no other possibilities */
        LOG_INFO("deleted message (%x) didn't accept flag change...?\n",
                FU(exp_flags->uid_dn));
        return e;
    }

    // we expected this change, do we report it?
    if(!dn->store.silent){
        unsigned int seq_num;
        PROP(&e, index_to_seq_num(index, &seq_num) );

        PROP(&e, send_flags_update(dn, seq_num, view->flags, view->recent) );
        return e;
    }

    // got an expected change, but it was a .SILENT command

    return e;
}

static derr_t send_store_resp_noexp(dn_t *dn, unsigned int uid_dn){
    derr_t e = E_OK;

    size_t index;
    jsw_anode_t *node = jsw_afind(&dn->views, &uid_dn, &index);
    if(!node){
        ORIG(&e, E_INTERNAL, "missing uid_dn");
    }

    msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);

    unsigned int num;
    if(dn->store.uid_mode){
        num = uid_dn;
    }else{
        PROP(&e, index_to_seq_num(index, &num) );
    }

    PROP(&e, send_flags_update(dn, num, view->flags, view->recent) );
    return e;
}

static derr_t send_store_resp_expupdate(dn_t *dn,
        const exp_flags_t *exp_flags){
    derr_t e = E_OK;

    size_t index;
    jsw_anode_t *node = jsw_afind(&dn->views, &exp_flags->uid_dn, &index);
    if(!node){
        // the update must have deleted this message
        // (this is only possible if we handle the EXPUNGE commands promptly)
        // TODO: handle this properly
        ORIG(&e, E_INTERNAL, "not implemented");
    }

    unsigned int num;
    if(dn->store.uid_mode){
        num = exp_flags->uid_dn;
    }else{
        PROP(&e, index_to_seq_num(index, &num) );
    }

    msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);
    if(!msg_flags_eq(exp_flags->flags, view->flags)){
        // a different update than we expected, always report it
        PROP(&e, send_flags_update(dn, num, view->flags, view->recent) );
        return e;
    }

    // we expected this change, do we report it?
    if(!dn->store.silent){
        PROP(&e, send_flags_update(dn, num, view->flags, view->recent) );
        return e;
    }

    // got an expected change, but it was a .SILENT command

    return e;
}

static derr_t send_store_resp(dn_t *dn, ie_seq_set_t *updated_uids_dn,
        ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    /* Send a FETCH response with for each update.  Unless there was a .SILENT
       store, in which case we ignore those messages.  Unless there was an
       external update that caused one of those messages to be different than
       expected.

       Walk the exp_flags_t's from the STORE command and the updated_uids_dn
       simlutaneously to figure out how to respond to each message. */

    jsw_atrav_t atrav;
    ie_seq_set_trav_t strav;

    exp_flags_t *exp_flags = NULL;
    unsigned int updated_uid_dn = 0;

    jsw_anode_t *node = jsw_atfirst(&atrav, &dn->store.tree);
    exp_flags = CONTAINER_OF(node, exp_flags_t, node);
    updated_uid_dn = ie_seq_set_iter(&strav, updated_uids_dn, 0, 0);

    // quit when we reach the end of both lists
    while(exp_flags || updated_uid_dn){
        if(updated_uid_dn){
            // either there's no more exp_flags or updated_uids_dn is behind
            if(!exp_flags || exp_flags->uid_dn > updated_uid_dn){
                PROP_GO(&e, send_store_resp_noexp(dn, updated_uid_dn), cu);
                updated_uid_dn = ie_seq_set_next(&strav);
                continue;
            }
        }

        if(exp_flags){
            // either there's no more updated_uids_dn or exp_flags is behind
            if(!updated_uid_dn || updated_uid_dn > exp_flags->uid_dn){
                PROP_GO(&e, send_store_resp_noupdate(dn, exp_flags), cu);
                node = jsw_atnext(&atrav);
                exp_flags = CONTAINER_OF(node, exp_flags_t, node);
                continue;
            }
        }

        // otherwise, exp_flags->uid_dn and updated_uid_dn are valid and equal
        PROP_GO(&e, send_store_resp_expupdate(dn, exp_flags), cu);

        node = jsw_atnext(&atrav);
        exp_flags = CONTAINER_OF(node, exp_flags_t, node);
        updated_uid_dn = ie_seq_set_next(&strav);
    }

    if(st_resp){
        // the command failed
        PROP_GO(&e,
            send_st_resp(
                dn,
                dn->store.tag,
                &st_resp->text->dstr,
                st_resp->status
            ),
        cu);
    }else{
        PROP_GO(&e, send_ok(dn, dn->store.tag, &DSTR_LIT("as you wish")), cu);
    }

cu:
    ie_seq_set_free(updated_uids_dn);
    ie_st_resp_free(st_resp);
    return e;
}

static derr_t process_meta_update_for_store(dn_t *dn, update_t *update,
        seq_set_builder_t *uids_dn_ssb){
    derr_t e = E_OK;

    msg_view_t *view = update->arg.meta;

    // remove the old view
    jsw_anode_t *node = jsw_aerase(&dn->views, &view->uid_dn);
    if(!node){
        // TODO: update a pending UPDATE_NEW message with this uid_dn in it
        ORIG_GO(&e, E_INTERNAL, "missing uid_dn", cu);
    }
    msg_view_t *old_view = CONTAINER_OF(node, msg_view_t, node);
    msg_view_free(&old_view);

    // add the new view
    jsw_ainsert(&dn->views, &view->node);
    update->arg.new = NULL;

    PROP_GO(&e, seq_set_builder_add_val(uids_dn_ssb, view->uid_dn), cu);

cu:
    update_free(&update);
    return e;
}

static derr_t do_work_store(dn_t *dn){
    derr_t e = E_OK;

    // prepare a list of uid_dns updates we got
    seq_set_builder_t uids_dn_ssb;
    seq_set_builder_prep(&uids_dn_ssb);

    ie_st_resp_t *st_resp = NULL;

    update_t *update, *temp;
    LINK_FOR_EACH_SAFE(update, temp, &dn->pending_updates, update_t, link){
        bool last_update_to_process = false;

        switch(update->type){
            case UPDATE_NEW:
                // TODO: handle these
                LOG_ERROR("don't know what to do with UPDATE_NEW yet\n");
                break;

            case UPDATE_META:
                // process the event right now
                link_remove(&update->link);
                PROP_GO(&e,
                    process_meta_update_for_store(dn, update, &uids_dn_ssb),
                cu);
                break;

            case UPDATE_EXPUNGE:
                // TODO: handle these in UID mode
                break;

            case UPDATE_SYNC:
                // free the update and break out of the loop
                link_remove(&update->link);
                st_resp = STEAL(ie_st_resp_t, &update->arg.sync);
                update_free(&update);
                last_update_to_process = true;
                break;
        }

        if(last_update_to_process) break;
    }

    ie_seq_set_t *updated_uids_dn = seq_set_builder_extract(&e, &uids_dn_ssb);
    CHECK_GO(&e, cu);

    // send the response to the store command
    PROP_GO(&e,
        send_store_resp(dn, updated_uids_dn, STEAL(ie_st_resp_t, &st_resp)),
    cu);

cu:
    ie_st_resp_free(st_resp);
    seq_set_builder_free(&uids_dn_ssb);
    dn_free_store(dn);
    return e;
}


static derr_t do_work_expunge(dn_t *dn){
    derr_t e = E_OK;

    ie_st_resp_t *st_resp = NULL;

    PROP_GO(&e, dn_gather_updates(dn, true, &st_resp), cu);

    // create an st_resp if there was no error
    if(!st_resp){
        const dstr_t *msg = &DSTR_LIT("may they be cursed forever");
        ie_dstr_t *text = ie_dstr_new(&e, msg, KEEP_RAW);
        ie_status_t status = IE_ST_OK;
        st_resp = ie_st_resp_new(&e,
            STEAL(ie_dstr_t, &dn->expunge.tag), status, NULL, text
        );
        CHECK_GO(&e, cu);
    }else{
        ie_dstr_free(st_resp->tag);
        st_resp->tag = STEAL(ie_dstr_t, &dn->expunge.tag);
    }

    // build response
    imap_resp_arg_t arg = {.status_type=STEAL(ie_st_resp_t, &st_resp)};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);
    resp = imap_resp_assert_writable(&e, resp, dn->exts);
    CHECK_GO(&e, cu);

    PROP_GO(&e, dn->cb->resp(dn->cb, resp), cu);

cu:
    dn_free_expunge(dn);
    ie_st_resp_free(st_resp);
    return e;
}


static derr_t do_work_copy(dn_t *dn){
    derr_t e = E_OK;

    ie_st_resp_t *st_resp = NULL;

    PROP_GO(&e, dn_gather_updates(dn, true, &st_resp), cu);

    // create an st_resp if there was no error
    if(!st_resp){
        const dstr_t *msg = &DSTR_LIT("No one will know the difference");
        ie_dstr_t *text = ie_dstr_new(&e, msg, KEEP_RAW);
        ie_status_t status = IE_ST_OK;
        st_resp = ie_st_resp_new(&e,
            STEAL(ie_dstr_t, &dn->copy.tag), status, NULL, text
        );
        CHECK_GO(&e, cu);
    }else{
        ie_dstr_free(st_resp->tag);
        st_resp->tag = STEAL(ie_dstr_t, &dn->copy.tag);
    }

    // build response
    imap_resp_arg_t arg = {.status_type=STEAL(ie_st_resp_t, &st_resp)};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);
    resp = imap_resp_assert_writable(&e, resp, dn->exts);
    CHECK_GO(&e, cu);

    PROP_GO(&e, dn->cb->resp(dn->cb, resp), cu);

cu:
    dn_free_copy(dn);
    ie_st_resp_free(st_resp);
    return e;
}


static derr_t do_work_disconnect(dn_t *dn){
    derr_t e = E_OK;

    ie_st_resp_t *st_resp = NULL;

    update_t *update, *temp;
    LINK_FOR_EACH_SAFE(update, temp, &dn->pending_updates, update_t, link){
        switch(update->type){
            // ignore everything but the status-type response
            case UPDATE_NEW:
            case UPDATE_META:
            case UPDATE_EXPUNGE:
                break;

            case UPDATE_SYNC:
                // free the update and break out of the loop
                link_remove(&update->link);
                st_resp = STEAL(ie_st_resp_t, &update->arg.sync);
                update_free(&update);
                break;
        }
    }

    // done disconnecting
    PROP_GO(&e,
        dn->cb->disconnected(dn->cb, STEAL(ie_st_resp_t, &st_resp)),
    cu);

cu:
    dn_free_disconnect(dn);
    ie_st_resp_free(st_resp);
    return e;
}


// process updates until we hit our own update
derr_t dn_do_work(dn_t *dn, bool *noop){
    derr_t e = E_OK;

    if(dn->store.state == DN_WAIT_READY){
        *noop = false;
        PROP(&e, do_work_store(dn) );
    }

    if(dn->expunge.state == DN_WAIT_READY){
        *noop = false;
        PROP(&e, do_work_expunge(dn) );
    }

    if(dn->copy.state == DN_WAIT_READY){
        *noop = false;
        PROP(&e, do_work_copy(dn) );
    }

    if(dn->disconnect.state == DN_WAIT_READY){
        *noop = false;
        PROP(&e, do_work_disconnect(dn) );
    }

    return e;
}

void dn_imaildir_update(dn_t *dn, update_t *update){
    // ignore updates that come in before we have built a view
    if(!dn->selected){
        update_free(&update);
        return;
    }

    link_list_append(&dn->pending_updates, &update->link);

    if(update->type == UPDATE_SYNC){
        if(dn->store.state == DN_WAIT_WAITING){
            dn->store.state = DN_WAIT_READY;
        }else if(dn->expunge.state == DN_WAIT_WAITING){
            dn->expunge.state = DN_WAIT_READY;
        }else if(dn->copy.state == DN_WAIT_WAITING){
            dn->copy.state = DN_WAIT_READY;
        }else if(dn->disconnect.state == DN_WAIT_WAITING){
            dn->disconnect.state = DN_WAIT_READY;
        }else{
            LOG_ERROR("dn_t doesn't know what it's waiting for\n");
        }
        dn->cb->enqueue(dn->cb);
    }
}

// we have to free the view as we unregister
void dn_imaildir_preunregister(dn_t *dn){
    /* free all the message views before freeing updates (which might
       invalidate some views) */
    jsw_anode_t *node;
    while((node = jsw_apop(&dn->views))){
        msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);
        msg_view_free(&view);
    }

    // free any unhandled updates
    link_t *link;
    while((link = link_list_pop_first(&dn->pending_updates))){
        update_t *update = CONTAINER_OF(link, update_t, link);
        update_free(&update);
    }

    dn_free_store(dn);
    dn_free_expunge(dn);
    dn_free_copy(dn);
    dn_free_disconnect(dn);
}

bool dn_imaildir_examining(dn_t *dn){
    return dn->examine;
}
