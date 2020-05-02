#include "libimaildir.h"

#include "uv_util.h"

typedef struct {
    unsigned int uid;
    msg_flags_t flags;
    jsw_anode_t node;  // dn_t->store->tree
} exp_flags_t;
DEF_CONTAINER_OF(exp_flags_t, node, jsw_anode_t);

static const void *exp_flags_jsw_get(const jsw_anode_t *node){
    exp_flags_t *exp_flags = CONTAINER_OF(node, exp_flags_t, node);
    return &exp_flags->uid;
}

static derr_t exp_flags_new(exp_flags_t **out, unsigned int uid,
        msg_flags_t flags){
    derr_t e = E_OK;
    *out = NULL;

    exp_flags_t *exp_flags = malloc(sizeof(*exp_flags));
    if(!exp_flags) ORIG(&e, E_NOMEM, "nomem");
    *exp_flags = (exp_flags_t){.uid = uid, .flags = flags};

    *out = exp_flags;

    return e;
}

static void exp_flags_free(exp_flags_t **exp_flags){
    if(!*exp_flags) return;
    free(*exp_flags);
    *exp_flags = NULL;
}

// free everything related to dn.store
static void dn_store_free(dn_t *dn){
    ie_dstr_free(dn->store.tag);
    dn->store.tag = NULL;

    jsw_anode_t *node;
    while((node = jsw_apop(&dn->store.tree))){
        exp_flags_t *exp_flags = CONTAINER_OF(node, exp_flags_t, node);
        exp_flags_free(&exp_flags);
    }
}

// forward declarations
static derr_t conn_dn_cmd(maildir_dn_i*, imap_cmd_t*);
static bool conn_dn_more_work(maildir_dn_i*);
static derr_t conn_dn_do_work(maildir_dn_i*);

static void dn_finalize(refs_t *refs){
    dn_t *dn = CONTAINER_OF(refs, dn_t, refs);

    // free any unhandled updates
    link_t *link;
    while((link = link_list_pop_first(&dn->pending_updates.list))){
        update_t *update = CONTAINER_OF(link, update_t, link);
        update_free(&update);
    }

    uv_mutex_destroy(&dn->pending_updates.mutex);

    // release the conn_dn if we haven't yet
    if(dn->conn) dn->conn->release(dn->conn);

    // free all the message views
    jsw_anode_t *node;
    while((node = jsw_apop(&dn->views))){
        msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);
        msg_view_free(&view);
    }

    dn_store_free(dn);

    refs_free(&dn->refs);

    // free memory
    free(dn);
}

derr_t dn_new(dn_t **out, maildir_conn_dn_i *conn, imaildir_t *m){
    derr_t e = E_OK;
    *out = NULL;

    dn_t *dn = malloc(sizeof(*dn));
    if(!dn) ORIG(&e, E_NOMEM, "nomem");
    *dn = (dn_t){
        .m = m,
        .conn = conn,
        .maildir_dn = {
            .cmd = conn_dn_cmd,
            .more_work = conn_dn_more_work,
            .do_work = conn_dn_do_work,
        },
        .selected = false,
    };

    int ret = uv_mutex_init(&dn->pending_updates.mutex);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing mutex", fail_malloc);
    }

    PROP_GO(&e, refs_init(&dn->refs, 1, dn_finalize), fail_mutex);

    link_init(&dn->link);
    link_init(&dn->pending_updates.list);

    /* the view gets built during processing of the SELECT command, so that the
       CONDSTORE/QRESYNC extensions can be handled efficiently */
    jsw_ainit(&dn->views, jsw_cmp_uid, msg_view_jsw_get);

    jsw_ainit(&dn->store.tree, jsw_cmp_uid, exp_flags_jsw_get);

    *out = dn;

    return e;

fail_mutex:
    uv_mutex_destroy(&dn->pending_updates.mutex);
fail_malloc:
    free(dn);
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
    CHECK(&e);

    dn->conn->resp(dn->conn, resp);

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
    CHECK(&e);

    dn->conn->resp(dn->conn, resp);

    return e;
}

