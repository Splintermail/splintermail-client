#include <stdlib.h>

#include "libimaildir.h"

#include "libuvthread/libuvthread.h"

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
DEF_CONTAINER_OF(gathered_t, node, jsw_anode_t)

// forward declarations
static derr_t get_msg_keys_to_expunge(dn_t *dn, msg_key_list_t **out);

static const void *gathered_jsw_get_uid_dn(const jsw_anode_t *node);
static void gather_prep(gather_t *gather);
static void gather_free(gather_t *gather);
static derr_t gathered_new(gathered_t **out, unsigned int uid_dn);
static void gathered_free(gathered_t *gathered);
static derr_t gather_updates_dont_send(
    dn_t *dn,
    bool allow_expunge,
    ie_st_resp_t **st_resp,
    gather_t *gather
);
static derr_t gather_send_responses(
    dn_t *dn, gather_t *gather, bool uid_mode, bool for_store
);
static derr_t send_store_fetch_resps(dn_t *dn, gather_t *gather);

// a helper struct to lazily load the message body
typedef struct {
    imaildir_t *m;
    msg_key_t key;
    int fd;
    bool eof;
    ie_dstr_t *content;
    bool taken; // the first taker gets *content; following takers get a copy
    imf_hdrs_t *hdrs;
    imf_t *imf;
} loader_t;

static loader_t loader_prep(imaildir_t *m, msg_key_t key){
    return (loader_t){ .m = m, .key = key, .fd = -1 };
}

// get the next 4096 bytes of data from the file
// returns the amount read (0 means eof or error)
static size_t _loader_read(derr_t *e, loader_t *loader){
    if(is_error(*e)) goto fail;
    if(loader->eof) return 0;

    if(loader->fd == -1){
        PROP_GO(e,
            imaildir_dn_open_msg(loader->m, loader->key, &loader->fd),
        fail);
        loader->content = ie_dstr_new_empty(e);
        CHECK_GO(e, fail);
    }

    // read 4096 chunks at a time
    size_t amnt_read;
    PROP_GO(e,
        dstr_read(loader->fd, &loader->content->dstr, 4096, &amnt_read),
    fail);
    if(amnt_read == 0) loader->eof = true;

    return amnt_read;

fail:
    return 0;
}

static void _loader_read_all(derr_t *e, loader_t *loader){
    while(!is_error(*e) && !loader->eof){
        _loader_read(e, loader);
    }
}

// // get the content but don't take ownership
// static ie_dstr_t *loader_get_content(derr_t *e, loader_t *loader){
//     if(is_error(*e)) goto fail;
//
//     _loader_read_all(e, loader);
//     CHECK_GO(e, fail);
//
//     return loader->content;
//
// fail:
//     return NULL;
// }

// take ownership of the content (or a copy if the original is spoken for)
static ie_dstr_t *loader_take_content(derr_t *e, loader_t *loader){
    if(is_error(*e)) goto fail;

    _loader_read_all(e, loader);

    if(loader->taken){
        return ie_dstr_copy(e, loader->content);
    }

    loader->taken = true;
    return loader->content;

fail:
    return NULL;
}

// a closure around _loader_read, for imf_scanner_t
static derr_t _loader_read_fn(void *data, size_t *amnt_read){
    derr_t e = E_OK;
    *amnt_read = _loader_read(&e, (loader_t*)data);
    CHECK(&e);
    return e;
}

static const imf_hdrs_t *loader_parse_hdrs(derr_t *e, loader_t *loader){
    if(is_error(*e)) goto fail;
    // pre-parsed content may be just the headers or the whole imf
    if(loader->hdrs) return loader->hdrs;
    if(loader->imf) return loader->imf->hdrs;

    if(!loader->content){
        // read at least one chunk so we have a valid loader->content
        _loader_read(e, loader);
        CHECK_GO(e, fail);
    }

    PROP_GO(e,
        imf_hdrs_parse(
            &loader->content->dstr,
            _loader_read_fn,
            (void*)loader,
            &loader->hdrs
        ),
    fail);

    return loader->hdrs;

fail:
    return NULL;
}

// parse the content into an imf_t
static const imf_t *loader_parse_imf(derr_t *e, loader_t *loader){
    if(is_error(*e)) goto fail;
    if(loader->imf) return loader->imf;

    if(!loader->content){
        // read at least one chunk so we have a valid loader->content
        _loader_read(e, loader);
        CHECK_GO(e, fail);
    }

    PROP_GO(e,
        imf_parse(
            &loader->content->dstr,
            _loader_read_fn,
            (void*)loader,
            &loader->hdrs,
            &loader->imf
        ),
    fail);

    return loader->imf;

fail:
    return NULL;
}

static void loader_close(derr_t *e, loader_t *loader){
    imf_hdrs_free(loader->hdrs);
    imf_free(loader->imf);
    loader->imf = NULL;
    if(!loader->taken){
        ie_dstr_free(loader->content);
    }
    loader->content = NULL;

    // if imalidir fails in this call, this will overwrite e with E_IMAILDIR
    derr_t e2 = imaildir_dn_close_msg(loader->m, loader->key, &loader->fd);
    if(is_error(e2)){
        PROP_VAR_GO(e, &e2, fail);
    }

fail:
    return;
}

typedef struct {
    unsigned int uid_dn;
    msg_flags_t flags;
    jsw_anode_t node;  // dn_t->store->tree
} exp_flags_t;
DEF_CONTAINER_OF(exp_flags_t, node, jsw_anode_t)

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
    dn->copy.uid_mode = false;
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
}

