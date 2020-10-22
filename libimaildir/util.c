#include "libdstr/libdstr.h"

#include "libimaildir.h"

const void *msg_view_jsw_get_uid_dn(const jsw_anode_t *node){
    const msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);
    return (void*)&view->uid_dn;
}

const void *msg_expunge_jsw_get_uid_up(const jsw_anode_t *node){
    const msg_expunge_t *expunge = CONTAINER_OF(node, msg_expunge_t, node);
    return (void*)&expunge->uid_up;
}

const void *msg_mod_jsw_get_modseq(const jsw_anode_t *node){
    const msg_mod_t *mod = CONTAINER_OF(node, msg_mod_t, node);
    return (void*)&mod->modseq;
}

const void *msg_jsw_get_uid_up(const jsw_anode_t *node){
    const msg_t *msg = CONTAINER_OF(node, msg_t, node);
    return (void*)&msg->uid_up;
}

int jsw_cmp_modseq(const void *a, const void *b){
    const unsigned long *modseqa = a;
    const unsigned long *modseqb = b;
    return JSW_NUM_CMP(*modseqa, *modseqb);
}

int jsw_cmp_uid(const void *a, const void *b){
    const unsigned int *uida = a;
    const unsigned int *uidb = b;
    return JSW_NUM_CMP(*uida, *uidb);
}


derr_t index_to_seq_num(size_t index, unsigned int *seq_num){
    derr_t e = E_OK;
    *seq_num = 0;
    // don't let index + 1 be a non-uint32 value
    if(index >= UINT_MAX){
        ORIG(&e, E_INTERNAL, "index too high");
    }
    *seq_num = (unsigned int)index + 1;
    return e;
}


void imap_cmd_cb_init(derr_t *e, imap_cmd_cb_t *cb, const ie_dstr_t *tag,
        imap_cmd_cb_call_f call, imap_cmd_cb_free_f free){
    *cb = (imap_cmd_cb_t){
        .tag = ie_dstr_copy(e, tag),
        .call = call,
        .free = free,
    };
    link_init(&cb->link);
}

void imap_cmd_cb_free(imap_cmd_cb_t *cb){
    if(!cb) return;
    ie_dstr_free(cb->tag);
    cb->tag = NULL;
}

/* himdodeq_calc_t */

void hmsc_prep(himodseq_calc_t *hmsc, unsigned long starting_val){
    *hmsc = (himodseq_calc_t){
        .now = starting_val,
    };
}

unsigned long hmsc_now(himodseq_calc_t *hmsc){
    return hmsc->now;
}

void hmsc_saw_ok_code(himodseq_calc_t *hmsc, unsigned long val){
    hmsc->from_ok_code = val;
}

void hmsc_saw_fetch(himodseq_calc_t *hmsc, unsigned long val){
    // only keep it if it is higher
    hmsc->from_fetch = MAX(hmsc->from_fetch, val);
}

void hmsc_invalidate_starting_val(himodseq_calc_t *hmsc){
    hmsc->now = 0;
}

// gather whatever we saw since the last tagged response return if it changed
bool hmsc_step(himodseq_calc_t *hmsc){
    unsigned long old = hmsc->now;
    if(hmsc->from_ok_code){
        hmsc->now = hmsc->from_ok_code;
    }else if(hmsc->from_fetch){
        hmsc->now = MAX(hmsc->now, hmsc->from_fetch);
    }
    // flush the values we saw
    hmsc->from_ok_code = 0;
    hmsc->from_fetch = 0;
    // return whether or not it changed
    return old != hmsc->now;
}

/* seq_set_builder_t */

static const void *ssbe_jsw_get(const jsw_anode_t *node){
    return CONTAINER_OF(node, seq_set_builder_elem_t, node);
}
static int ssbe_jsw_cmp_n1(const void *a, const void *b){
    const seq_set_builder_elem_t *ssbea = a;
    const seq_set_builder_elem_t *ssbeb = b;
    return JSW_NUM_CMP(ssbea->n1, ssbeb->n1);
}