static derr_t send_exists_resp_unsafe(dn_t *dn){
    derr_t e = E_OK;

    if(dn->m->msgs.tree.size > UINT_MAX){
        ORIG(&e, E_VALUE, "too many messages for exists response");
    }
    unsigned int exists = (unsigned int)dn->m->msgs.tree.size;

    imap_resp_arg_t arg = {.exists=exists};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_EXISTS, arg);
    CHECK(&e);

    dn->conn->resp(dn->conn, resp);

    return e;
}

static derr_t send_recent_resp_unsafe(dn_t *dn){
    derr_t e = E_OK;

    // TODO: support \Recent flag

    unsigned int recent = 0;
    imap_resp_arg_t arg = {.recent=recent};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_RECENT, arg);
    CHECK(&e);

    dn->conn->resp(dn->conn, resp);

    return e;
}

static derr_t send_unseen_resp_unsafe(dn_t *dn){
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
    CHECK(&e);

    dn->conn->resp(dn->conn, resp);

    return e;
}

static derr_t send_uidnext_resp_unsafe(dn_t *dn){
    derr_t e = E_OK;

    unsigned int max_uid = 0;

    // check the highest value in msgs tree
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atlast(&trav, &dn->m->msgs.tree);
    if(node){
        msg_base_t *base = CONTAINER_OF(node, msg_base_t, node);
        max_uid = MAX(max_uid, base->ref.uid);
    }

    // check the highest value in expunged tree
    node = jsw_atlast(&trav, &dn->m->expunged.tree);
    if(node){
        msg_expunge_t *expunge = CONTAINER_OF(node, msg_expunge_t, node);
        max_uid = MAX(max_uid, expunge->uid);
    }

    ie_st_code_arg_t code_arg = {.uidnext = max_uid + 1};
    ie_st_code_t *code = ie_st_code_new(&e, IE_ST_CODE_UIDNEXT, code_arg);

    DSTR_STATIC(msg, "get ready, it's coming");
    ie_dstr_t *text = ie_dstr_new(&e, &msg, KEEP_RAW);

    ie_st_resp_t *st_resp = ie_st_resp_new(&e, NULL, IE_ST_OK, code, text);
    imap_resp_arg_t arg = {.status_type=st_resp};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);
    CHECK(&e);

    dn->conn->resp(dn->conn, resp);

    return e;
}

static derr_t send_uidvld_resp(dn_t *dn){
    derr_t e = E_OK;

    maildir_log_i *log = dn->m->log.log;
    ie_st_code_arg_t code_arg = {.uidvld = log->get_uidvld(log)};
    ie_st_code_t *code = ie_st_code_new(&e, IE_ST_CODE_UIDVLD, code_arg);

    DSTR_STATIC(msg, "ride or die");
    ie_dstr_t *text = ie_dstr_new(&e, &msg, KEEP_RAW);

    ie_st_resp_t *st_resp = ie_st_resp_new(&e, NULL, IE_ST_OK, code, text);
    imap_resp_arg_t arg = {.status_type=st_resp};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);
    CHECK(&e);

    dn->conn->resp(dn->conn, resp);

    return e;
}

static derr_t build_views_unsafe(dn_t *dn){
    derr_t e = E_OK;

    /* TODO: wait, if we build the view lazily like this, how do we properly
       ignore updates that come to us before we have updated? */

    // make one view for every message present in the mailbox
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, &dn->m->msgs.tree);
    for(; node != NULL; node = jsw_atnext(&trav)){
        msg_base_t *msg = CONTAINER_OF(node, msg_base_t, node);
        msg_view_t *view;
        PROP(&e, msg_view_new(&view, msg) );
        jsw_ainsert(&dn->views, &view->node);
    }

    return e;
}