static derr_t send_st_resp(
    dn_t *dn, const ie_dstr_t *tag, const dstr_t *msg, ie_status_t status
){
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

static derr_t send_ok(dn_t *dn, const ie_dstr_t *tag, const dstr_t *msg){
    derr_t e = E_OK;
    PROP(&e, send_st_resp(dn, tag, msg, IE_ST_OK) );
    return e;
}

static derr_t send_no(dn_t *dn, const ie_dstr_t *tag, const dstr_t *msg){
    derr_t e = E_OK;
    PROP(&e, send_st_resp(dn, tag, msg, IE_ST_NO) );
    return e;
}

static derr_t send_bad(dn_t *dn, const ie_dstr_t *tag, const dstr_t *msg){
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

    // we don't support \Recent
    imap_resp_arg_t arg = {.recent=0};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_RECENT, arg);
    resp = imap_resp_assert_writable(&e, resp, dn->exts);
    CHECK(&e);

    PROP(&e, dn->cb->resp(dn->cb, resp) );

    return e;
}

static derr_t send_unseen_resp(dn_t *dn){
    derr_t e = E_OK;

    /* note: UNSEEN can't be calculated in constant memory while building the
       view because you have to have the complete view already constructed to
       be able to properly report the lowest UNSEEN sequence number */

    unsigned int seq = 1;
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, &dn->views);
    for(; node != NULL; seq++, node = jsw_atprev(&trav)){
        msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);
        if(view->flags.seen) continue;

        // found the first UNSEEN
        ie_st_code_arg_t code_arg = {.unseen = seq};
        ie_st_code_t *code = ie_st_code_new(&e, IE_ST_CODE_UNSEEN, code_arg);

        DSTR_STATIC(msg, "you ain't seen nuthin yet");
        ie_dstr_t *text = ie_dstr_new(&e, &msg, KEEP_RAW);

        ie_st_resp_t *st_resp = ie_st_resp_new(&e, NULL, IE_ST_OK, code, text);
        imap_resp_arg_t arg = {.status_type=st_resp};
        imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);
        resp = imap_resp_assert_writable(&e, resp, dn->exts);
        CHECK(&e);

        PROP(&e, dn->cb->resp(dn->cb, resp) );
    }

    // if no messages were unseen, don't report anything

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
    jsw_anode_t *node;

    // make sure the select did not include QRESYNC or CONDSTORE
    if(select->params){
        ORIG(&e, E_PARAM, "QRESYNC and CONDSTORE not supported");
    }

    unsigned int max_uid_dn;
    unsigned int uidvld_dn;
    PROP(&e,
        imaildir_dn_build_views(dn->m,
            &dn->views,
            &max_uid_dn,
            &uidvld_dn
        )
    );

    // generate/send required SELECT responses
    PROP_GO(&e, send_flags_resp(dn), fail);
    PROP_GO(&e, send_exists_resp(dn), fail);
    PROP_GO(&e, send_recent_resp(dn), fail);
    PROP_GO(&e, send_unseen_resp(dn), fail);
    PROP_GO(&e, send_pflags_resp(dn), fail);
    PROP_GO(&e, send_uidnext_resp(dn, max_uid_dn), fail);
    PROP_GO(&e, send_uidvld_resp(dn, uidvld_dn), fail);

    // build an appropriate OK message
    ie_dstr_t *tag_copy = ie_dstr_copy(&e, tag);
    ie_dstr_t *text = ie_dstr_new2(&e, DSTR_LIT("welcome in"));
    ie_st_code_arg_t code_arg = {0};
    ie_st_code_t *code = ie_st_code_new(&e,
        dn->examine ? IE_ST_CODE_READ_ONLY : IE_ST_CODE_READ_WRITE, code_arg
    );
    ie_st_resp_t *st_resp = ie_st_resp_new(&e, tag_copy, IE_ST_OK, code, text);
    imap_resp_arg_t arg = {.status_type=st_resp};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);
    resp = imap_resp_assert_writable(&e, resp, dn->exts);
    CHECK_GO(&e, fail);

    PROP_GO(&e, dn->cb->resp(dn->cb, resp), fail);

    return e;

fail:
    while((node = jsw_apop(&dn->views))){
        msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);
        msg_view_free(&view);
    }
    return e;
}

// nums will be consumed
static derr_t send_search_resp(dn_t *dn, ie_nums_t *nums){
    derr_t e = E_OK;

    // TODO: support modseq here
    bool modseq_present = false;
    uint64_t modseqnum = 0;
    ie_search_resp_t *search = ie_search_resp_new(&e, nums, modseq_present,
            modseqnum);
    imap_resp_arg_t arg = {.search=search};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_SEARCH, arg);
    resp = imap_resp_assert_writable(&e, resp, dn->exts);
    CHECK(&e);

    PROP(&e, dn->cb->resp(dn->cb, resp) );

    return e;
}

// a closure around loader_parse_hdrs for search_key_eval()
static derr_t _loader_parse_hdrs_fn(void *data, const imf_hdrs_t **hdrs){
    derr_t e = E_OK;
    *hdrs = loader_parse_hdrs(&e, (loader_t*)data);
    CHECK(&e);
    return e;
}