void seq_set_builder_prep(seq_set_builder_t *ssb){
    jsw_ainit(ssb, ssbe_jsw_cmp_n1, ssbe_jsw_get);
}

// free all the nodes of the ssb
void seq_set_builder_free(seq_set_builder_t *ssb){
    jsw_anode_t *node;
    while((node = jsw_apop(ssb))){
        seq_set_builder_elem_t *ssbe =
            CONTAINER_OF(node, seq_set_builder_elem_t, node);
        free(ssbe);
    }
}

bool seq_set_builder_isempty(seq_set_builder_t *ssb){
    return ssb->root->count == 0;
}

derr_t seq_set_builder_add_range(seq_set_builder_t *ssb, unsigned int n1,
        unsigned int n2){
    derr_t e = E_OK;

    // for now only accept valid UIDs (no 0, no *)
    // (this simplifies seq_set_builder_extract)
    if(!n1 || !n2){
        ORIG(&e, E_PARAM, "invalid UID in seq_set_builder");
    }

    seq_set_builder_elem_t *ssbe = malloc(sizeof(*ssbe));
    if(!ssbe) ORIG(&e, E_NOMEM, "nomem");
    *ssbe = (seq_set_builder_elem_t){ .n1 = MIN(n1, n2), .n2 = MAX(n1, n2) };

    jsw_ainsert(ssb, &ssbe->node);

    return e;
}

derr_t seq_set_builder_add_val(seq_set_builder_t *ssb, unsigned int n1){
    derr_t e = E_OK;

    PROP(&e, seq_set_builder_add_range(ssb, n1, n1) );

    return e;
}

// del_val() will not work if you ever called add_range()
void seq_set_builder_del_val(seq_set_builder_t *ssb, unsigned int val){
    // erase all nodes with a matching n1
    // (this assumes add_range() was never called)
    jsw_anode_t *node;
    while((node = jsw_aerase(ssb, &val))){
        seq_set_builder_elem_t *ssbe =
            CONTAINER_OF(node, seq_set_builder_elem_t, node);
        free(ssbe);
    }
}

// pop_val() will not work if you ever called add_range()
// returns 0 if there is nothing to pop
unsigned int seq_set_builder_pop_val(seq_set_builder_t *ssb){
    unsigned int out = 0;

    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, ssb);
    if(!node) return out;

    // pop that value from the tree
    jsw_pop_atnext(&trav);

    seq_set_builder_elem_t *ssbe =
        CONTAINER_OF(node, seq_set_builder_elem_t, node);

    out = ssbe->n1;

    free(ssbe);

    return out;
}

// walk the sorted values and build a dense-as-possible sequence set
ie_seq_set_t *seq_set_builder_extract(derr_t *e, seq_set_builder_t *ssb){
    if(is_error(*e)) goto fail;

    ie_seq_set_t *base = NULL;

    // start the traversal
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, ssb);

    // detect the empty-list case
    if(!node) goto done;

    // handle the very first node
    seq_set_builder_elem_t *ssbe =
        CONTAINER_OF(node, seq_set_builder_elem_t, node);
    base = ie_seq_set_new(e, ssbe->n1, ssbe->n2);
    ie_seq_set_t *cur = base;
    CHECK_GO(e, fail);

    // handle all other nodes
    for(node = jsw_atnext(&trav); node != NULL; node = jsw_atnext(&trav)){
        seq_set_builder_elem_t *ssbe =
            CONTAINER_OF(node, seq_set_builder_elem_t, node);
        // sequences touch?
        if(ssbe->n1 <= cur->n2 + 1){
            // yes, extend the current seq_set_t
            cur->n2 = ssbe->n2;
        }else{
            // no, start a new one
            cur->next = ie_seq_set_new(e, ssbe->n1, ssbe->n2);
            CHECK_GO(e, fail_base);
            cur = cur->next;
        }
    }

done:
    // always free the ssb, it's a one-time use item
    seq_set_builder_free(ssb);

    return base;

fail_base:
    // free anything we allocated
    ie_seq_set_free(base);
fail:
    seq_set_builder_free(ssb);
    return NULL;
}