static derr_t select_cmd(dn_t *dn, const ie_dstr_t *tag,
        const ie_select_cmd_t *select){
    derr_t e = E_OK;

    // make sure the select did not include QRESYNC or CONDSTORE
    if(select->params){
        ORIG(&e, E_PARAM, "QRESYNC and CONDSTORE not supported");
    }

    // obtain lock
    uv_rwlock_rdlock(&dn->m->msgs.lock);
    uv_rwlock_rdlock(&dn->m->expunged.lock);
    uv_rwlock_rdlock(&dn->m->mods.lock);

    // generate/send required SELECT responses
    PROP_GO(&e, send_flags_resp(dn), unlock);
    PROP_GO(&e, send_exists_resp_unsafe(dn), unlock);
    PROP_GO(&e, send_recent_resp_unsafe(dn), unlock);
    PROP_GO(&e, send_unseen_resp_unsafe(dn), unlock);
    PROP_GO(&e, send_pflags_resp(dn), unlock);
    PROP_GO(&e, send_uidnext_resp_unsafe(dn), unlock);
    PROP_GO(&e, send_uidvld_resp(dn), unlock);

    PROP_GO(&e, build_views_unsafe(dn), unlock);

unlock:
    // release lock
    uv_rwlock_rdunlock(&dn->m->mods.lock);
    uv_rwlock_rdunlock(&dn->m->expunged.lock);
    uv_rwlock_rdunlock(&dn->m->msgs.lock);
    if(is_error(e)){
        goto done;
    }


    /* TODO: check if we are in DITM mode or serve-locally mode.  For now we
             only support serve-locally mode */
    PROP(&e, send_ok(dn, tag, &DSTR_LIT("welcome in")) );

done:
    return e;
}

// nums will be consumed
static derr_t send_search_resp(dn_t *dn, const ie_dstr_t *tag,
        ie_nums_t *nums){
    derr_t e = E_OK;

    // TODO: support modseq here
    bool modseq_present = false;
    unsigned long modseqnum = 0;
    ie_search_resp_t *search = ie_search_resp_new(&e, nums, modseq_present,
            modseqnum);
    imap_resp_arg_t arg = {.search=search};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_SEARCH, arg);
    CHECK(&e);

    dn->conn->resp(dn->conn, resp);

    return e;
}