// a closure around loader_parse_hdrs for search_key_eval()
static derr_t _loader_parse_imf_fn(void *data, const imf_t **imf){
    derr_t e = E_OK;
    *imf = loader_parse_imf(&e, (loader_t*)data);
    CHECK(&e);
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
    loader_t loader = {0};

    // check every message in the view in reverse order
    for(; node != NULL; node = jsw_atprev(&trav)){
        view = CONTAINER_OF(node, msg_view_t, node);

        loader = loader_prep(dn->m, view->key);

        bool match;
        PROP_GO(&e,
            search_key_eval(
                search->search_key,
                view,
                seq,
                seq_max,
                uid_dn_max,
                _loader_parse_hdrs_fn,
                &loader,
                _loader_parse_imf_fn,
                &loader,
                &match
            ),
        fail);

        loader_close(&e, &loader);
        CHECK_GO(&e, fail);

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
    {
        derr_t e2 = E_OK;
        loader_close(&e2, &loader);
        // prefer the E_IMAILDIR error
        MERGE_VAR(&e2, &e, "closing lazy-loaded message fd");
        // but store it at e
        e = e2;
    }
    ie_nums_free(nums);

    return e;
}

/* Take a sequence set and create a "canonical" uid_dn seq set.
   A "canonical" set means it is well-ordered, has no duplicates, and every
   value in the seq set is present in the dn_t's view. */
static derr_t seq_set_to_canonical_uids_dn(
    dn_t *dn,
    bool uid_mode,
    const ie_seq_set_t *old,
    ie_seq_set_t **out
){
    derr_t e = E_OK;

    *out = NULL;

    // nothing in the tree
    if(dn->views.size == 0){
        return e;
    }

    jsw_anode_t *node;

    // get the last UID or last index, for replacing 0's we see in the seq_set
    // also get the starting index
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
        unsigned int uid_out = view->uid_dn;
        PROP_GO(&e, seq_set_builder_add_val(&ssb, uid_out), fail_ssb);
    }

    *out = seq_set_builder_extract(&e, &ssb);
    CHECK(&e);

    return e;

fail_ssb:
    seq_set_builder_free(&ssb);
    return e;
}

// uids_dn must be a canonical uid_dn list
static derr_t uids_dn_to_msg_keys(
    dn_t *dn, const ie_seq_set_t *uids_dn, msg_key_list_t **out
){
    derr_t e = E_OK;

    *out = NULL;
    msg_key_list_t *keys = NULL;

    // no need for first or last since uids_dn is canonicalized
    ie_seq_set_trav_t trav;
    unsigned int i = ie_seq_set_iter(&trav, uids_dn, 0, 0);
    for(; i != 0; i = ie_seq_set_next(&trav)){
        // uid_dn in mailbox?
        jsw_anode_t *node = jsw_afind(&dn->views, &i, NULL);
        if(!node){
            ORIG_GO(&e, E_INTERNAL, "canonical list has missing uid", cu);
        }
        msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);
        keys = msg_key_list_new(&e, view->key, keys);
        CHECK_GO(&e, cu);
    }

    *out = keys;

cu:
    if(is_error(e)) msg_key_list_free(keys);
    return e;
}


static derr_t store_cmd(
    dn_t *dn, const ie_dstr_t *tag, const ie_store_cmd_t *store
){
    derr_t e = E_OK;

    ie_seq_set_t *uids_dn = NULL;
    msg_key_list_t *keys = NULL;

    // we need each msg_keys for relaying the store command upwards
    // start by canonicalizing the uids_dn
    PROP(&e,
        seq_set_to_canonical_uids_dn(
            dn, store->uid_mode, store->seq_set, &uids_dn
        )
    );

    // detect noop STOREs
    if(!uids_dn){
        bool allow_expunge = store->uid_mode;
        PROP(&e, dn_gather_updates(dn, allow_expunge, store->uid_mode, NULL) );
        PROP(&e, send_ok(dn, tag, &DSTR_LIT("noop STORE command")) );
        return e;
    }

    // convert to msg_keys
    PROP_GO(&e, uids_dn_to_msg_keys(dn, uids_dn, &keys), cu);

    // reset the dn.store state
    dn_free_store(dn);
    dn->store.uid_mode = store->uid_mode;
    dn->store.silent = store->silent;

    dn->store.tag = ie_dstr_copy(&e, tag);
    CHECK_GO(&e, cu);

    // figure out what all of the flags we expect to see are
    msg_flags_t cmd_flags = msg_flags_from_flags(store->flags);
    ie_seq_set_trav_t trav;
    unsigned int uid_dn = ie_seq_set_iter(&trav, uids_dn, 0, 0);
    for(; uid_dn != 0; uid_dn = ie_seq_set_next(&trav)){
        jsw_anode_t *node = jsw_afind(&dn->views, &uid_dn, NULL);
        if(!node){
            ORIG_GO(&e, E_INTERNAL, "uid_dn not found", cu);
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
                ORIG_GO(&e, E_INTERNAL, "invalid store->sign", cu);
        }

        exp_flags_t *exp_flags;
        PROP_GO(&e,
            exp_flags_new(&exp_flags, uid_dn, new_flags),
        cu);
        jsw_ainsert(&dn->store.tree, &exp_flags->node);
    }

    // TODO: run all STOREs as silent to save bandwidth
    bool silent = false;
    msg_store_cmd_t *msg_store = msg_store_cmd_new(&e,
        STEAL(msg_key_list_t, &keys),
        ie_store_mods_copy(&e, store->mods),
        store->sign,
        silent,
        ie_flags_copy(&e, store->flags)
    );

    // now send the msg_store to the imaildir_t
    update_req_t *req = update_req_store_new(&e, msg_store, dn);

    CHECK_GO(&e, cu);

    dn->store.state = DN_WAIT_WAITING;
    PROP_GO(&e, imaildir_dn_request_update(dn->m, req), cu);

cu:
    msg_key_list_free(keys);
    ie_seq_set_free(uids_dn);

    return e;
}


static ie_dstr_t *copy_hdr_fields(
    derr_t *e,
    const imf_hdrs_t *hdrs,
    const ie_dstr_t *names
){
    if(is_error(*e)) return NULL;

    ie_dstr_t *out = ie_dstr_new_empty(e);
    const imf_hdr_t *hdr = hdrs->hdr;
    for( ; hdr; hdr = hdr->next){
        const ie_dstr_t *name = names;
        for( ; name; name = name->next){
            if(dstr_icmp2(dstr_from_off(hdr->name), name->dstr) == 0){
                dstr_t hdr_bytes = dstr_from_off(hdr->bytes);
                out = ie_dstr_append(e, out, &hdr_bytes, KEEP_RAW);
                break;
            }
        }
    }
    // include any post-header separator line
    dstr_t sep = dstr_from_off(hdrs->sep);
    out = ie_dstr_append(e, out, &sep, KEEP_RAW);
    return out;
}


static ie_dstr_t *copy_hdr_fields_not(
    derr_t *e,
    const imf_hdrs_t *hdrs,
    const ie_dstr_t *names
){
    if(is_error(*e)) return NULL;

    ie_dstr_t *out = ie_dstr_new_empty(e);
    const imf_hdr_t *hdr = hdrs->hdr;
    for( ; hdr; hdr = hdr->next){
        const ie_dstr_t *name = names;
        bool keep = true;
        for( ; name; name = name->next){
            if(dstr_icmp2(dstr_from_off(hdr->name), name->dstr) == 0){
                keep = false;
                break;
            }
        }
        if(keep){
            dstr_t hdr_bytes = dstr_from_off(hdr->bytes);
            out = ie_dstr_append(e, out, &hdr_bytes, KEEP_RAW);
        }
    }
    // include any post-header separator line
    dstr_t sep = dstr_from_off(hdrs->sep);
    out = ie_dstr_append(e, out, &sep, KEEP_RAW);
    return out;
}

// imf_t-level wrapper
static ie_dstr_t *imf_copy_hdr_fields(
    derr_t *e, const imf_t *imf, const ie_dstr_t *names
){
    if(is_error(*e)) return NULL;
    return copy_hdr_fields(e, imf->hdrs, names);
}

// imf_t-level wrapper
static ie_dstr_t *imf_copy_hdr_fields_not(
    derr_t *e, const imf_t *imf, const ie_dstr_t *names
){
    if(is_error(*e)) return NULL;
    return copy_hdr_fields_not(e, imf->hdrs, names);
}


// returns true if the request is not actually fetchable
/* (mostly in its own function because the memory freeing requirements of the
   of the calling code are complex) */
static bool build_sect_txt_content_resp(
    derr_t *e,
    const ie_sect_txt_t *sect_txt,
    const dstr_off_t bytes,
    const dstr_off_t mime_hdrs,
    const imf_t *imf,
    ie_dstr_t **content_resp_out
){
    *content_resp_out = NULL;
    if(is_error(*e)) return false;

    if(!sect_txt){
        /* A missing sect_text indicates the whole submessage:
           ex: 1.2.3 */
        *content_resp_out = ie_dstr_from_off(e, bytes);
        return false;
    }

    switch(sect_txt->type){
        case IE_SECT_MIME:
            *content_resp_out = ie_dstr_from_off(e, mime_hdrs);
            break;

        case IE_SECT_TEXT:
            if(!imf) return true;
            // just the body of the message
            *content_resp_out = ie_dstr_from_off(e, imf->body);
            break;

        case IE_SECT_HEADER:
            if(!imf) return true;
            // just the headers of the message
            *content_resp_out = ie_dstr_from_off(e, imf->hdrs->bytes);
            break;

        case IE_SECT_HDR_FLDS:
            if(!imf) return true;
            // just the headers of the message
            *content_resp_out = imf_copy_hdr_fields(
                e, imf, sect_txt->headers
            );
            break;

        case IE_SECT_HDR_FLDS_NOT:
            // just the headers of the message
            *content_resp_out = imf_copy_hdr_fields_not(
                e, imf, sect_txt->headers
            );
            break;
    }

    return false;
}


static ie_fetch_resp_t *build_fetch_resp_extra(
    derr_t *e,
    ie_fetch_resp_t *f,
    ie_fetch_extra_t *extra,
    loader_t *loader,
    bool *part_missing
){
    ie_dstr_t *content_resp = NULL;

    *part_missing = false;
    if(is_error(*e)) goto fail;

    ie_sect_t *sect = extra->sect;
    if(!sect){
        // BODY[] by itself
        content_resp = loader_take_content(e, loader);

    /* optimization clause: detect cases where we don't have to read the
       whole message at all.  These cases meet the following criteria:
         - no sect_part specified (no nested imf's)
         - sect_txt is specified
         - only concerned with content of headers */
    }else if(
        sect->sect_part == NULL
        && sect->sect_txt != NULL
        && (
            sect->sect_txt->type == IE_SECT_HEADER
            || sect->sect_txt->type == IE_SECT_HDR_FLDS
            || sect->sect_txt->type == IE_SECT_HDR_FLDS_NOT
        )
    ){
        // parse just the headers
        const imf_hdrs_t *hdrs = loader_parse_hdrs(e, loader);
        if(sect->sect_txt->type == IE_SECT_HEADER){
            dstr_t bytes = dstr_from_off(hdrs->bytes);
            content_resp = ie_dstr_new(e, &bytes, KEEP_RAW);
        }else if(sect->sect_txt->type == IE_SECT_HDR_FLDS){
            content_resp = copy_hdr_fields(
                e, hdrs, sect->sect_txt->headers
            );
        }else{  // IE_SECT_HDR_FLDS_NOT
            content_resp = copy_hdr_fields_not(
                e, hdrs, sect->sect_txt->headers
            );
        }

    // general clause: parse the whole message up front
    }else{
        // first read the whole message into memory
        const imf_t *root_imf = loader_parse_imf(e, loader);

        /* if present, the sect part marks which submessage to fetch from:
           ex: 1.2.3.MIME
               ^^^^^       */
        dstr_off_t bytes;
        dstr_off_t mime_hdrs;
        const imf_t *imf;
        imf_t *heap_imf; // always free...
        bool missing = imf_get_submessage(e,
            root_imf, sect->sect_part, &bytes, &mime_hdrs, &imf, &heap_imf
        );
        if(!missing){
            /* if present, the sect txt marks what to fetch from the message:
                   1.2.3.MIME
                         ^^^^  */
            missing = build_sect_txt_content_resp(e,
                sect->sect_txt, bytes, mime_hdrs, imf, &content_resp
            );
        }

        // ... always free:
        imf_free(heap_imf);

        CHECK_GO(e, fail);

        if(missing){
            *part_missing = true;
            return f;
        }
    }

    ie_nums_t *offset = NULL;
    if(extra->partial){
        // replace content_resp with a substring of the content_resp
        size_t start = extra->partial->a;  // a = starting byte
        size_t end = start + extra->partial->b;  // b = length
        dstr_t sub = ie_dstr_sub(content_resp, start, end);
        ie_dstr_t *temp = content_resp;
        content_resp = ie_dstr_new(e, &sub, KEEP_RAW);
        ie_dstr_free(temp);
        // in the response, only the start offset is specified
        offset = ie_nums_new(e, extra->partial->a);
    }

    // extend the fetch_resp
    ie_sect_t *sect_resp = ie_sect_copy(e, sect);
    ie_fetch_resp_extra_t *extra_resp =
        ie_fetch_resp_extra_new(e, sect_resp, offset, content_resp);
    f = ie_fetch_resp_add_extra(e, f, extra_resp);

    return f;

fail:
    ie_fetch_resp_free(f);
    ie_dstr_free(content_resp);
    return NULL;
}