static derr_t search_cmd(dn_t *dn, const ie_dstr_t *tag,
        const ie_search_cmd_t *search){
    derr_t e = E_OK;

    // handle the empty maildir case
    if(dn->views.size == 0){
        PROP(&e, send_search_resp(dn, tag, NULL) );
        PROP(&e, send_ok(dn, tag, &DSTR_LIT("don't waste my time!")) );
        return e;
    }

    // now figure out some constants to do the search
    unsigned int seq_max = dn->views.size;

    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atlast(&trav, &dn->views);
    msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);

    unsigned int uid_max = view->base->uid;

    // we'll calculate the sequence number as we go
    unsigned int seq = seq_max;

    ie_nums_t *nums = NULL;

    // check every message in the view in reverse order
    for(; node != NULL; node = jsw_atprev(&trav)){
        view = CONTAINER_OF(node, msg_view_t, node);

        bool match;
        PROP_GO(&e, search_key_eval(search->search_key, view, seq, seq_max,
                    uid_max, &match), fail);

        if(match){
            unsigned int val = search->uid_mode ? view->base->uid : seq;
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
    PROP(&e, send_search_resp(dn, tag, nums) );
    PROP(&e, send_ok(dn, tag, &DSTR_LIT("too easy!")) );

    return e;

fail:
    ie_nums_free(nums);
    return e;
}

// take a sequence set and create a UID seq set
// sets *ok = true IFF all numbers present
static derr_t copy_seq_to_uids(dn_t *dn, bool uid_mode,
        const ie_seq_set_t *old, bool *ok, ie_seq_set_t **out){
    derr_t e = E_OK;

    *out = NULL;
    *ok = false;

    // nothing in the tree
    if(dn->views.size == 0){
        return e;
    }

    jsw_anode_t *node;

    // get the last UID or last index, for replacing 0's we see in the seq_set
    unsigned int last;
    if(uid_mode){
        jsw_atrav_t trav;
        jsw_anode_t *node = jsw_atlast(&trav, &dn->views);
        msg_view_t *last_view = CONTAINER_OF(node, msg_view_t, node);
        last = last_view->base->uid;
    } else {
        size_t last_index = dn->views.size - 1;
        PROP(&e, index_to_seq_num(last_index, &last) );
    }

    seq_set_builder_t ssb;
    seq_set_builder_prep(&ssb);

    ie_seq_set_trav_t trav;
    unsigned int i = ie_seq_set_iter(&trav, old, last);
    for(; i != 0; i = ie_seq_set_next(&trav)){
        if(uid_mode){
            // UID in mailbox?
            node = jsw_afind(&dn->views, &i, NULL);
            if(!node) goto fail_ssb;
            PROP_GO(&e, seq_set_builder_add_val(&ssb, i), fail_ssb);
        }else{
            // sequence number in mailbox?
            node = jsw_aindex(&dn->views, (size_t)i - 1);
            if(!node) goto fail_ssb;
            msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);
            unsigned int uid = view->base->uid;
            PROP_GO(&e, seq_set_builder_add_val(&ssb, uid), fail_ssb);
        }
    }

    // all numbers in seq set were present
    *ok = true;
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

    bool ok;
    ie_seq_set_t *uid_seq;
    PROP(&e, copy_seq_to_uids(dn, store->uid_mode, store->seq_set, &ok,
                &uid_seq) );
    if(!ok){
        PROP_GO(&e, send_bad(dn, tag, &DSTR_LIT("bad sequence set")),
                fail_uid_seq);
        goto fail_uid_seq;
    }

    // reset the dn.store state
    dn_store_free(dn);
    dn->store.uid_mode = store->uid_mode;
    dn->store.silent = store->silent;

    dn->store.tag = ie_dstr_copy(&e, tag);
    CHECK_GO(&e, fail_dn_store);

    // figure out what all of the flags we expect to see are
    ie_seq_set_trav_t trav;
    unsigned int uid = ie_seq_set_iter(&trav, uid_seq, 0);
    for(; uid != 0; uid = ie_seq_set_next(&trav)){
        jsw_anode_t *node = jsw_afind(&dn->views, &uid, NULL);
        if(!node){
            ORIG_GO(&e, E_INTERNAL, "uid not found", fail_dn_store);
        }

        msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);

        msg_flags_t new_flags;
        msg_flags_t cmd_flags = msg_flags_from_flags(store->flags);
        switch(store->sign){
            case 0:
                // set flags exactly (new = cmd)
                new_flags = cmd_flags;
                break;
            case 1:
                // add the marked flags (new = old | cmd)
                new_flags = msg_flags_or(*view->flags, cmd_flags);
                break;
            case -1:
                // remove the marked flags (new = old & (~cmd))
                new_flags = msg_flags_and(*view->flags,
                        msg_flags_not(cmd_flags));
                break;
            default:
                ORIG_GO(&e, E_INTERNAL, "invalid store->sign", fail_dn_store);
        }

        // if we expect no difference in flags, omit this uid
        if(msg_flags_eq(new_flags, *view->flags)) continue;

        exp_flags_t *exp_flags;
        PROP_GO(&e, exp_flags_new(&exp_flags, uid, new_flags) , fail_dn_store);
        jsw_ainsert(&dn->store.tree, &exp_flags->node);
    }


    // convert the the range to local uids
    ie_store_cmd_t *uid_store = ie_store_cmd_new(&e,
        store->uid_mode,
        uid_seq,
        ie_store_mods_copy(&e, store->mods),
        store->sign,
        store->silent,
        ie_flags_copy(&e, store->flags)
    );
    uid_seq = NULL;

    // now send the uid_store to the imaildir_t
    update_req_t *req = update_req_store_new(&e, uid_store, dn);

    CHECK_GO(&e, fail_dn_store);

    // at this point, it may be better to leave the dn_store in-tact
    PROP(&e, imaildir_request_update(dn->m, req) );

    return e;

fail_dn_store:
    dn_store_free(dn);
fail_uid_seq:
    ie_seq_set_free(uid_seq);
    return e;
}


static derr_t send_fetch_resp(dn_t *dn, const ie_fetch_cmd_t *fetch,
        unsigned int uid){
    derr_t e = E_OK;

    // get the view
    size_t index;
    jsw_anode_t *node = jsw_afind(&dn->views, &uid, &index);
    if(!node) ORIG(&e, E_INTERNAL, "uid missing");
    msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);

    unsigned int seq_num;
    PROP(&e, index_to_seq_num(index, &seq_num) );

    // build a fetch response
    ie_fetch_resp_t *f = ie_fetch_resp_new(&e);

    f = ie_fetch_resp_num(&e, f, fetch->uid_mode ? uid : seq_num);

    if(fetch->attr->envelope) ORIG_GO(&e, E_INTERNAL, "not implemented", fail);

    if(fetch->attr->flags){
        ie_fflags_t *ff = ie_fflags_new(&e);
        if(view->flags->answered)
            ff = ie_fflags_add_simple(&e, ff, IE_FFLAG_ANSWERED);
        if(view->flags->flagged)
            ff = ie_fflags_add_simple(&e, ff, IE_FFLAG_FLAGGED);
        if(view->flags->seen)
            ff = ie_fflags_add_simple(&e, ff, IE_FFLAG_SEEN);
        if(view->flags->draft)
            ff = ie_fflags_add_simple(&e, ff, IE_FFLAG_DRAFT);
        if(view->flags->deleted)
            ff = ie_fflags_add_simple(&e, ff, IE_FFLAG_DELETED);
        if(view->recent)
            ff = ie_fflags_add_simple(&e, ff, IE_FFLAG_RECENT);
        f = ie_fetch_resp_flags(&e, f, ff);
    }

    if(fetch->attr->intdate){
        f = ie_fetch_resp_intdate(&e, f, view->base->internaldate);
    }

    if(fetch->attr->uid){
        f = ie_fetch_resp_uid(&e, f, uid);
    }

    if(fetch->attr->rfc822){
        int fd;
        IF_PROP(&e, imaildir_open_msg(dn->m, uid, &fd) ){}
        ie_dstr_t *content = ie_dstr_new_from_fd(&e, fd);
        // ignore return value of close on read-only file descriptor
        imaildir_close_msg(dn->m, uid, &fd);
        f = ie_fetch_resp_content(&e, f, content);
    }

    if(fetch->attr->rfc822_header) ORIG_GO(&e, E_INTERNAL, "not implemented", fail);
    if(fetch->attr->rfc822_size) ORIG_GO(&e, E_INTERNAL, "not implemented", fail);
    if(fetch->attr->rfc822_text) ORIG_GO(&e, E_INTERNAL, "not implemented", fail);
    if(fetch->attr->body) ORIG_GO(&e, E_INTERNAL, "not implemented", fail); // means BODY, not BODY[]
    if(fetch->attr->bodystruct) ORIG_GO(&e, E_INTERNAL, "not implemented", fail);
    if(fetch->attr->modseq) ORIG_GO(&e, E_INTERNAL, "not implemented", fail);

    if(fetch->attr->extras) ORIG_GO(&e, E_INTERNAL, "not implemented", fail);

    // finally, send the fetch response
    imap_resp_arg_t arg = {.fetch=f};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_FETCH, arg);

    extensions_t exts = {0};
    resp = imap_resp_assert_writable(&e, resp, &exts);

    CHECK(&e);

    dn->conn->resp(dn->conn, resp);

    return e;

fail:
    ie_fetch_resp_free(f);
    return e;
}


static derr_t fetch_cmd(dn_t *dn, const ie_dstr_t *tag,
        const ie_fetch_cmd_t *fetch){
    derr_t e = E_OK;

    bool ok;
    ie_seq_set_t *uid_seq;
    PROP(&e, copy_seq_to_uids(dn, fetch->uid_mode, fetch->seq_set, &ok,
                &uid_seq) );
    if(!ok){
        PROP_GO(&e, send_bad(dn, tag, &DSTR_LIT("bad sequence set")), cu);
        goto cu;
    }

    // build a response for every uid requested
    ie_seq_set_trav_t trav;
    unsigned int uid = ie_seq_set_iter(&trav, uid_seq, 0);
    for(; uid != 0; uid = ie_seq_set_next(&trav)){
        PROP_GO(&e, send_fetch_resp(dn, fetch, uid), cu);
    }
    uid = ie_seq_set_iter(&trav, uid_seq, 0);
    for(; uid != 0; uid = ie_seq_set_next(&trav)){
        printf("uid: %u\n", uid);
    }

    DSTR_STATIC(msg, "you didn't hear it from me");
    PROP_GO(&e, send_ok(dn, tag, &msg), cu);

cu:
    ie_seq_set_free(uid_seq);

    return e;
}