static derr_t send_fetch_resp(
    dn_t *dn,
    const ie_fetch_cmd_t *fetch,
    unsigned int uid_dn,
    bool force_flags,
    bool *part_missing
){
    derr_t e = E_OK;
    *part_missing = false;

    // get the view
    size_t index;
    jsw_anode_t *node = jsw_afind(&dn->views, &uid_dn, &index);
    if(!node) ORIG(&e, E_INTERNAL, "uid_dn missing");
    msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);

    unsigned int seq_num;
    PROP(&e, index_to_seq_num(index, &seq_num) );

    // handle lazy loading of the content
    loader_t loader = loader_prep(dn->m, view->key);

    // build a fetch response
    ie_fetch_resp_t *f = ie_fetch_resp_new(&e);

    // the num is always a sequence number, even in case of a UID FETCH
    f = ie_fetch_resp_seq_num(&e, f, seq_num);

    if(fetch->attr->envelope){
        const imf_hdrs_t *hdrs = loader_parse_hdrs(&e, &loader);
        ie_envelope_t *envelope = read_envelope_info(&e, hdrs);
        f = ie_fetch_resp_envelope(&e, f, envelope);
    }

    // if we gathered an flag update for this uid_dn, we always include FLAGS
    if(fetch->attr->flags || force_flags){
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
        f = ie_fetch_resp_flags(&e, f, ff);
    }

    if(fetch->attr->intdate){
        f = ie_fetch_resp_intdate(&e, f, view->internaldate);
    }

    // UID is implcitly requested by all UID FETCH commands
    if(fetch->attr->uid || fetch->uid_mode){
        f = ie_fetch_resp_uid(&e, f, uid_dn);
    }

    if(fetch->attr->rfc822){
        // take ownership of the content
        f = ie_fetch_resp_rfc822(&e, f, loader_take_content(&e, &loader));
    }

    if(fetch->attr->rfc822_header){
        // parse the headers and copy the header byes
        const imf_hdrs_t *hdrs = loader_parse_hdrs(&e, &loader);
        dstr_t text = dstr_from_off(hdrs->bytes);
        f = ie_fetch_resp_rfc822_hdr(&e, f, ie_dstr_new(&e, &text, KEEP_RAW));
    }

    if(fetch->attr->rfc822_text){
        const imf_t *imf = loader_parse_imf(&e, &loader);
        f = ie_fetch_resp_rfc822_text(&e, f, ie_dstr_from_off(&e, imf->body));
    }

    if(fetch->attr->rfc822_size){
        if(view->length > UINT_MAX){
            TRACE_ORIG(&e, E_INTERNAL, "msg length exceeds UINT_MAX");
        }else{
            unsigned int size = (unsigned int)view->length;
            f = ie_fetch_resp_rfc822_size(&e, f, ie_nums_new(&e, size));
        }
    }

    // BODY, not BODY[]
    if(fetch->attr->body){
        const imf_t *root_imf = loader_parse_imf(&e, &loader);
        /* for simplicity we always parse bodystructure, even though for BODY
           fetches we don't send the BODYSTRUCTURE extension data */
        ie_body_t *body = imf_bodystructure(&e, root_imf);
        f = ie_fetch_resp_body(&e, f, body);
    }

    if(fetch->attr->bodystruct){
        const imf_t *root_imf = loader_parse_imf(&e, &loader);
        ie_body_t *body = imf_bodystructure(&e, root_imf);
        f = ie_fetch_resp_bodystruct(&e, f, body);
    }

    if(fetch->attr->modseq) TRACE_ORIG(&e, E_INTERNAL, "not implemented");

    ie_fetch_extra_t *extra = fetch->attr->extras;
    for( ; extra; extra = extra->next){
        f = build_fetch_resp_extra(&e, f, extra, &loader, part_missing);
    }

    // finally, send the fetch response
    imap_resp_arg_t arg = {.fetch=f};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_FETCH, arg);

    resp = imap_resp_assert_writable(&e, resp, dn->exts);

    loader_close(&e, &loader);

    CHECK(&e);

    PROP(&e, dn->cb->resp(dn->cb, resp) );

    return e;
}