// we either need to consume the cmd or free it
static derr_t conn_dn_cmd(maildir_dn_i *maildir_dn, imap_cmd_t *cmd){
    derr_t e = E_OK;

    dn_t *dn = CONTAINER_OF(maildir_dn, dn_t, maildir_dn);

    const ie_dstr_t *tag = cmd->tag;
    const imap_cmd_arg_t *arg = &cmd->arg;

    if(cmd->type == IMAP_CMD_SELECT && dn->selected){
        ORIG_GO(&e, E_INTERNAL, "SELECT sent to selected dn_t", cu_cmd);
    }
    if(cmd->type != IMAP_CMD_SELECT && !dn->selected){
        ORIG_GO(&e, E_INTERNAL, "non-SELECT sent to unselected dn_t", cu_cmd);
    }

    switch(cmd->type){
        case IMAP_CMD_SELECT:
            PROP_GO(&e, select_cmd(dn, tag, arg->select), cu_cmd);
            dn->selected = true;
            break;

        case IMAP_CMD_SEARCH:
            PROP_GO(&e, search_cmd(dn, tag, arg->search), cu_cmd);
            break;

        case IMAP_CMD_CHECK:
        case IMAP_CMD_NOOP:
            // TODO: check for pending responses to send here
            PROP_GO(&e, send_ok(dn, tag, &DSTR_LIT("zzzzz...")), cu_cmd);
            break;

        case IMAP_CMD_STORE:
            PROP_GO(&e, store_cmd(dn, tag, arg->store), cu_cmd);
            break;

        case IMAP_CMD_FETCH:
            PROP_GO(&e, fetch_cmd(dn, tag, arg->fetch), cu_cmd);
            break;

        // things we need to support here
        case IMAP_CMD_EXPUNGE:
        case IMAP_CMD_COPY:

        // supported in a different layer
        case IMAP_CMD_CAPA:
        case IMAP_CMD_LOGOUT:
        case IMAP_CMD_CLOSE:
            ORIG_GO(&e, E_INTERNAL, "unhandled command", cu_cmd);

        // unsupported commands in this state
        case IMAP_CMD_STARTTLS:
        case IMAP_CMD_AUTH:
        case IMAP_CMD_LOGIN:
        case IMAP_CMD_EXAMINE:
        case IMAP_CMD_CREATE:
        case IMAP_CMD_DELETE:
        case IMAP_CMD_RENAME:
        case IMAP_CMD_SUB:
        case IMAP_CMD_UNSUB:
        case IMAP_CMD_LIST:
        case IMAP_CMD_LSUB:
        case IMAP_CMD_STATUS:
        case IMAP_CMD_APPEND:
        case IMAP_CMD_ENABLE:
            PROP_GO(&e, send_bad(
                dn, tag, &DSTR_LIT("command not allowed in SELECTED state")
            ), cu_cmd);
            break;
    }

cu_cmd:
    imap_cmd_free(cmd);

    return e;
}

static bool conn_dn_more_work(maildir_dn_i *maildir_dn){
    dn_t *dn = CONTAINER_OF(maildir_dn, dn_t, maildir_dn);
    return dn->pending_updates.ready;
}

static derr_t send_flags_update(dn_t *dn, unsigned int num, msg_flags_t flags,
        bool recent){
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
    fetch = ie_fetch_resp_num(&e, fetch, num);
    fetch = ie_fetch_resp_flags(&e, fetch, ff);

    imap_resp_arg_t arg = {.fetch=fetch};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_FETCH, arg);

    extensions_t exts = {0};
    resp = imap_resp_assert_writable(&e, resp, &exts);

    CHECK(&e);

    dn->conn->resp(dn->conn, resp);

    return e;
}