// TODO: support non-PEEK properly
static derr_t fetch_cmd(dn_t *dn, const ie_dstr_t *tag,
        const ie_fetch_cmd_t *fetch){
    derr_t e = E_OK;

    gather_t gather;
    gather_prep(&gather);

    /* determine uids_dn before gathering updates, so that we don't
       accidentally add new messages to our view before we decide which uids
       the client was referring to */
    bool uid_mode = fetch->uid_mode;
    ie_seq_set_t *uids_dn;
    PROP(&e,
        seq_set_to_canonical_uids_dn(dn, uid_mode, fetch->seq_set, &uids_dn)
    );

    // gather updates without sending, since we'll modify them before sending
    bool allow_expunge = fetch->uid_mode;
    PROP_GO(&e,
        gather_updates_dont_send(dn, allow_expunge, NULL, &gather),
    cu);

    // build a response for every uid_dn requested
    ie_seq_set_trav_t trav;
    unsigned int uid_dn = ie_seq_set_iter(&trav, uids_dn, 0, 0);
    bool any_missing = false;
    for(; uid_dn != 0; uid_dn = ie_seq_set_next(&trav)){
        bool force_flags = false;
        // if we gathered an update for uid_dn, combine the FETCH responses
        jsw_anode_t *node = jsw_aerase(&gather.metas, &uid_dn);
        if(node){
            gathered_free(CONTAINER_OF(node, gathered_t, node));
            force_flags = true;
        }
        bool part_missing = false;
        PROP_GO(&e,
            send_fetch_resp(dn, fetch, uid_dn, force_flags, &part_missing),
        cu);
        any_missing |= part_missing;
    }

    // now send the rest of the gathered updates
    bool for_store = false;
    PROP_GO(&e, gather_send_responses(dn, &gather, uid_mode, for_store), cu);

    if(!any_missing){
        DSTR_STATIC(msg, "you didn't hear it from me");
        PROP_GO(&e, send_ok(dn, tag, &msg), cu);
    }else{
        DSTR_STATIC(msg, "some requested subparts were missing");
        PROP_GO(&e, send_no(dn, tag, &msg), cu);
    }

cu:
    ie_seq_set_free(uids_dn);
    gather_free(&gather);

    return e;
}


static derr_t expunge_cmd(dn_t *dn, const ie_dstr_t *tag){
    derr_t e = E_OK;

    msg_key_list_t *keys;
    PROP(&e, get_msg_keys_to_expunge(dn, &keys) );
    // detect noop EXPUNGEs
    if(!keys){
        // true = always allow expunges, false = there is no uid mode
        PROP(&e, dn_gather_updates(dn, true, false, NULL) );
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
        update_req_expunge_new(&e, STEAL(msg_key_list_t, &keys), dn);
    CHECK_GO(&e, fail);
    PROP_GO(&e, imaildir_dn_request_update(dn->m, req), fail);

    return e;

fail:
    dn_free_expunge(dn);
    msg_key_list_free(keys);
    return e;
}


static derr_t copy_cmd(dn_t *dn, const ie_dstr_t *tag,
        const ie_copy_cmd_t *copy){
    derr_t e = E_OK;

    ie_seq_set_t *uids_dn = NULL;
    msg_key_list_t *keys = NULL;

    // get the canonical uids_dn
    PROP(&e,
        seq_set_to_canonical_uids_dn(
            dn, copy->uid_mode, copy->seq_set, &uids_dn
        )
    );

    // detect noop COPYs
    if(!uids_dn){
        // true = always allow expunges
        PROP(&e, dn_gather_updates(dn, true, copy->uid_mode, NULL) );
        PROP(&e, send_ok(dn, tag, &DSTR_LIT("noop COPY command")) );
        goto cu;
    }

    // get the msg_keys
    PROP_GO(&e, uids_dn_to_msg_keys(dn, uids_dn, &keys), cu);

    // reset the dn.copy state
    dn_free_copy(dn);
    dn->copy.tag = ie_dstr_copy(&e, tag);
    dn->copy.state = DN_WAIT_WAITING;
    dn->copy.uid_mode = copy->uid_mode;
    CHECK_GO(&e, cu);

    // request a copy update
    msg_copy_cmd_t *msg_copy = msg_copy_cmd_new(&e,
        STEAL(msg_key_list_t, &keys),
        ie_mailbox_copy(&e, copy->m)
    );
    update_req_t *req = update_req_copy_new(&e, msg_copy, dn);
    CHECK_GO(&e, cu);

    PROP_GO(&e, imaildir_dn_request_update(dn->m, req), cu);

cu:
    ie_seq_set_free(uids_dn);
    msg_key_list_free(keys);

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
                dn_gather_updates(dn,
                    arg->search->uid_mode, arg->search->uid_mode, NULL
                ),
            cu_cmd);
            PROP_GO(&e, search_cmd(dn, tag, arg->search), cu_cmd);
            break;

        case IMAP_CMD_CHECK:
        case IMAP_CMD_NOOP:
            // true = always allow expunges, false = there is no uid mode
            PROP_GO(&e, dn_gather_updates(dn, true, false, NULL), cu_cmd);
            PROP_GO(&e, send_ok(dn, tag, &DSTR_LIT("zzzzz...")), cu_cmd);
            break;

        case IMAP_CMD_STORE:
            // store_cmd has its own handling of updates
            PROP_GO(&e, store_cmd(dn, tag, arg->store), cu_cmd);
            break;

        case IMAP_CMD_FETCH:
            // fetch_cmd has its own handling of updates
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
static derr_t get_msg_keys_to_expunge(dn_t *dn, msg_key_list_t **out){
    derr_t e = E_OK;
    *out = NULL;

    msg_key_list_t *keys = NULL;

    jsw_atrav_t atrav;
    jsw_anode_t *node = jsw_atfirst(&atrav, &dn->views);
    for(; node; node = jsw_atnext(&atrav)){
        msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);
        if(view->flags.deleted){
            keys = msg_key_list_new(&e, view->key, keys);
            CHECK_GO(&e, fail);
        }
    }

    *out = keys;

    return e;

fail:
    msg_key_list_free(keys);
    return e;
}