static derr_t send_store_resp_noupdate(dn_t *dn, const exp_flags_t *exp_flags){
    derr_t e = E_OK;

    size_t index;
    jsw_anode_t *node = jsw_afind(&dn->views, &exp_flags->uid, &index);
    if(!node){
        /* pretty sure this can't happen because this uid would have to appear
           as an update for it to have disappeared */
        ORIG(&e, E_INTERNAL, "missing uid");
    }

    msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);
    if(!msg_flags_eq(exp_flags->flags, *view->flags)){
        /* we expected an update but none happened.  I'm pretty sure this only
           happens if the thing got deleted; if it was updated and canceled it
           should have appeared as an update still, and since updates are
           serialized in the imaildir_t and we are only processing the updates
           up to our own update, there's no other possibilities */
        LOG_INFO("deleted message (%x) didn't accept flag change...?\n",
                FU(exp_flags->uid));
        return e;
    }

    // we expected this change, do we report it?
    if(!dn->store.silent){
        unsigned int num;
        if(dn->store.uid_mode){
            num = exp_flags->uid;
        }else{
            PROP(&e, index_to_seq_num(index, &num) );
        }

        PROP(&e, send_flags_update(dn, num, *view->flags, view->recent) );
        return e;
    }

    // got an expected chage, but it was a .SILENT command

    return e;
}

static derr_t send_store_resp_noexp(dn_t *dn, unsigned int uid){
    derr_t e = E_OK;

    size_t index;
    jsw_anode_t *node = jsw_afind(&dn->views, &uid, &index);
    if(!node){
        ORIG(&e, E_INTERNAL, "missing uid");
    }

    msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);

    unsigned int num;
    if(dn->store.uid_mode){
        num = uid;
    }else{
        PROP(&e, index_to_seq_num(index, &num) );
    }

    PROP(&e, send_flags_update(dn, num, *view->flags, view->recent) );
    return e;
}

static derr_t send_store_resp_expupdate(dn_t *dn, const exp_flags_t *exp_flags){
    derr_t e = E_OK;

    size_t index;
    jsw_anode_t *node = jsw_afind(&dn->views, &exp_flags->uid, &index);
    if(!node){
        // the update must have deleted this message
        // (this is only possible if we handle the EXPUNGE commands promptly)
        // TODO: handle this properly
        ORIG(&e, E_INTERNAL, "not implemented");
    }

    unsigned int num;
    if(dn->store.uid_mode){
        num = exp_flags->uid;
    }else{
        PROP(&e, index_to_seq_num(index, &num) );
    }

    msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);
    if(!msg_flags_eq(exp_flags->flags, *view->flags)){
        // a different update than we expected, always report it
        PROP(&e, send_flags_update(dn, num, *view->flags, view->recent) );
        return e;
    }

    // we expected this change, do we report it?
    if(!dn->store.silent){
        PROP(&e, send_flags_update(dn, num, *view->flags, view->recent) );
        return e;
    }

    // got an expected chage, but it was a .SILENT command

    return e;
}

static derr_t send_store_resp(dn_t *dn, const ie_seq_set_t *updated_uids){
    derr_t e = E_OK;

    /* Send a FETCH response with all the updates.  Unless there was a .SILENT
       store, in which case we ignore those messages.  Unless there was an
       external update that caused one of those messages to be different than
       expected.

       Walk the exp_flags_t's from the STORE command and the updated_uids
       simlutaneously to figure out how to respond to each message. */

    jsw_atrav_t atrav;
    ie_seq_set_trav_t strav;

    exp_flags_t *exp_flags = NULL;
    unsigned int updated_uid = 0;

    jsw_anode_t *node = jsw_atfirst(&atrav, &dn->store.tree);
    exp_flags = CONTAINER_OF(node, exp_flags_t, node);
    updated_uid = ie_seq_set_iter(&strav, updated_uids, 0);

    // quit when we reach the end of both lists
    while(exp_flags || updated_uid){

        // either there's no more exp_flags or updated_uids is behind
        if(!exp_flags || exp_flags->uid > updated_uid){
            PROP(&e, send_store_resp_noexp(dn, updated_uid) );
            updated_uid = ie_seq_set_next(&strav);
            continue;
        }

        // either there's no more updated_uids or exp_flags is behind
        if(!updated_uid || updated_uid > exp_flags->uid){
            PROP(&e, send_store_resp_noupdate(dn, exp_flags) );
            node = jsw_atnext(&atrav);
            exp_flags = CONTAINER_OF(node, exp_flags_t, node);
            continue;
        }

        // otherwise, we know exp_flags->uid and updated_uid are valid and equal
        PROP(&e, send_store_resp_expupdate(dn, exp_flags) );

        node = jsw_atnext(&atrav);
        exp_flags = CONTAINER_OF(node, exp_flags_t, node);
        updated_uid = ie_seq_set_next(&strav);
    }

    PROP(&e, send_ok(dn, dn->store.tag, &DSTR_LIT("as you wish")) );

    return e;
}

static derr_t process_meta_update(dn_t *dn, const update_t *update,
        seq_set_builder_t *ssb){
    derr_t e = E_OK;

    update_val_t *update_val;
    LINK_FOR_EACH(update_val, &update->updates, update_val_t, link){
        const msg_meta_t *meta = update_val->val.meta;

        // find our view of this uid
        jsw_anode_t *node = jsw_afind(&dn->views, &meta->uid, NULL);
        if(!node){
            // TODO: update a pending UPDATE_NEW message with this uid in it
            ORIG(&e, E_INTERNAL, "missing uid");
        }

        // update the meta in our view
        msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);
        view->flags = &meta->flags;

        PROP(&e, seq_set_builder_add_val(ssb, meta->uid) );
    }

    return e;
}

// process updates until we hit our own update
static derr_t conn_dn_do_work(maildir_dn_i *maildir_dn){
    derr_t e = E_OK;
    derr_t e2;

    dn_t *dn = CONTAINER_OF(maildir_dn, dn_t, maildir_dn);

    uv_mutex_lock(&dn->pending_updates.mutex);
    dn->pending_updates.ready = false;

    // prepare a list of updates we got
    seq_set_builder_t ssb;
    seq_set_builder_prep(&ssb);

    update_t *update, *temp;
    LINK_FOR_EACH_SAFE(
        update, temp, &dn->pending_updates.list, update_t, link
    ){
        bool last_update_to_process = (update->requester == dn);

        switch(update->type){
            case UPDATE_NEW:
                LOG_ERROR("don't know what to do with UPDATE_NEW yet\n");
                break;

            case UPDATE_META:
                // process the event right now
                e2 = process_meta_update(dn, update, &ssb);
                link_remove(&update->link);
                update_free(&update);
                PROP_VAR_GO(&e, &e2, cu);
                break;

            case UDPATE_EXPUNGE:
                LOG_ERROR("don't know what to do with UPDATE_EXPUNGE yet\n");
                break;
        }

        if(last_update_to_process) break;
    }

    ie_seq_set_t *updated_uids = seq_set_builder_extract(&e, &ssb);
    CHECK_GO(&e, cu);

    // send the response to the store command
    e2 = send_store_resp(dn, updated_uids);
    ie_seq_set_free(updated_uids);
    PROP_VAR_GO(&e, &e2, cu);

cu:
    seq_set_builder_free(&ssb);
    dn_store_free(dn);

    uv_mutex_unlock(&dn->pending_updates.mutex);

    return e;
}

void dn_update(dn_t *dn, update_t *update){
    // was this our request?
    bool advance = update->requester == dn;

    uv_mutex_lock(&dn->pending_updates.mutex);
    link_list_append(&dn->pending_updates.list, &update->link);
    uv_mutex_unlock(&dn->pending_updates.mutex);

    if(advance){
        dn->pending_updates.ready = true;
        dn->conn->advance(dn->conn);
    }
}