derr_t dn_disconnect(dn_t *dn, bool expunge){
    derr_t e = E_OK;

    if(expunge){
        /* calculate a UID EXPUNGE command that we would push to the server,
           and do not disconnect until that response comes in */
        msg_key_list_t *keys;
        PROP(&e, get_msg_keys_to_expunge(dn, &keys) );
        if(!keys){
            // nothing to expunge, disconnect immediately
            PROP(&e, dn->cb->disconnected(dn->cb, NULL) );
        }else{
            // request an expunge update first
            update_req_t *req = update_req_expunge_new(&e, keys, dn);
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

static derr_t send_flags_update(
    dn_t *dn, unsigned int seq_num, msg_flags_t flags, const unsigned int *uid
){
    derr_t e = E_OK;

    ie_fflags_t *ff = ie_fflags_new(&e);
    if(flags.answered) ff = ie_fflags_add_simple(&e, ff, IE_FFLAG_ANSWERED);
    if(flags.flagged)  ff = ie_fflags_add_simple(&e, ff, IE_FFLAG_FLAGGED);
    if(flags.seen)     ff = ie_fflags_add_simple(&e, ff, IE_FFLAG_SEEN);
    if(flags.draft)    ff = ie_fflags_add_simple(&e, ff, IE_FFLAG_DRAFT);
    if(flags.deleted)  ff = ie_fflags_add_simple(&e, ff, IE_FFLAG_DELETED);

    // TODO: support modseq here too
    ie_fetch_resp_t *fetch = ie_fetch_resp_new(&e);
    fetch = ie_fetch_resp_seq_num(&e, fetch, seq_num);
    fetch = ie_fetch_resp_flags(&e, fetch, ff);

    // all UID commands must have a UID in their unsolicited FETCH responses
    // (meaning UID FETCH, UID STORE, UID SEARCH, UID COPY
    if(uid != NULL) fetch = ie_fetch_resp_uid(&e, fetch, *uid);

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


// gathering updates

static const void *gathered_jsw_get_uid_dn(const jsw_anode_t *node){
    const gathered_t *gathered = CONTAINER_OF(node, gathered_t, node);
    return (const void*)&gathered->uid_dn;
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

    // find the old view, if there is one
    /* (there might not be if this is an update to an expunge message where we
        have already accepted the expunge) */
    jsw_anode_t *node = jsw_aerase(&dn->views, &view->uid_dn);
    if(!node){
        // just discard this update
        goto cu;
    }


    // remember that this uid_dn is modified
    if(!jsw_afind(&gather->metas, &view->uid_dn, NULL)){
        gathered_t *gathered;
        PROP_GO(&e, gathered_new(&gathered, view->uid_dn), cu);
        jsw_ainsert(&gather->metas, &gathered->node);
    }

    // remove the old view
    msg_view_t *old_view = CONTAINER_OF(node, msg_view_t, node);
    msg_view_free(&old_view);

    // add the new view, if msg was not expunged
    jsw_ainsert(&dn->views, &view->node);
    update->arg.meta = NULL;

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

    // remember that this uid is expunged
    gathered_t *gathered;
    PROP_GO(&e, gathered_new(&gathered, expunge->uid_dn), cu);
    jsw_ainsert(&gather->expunges, &gathered->node);

    // removing the view happens later, so we don't affect the the seq numbers

cu:
    // delay freeing the update until we remove the message from the view
    link_list_append(&gather->updates_to_free, &update->link);
    return e;
}

static derr_t gather_send_responses(
    dn_t *dn, gather_t *gather, bool uid_mode, bool for_store
){
    derr_t e = E_OK;
    jsw_atrav_t trav;
    jsw_anode_t *node;

    // step 1: any EXPUNGEs don't get FETCHes or count for EXISTS
    /* (these EXPUNGEs are not witheld EXPUNGEs, hence it the choice to not
       report FETCH responses) */
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
    if(for_store){
        // STORE commands have very special rules for what they report back
        PROP(&e, send_store_fetch_resps(dn, gather) );
    }else{
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
            const unsigned int *uid = uid_mode ? &gathered->uid_dn : NULL;
            PROP(&e, send_flags_update(dn, seq_num, view->flags, uid) );
        }
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

/* Intermediate step to dn_gather_updates, which allows modifying the gather_t
   before sending updates.  Useful to FETCH commands, which won't want to send
   multiple FETCH responses for any single message at once, or for STORE
   commands which have special sending requirements. */
static derr_t gather_updates_dont_send(
    dn_t *dn,
    bool allow_expunge,
    ie_st_resp_t **st_resp,
    gather_t *gather
){
    derr_t e = E_OK;

    /* updates need to be processed in batches:
         - no meta updates should be passed for newly created messages
         - only one FETCH is allowed per UID
         - EXPUNGEs should come next and be reverse-ordered
         - EXISTS should come last, to make a delete-one-create-another
           situation a little less ambiguous */

    update_t *update, *temp;
    LINK_FOR_EACH_SAFE(update, temp, &dn->pending_updates, update_t, link){
        switch(update->type){
            // ignore everything but the status-type response
            case UPDATE_NEW:
                link_remove(&update->link);
                PROP_GO(&e, gather_update_new(dn, gather, update), cu);
                break;

            case UPDATE_META:
                link_remove(&update->link);
                PROP_GO(&e, gather_update_meta(dn, gather, update), cu);
                break;

            case UPDATE_EXPUNGE:
                if(!allow_expunge) break;
                link_remove(&update->link);
                PROP_GO(&e, gather_update_expunge(dn, gather, update), cu);
                break;

            case UPDATE_SYNC:
                // this must be the point up to which we are emitting messages
                if(!st_resp){
                    ORIG_GO(&e, E_INTERNAL, "got unexpected UPDATE_SYNC",  cu);
                }
                *st_resp = STEAL(ie_st_resp_t, &update->arg.sync);
                link_remove(&update->link);
                update_free(&update);
                goto cu;
        }
    }
    // if we are here, ensure that no UPDATE_SYNC was expected
    if(st_resp != NULL){
        ORIG_GO(&e, E_INTERNAL, "did not find expected UPDATE_SYNC", cu);
    }

cu:
    if(is_error(e)){
        // clean up memory in error cases
        if(st_resp) ie_st_resp_free(STEAL(ie_st_resp_t, st_resp));
        gather_free(gather);
    }

    return e;
}

// send unsolicited untagged responses for external updates to mailbox
derr_t dn_gather_updates(
    dn_t *dn, bool allow_expunge, bool uid_mode, ie_st_resp_t **st_resp
){
    derr_t e = E_OK;

    gather_t gather;
    gather_prep(&gather);

    PROP(&e,
        gather_updates_dont_send(dn, allow_expunge, st_resp, &gather)
    );

    bool for_store = false;
    PROP_GO(&e, gather_send_responses(dn, &gather, uid_mode, for_store), cu);

cu:
    gather_free(&gather);

    if(is_error(e)){
        // clean up memory in error cases
        if(st_resp) ie_st_resp_free(STEAL(ie_st_resp_t, st_resp));
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
        /* We expected an update but none happened.  I'm pretty sure this only
           happens if the thing got deleted; if it was updated and canceled it
           should have appeared as an update still, and since updates are
           serialized in the imaildir_t and we are only processing the updates
           up to our own update, there's no other possibilities.  Note that if
           we deleted it ourselves the imaildir would have intercepted it and
           shown us a fake flag change. */
        LOG_INFO("deleted message (%x) didn't accept flag change...?\n",
                FU(exp_flags->uid_dn));
        return e;
    }


    // we expected this (non)change, do we report it?
    if(!dn->store.silent){
        unsigned int seq_num;
        PROP(&e, index_to_seq_num(index, &seq_num) );

        const unsigned int *uid =
            dn->store.uid_mode ? &exp_flags->uid_dn : NULL;

        PROP(&e, send_flags_update(dn, seq_num, view->flags, uid) );
        return e;
    }

    // got an expected (non)change, but it was a .SILENT command

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

    unsigned int seq_num;
    PROP(&e, index_to_seq_num(index, &seq_num) );

    const unsigned int *uid = dn->store.uid_mode ? &uid_dn : NULL;

    PROP(&e, send_flags_update(dn, seq_num, view->flags, uid) );
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

    unsigned int seq_num;
    PROP(&e, index_to_seq_num(index, &seq_num) );

    const unsigned int *uid = dn->store.uid_mode ? &exp_flags->uid_dn : NULL;

    msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);
    if(!msg_flags_eq(exp_flags->flags, view->flags)){
        // a different update than we expected, always report it
        PROP(&e, send_flags_update(dn, seq_num, view->flags, uid) );
        return e;
    }

    // we expected this change, do we report it?
    if(!dn->store.silent){
        PROP(&e, send_flags_update(dn, seq_num, view->flags, uid) );
        return e;
    }

    // got an expected change, but it was a .SILENT command

    return e;
}

static derr_t send_store_fetch_resps(dn_t *dn, gather_t *gather){
    derr_t e = E_OK;

    /* Send a FETCH response for each update.  Unless there was a .SILENT
       store, in which case we ignore those messages.  Unless there was an
       external update that caused one of those messages to be different than
       expected.

       Walk the exp_flags_t's from the STORE command and the gather->metas
       simlutaneously to figure out how to respond to each message. */

    jsw_anode_t *node;
    jsw_atrav_t etrav;
    exp_flags_t *exp_flags = NULL;
    jsw_atrav_t gtrav;
    gathered_t *gathered = NULL;

    node = jsw_atfirst(&etrav, &dn->store.tree);
    exp_flags = CONTAINER_OF(node, exp_flags_t, node);
    node = jsw_atfirst(&gtrav, &gather->metas);
    gathered = CONTAINER_OF(node, gathered_t, node);

    // quit when we reach the end of both lists
    while(exp_flags || gathered){
        if(gathered){
            // maybe there's no more exp_flags or gathered is behind
            if(!exp_flags || exp_flags->uid_dn > gathered->uid_dn){
                PROP(&e, send_store_resp_noexp(dn, gathered->uid_dn) );
                node = jsw_atnext(&gtrav);
                gathered = CONTAINER_OF(node, gathered_t, node);
                continue;
            }
        }

        if(exp_flags){
            // maybe there's no more gathered or exp_flags is behind
            if(!gathered || gathered->uid_dn > exp_flags->uid_dn){
                PROP(&e, send_store_resp_noupdate(dn, exp_flags) );
                node = jsw_atnext(&etrav);
                exp_flags = CONTAINER_OF(node, exp_flags_t, node);
                continue;
            }
        }

        // otherwise exp_flags->uid_dn and gathered->uid_dn are valid and equal
        PROP(&e, send_store_resp_expupdate(dn, exp_flags) );

        node = jsw_atnext(&etrav);
        exp_flags = CONTAINER_OF(node, exp_flags_t, node);
        node = jsw_atnext(&gtrav);
        gathered = CONTAINER_OF(node, gathered_t, node);
    }

    return e;
}

static derr_t do_work_store(dn_t *dn){
    derr_t e = E_OK;

    gather_t gather;
    gather_prep(&gather);

    ie_st_resp_t *st_resp = NULL;

    // gather updates, including the st_resp from our UPDATE_SYNC
    bool allow_expunge = dn->store.uid_mode;
    PROP_GO(&e,
        gather_updates_dont_send(dn, allow_expunge, &st_resp, &gather),
    cu);

    // send all untagged responses
    bool uid_mode = dn->store.uid_mode;
    bool for_store = true;
    PROP_GO(&e, gather_send_responses(dn, &gather, uid_mode, for_store), cu);

    // send tagged status-type response
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
    ie_st_resp_free(st_resp);
    gather_free(&gather);
    dn_free_store(dn);
    return e;
}


static derr_t do_work_expunge(dn_t *dn){
    derr_t e = E_OK;

    ie_st_resp_t *st_resp = NULL;

    // true = always allow expunges, false = there is no uid mode
    PROP_GO(&e, dn_gather_updates(dn, true, false, &st_resp), cu);

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

    PROP_GO(&e, dn_gather_updates(dn, true, dn->copy.uid_mode, &st_resp), cu);

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

