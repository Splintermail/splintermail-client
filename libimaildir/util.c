#include <stdlib.h>

#include "libdstr/libdstr.h"

#include "libimaildir.h"

const void *msg_view_jsw_get_uid_dn(const jsw_anode_t *node){
    const msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);
    return (const void*)&view->uid_dn;
}

const void *msg_mod_jsw_get_modseq(const jsw_anode_t *node){
    const msg_mod_t *mod = CONTAINER_OF(node, msg_mod_t, node);
    return (const void*)&mod->modseq;
}

const void *msg_jsw_get_msg_key(const jsw_anode_t *node){
    const msg_t *msg = CONTAINER_OF(node, msg_t, node);
    return (const void*)&msg->key;
}

const void *expunge_jsw_get_msg_key(const jsw_anode_t *node){
    const msg_expunge_t *expunge = CONTAINER_OF(node, msg_expunge_t, node);
    return (const void*)&expunge->key;
}

int jsw_cmp_msg_key(const void *a, const void *b){
    // each msg_t has either a uid_local or a uid_up; the other will be zero
    // we just sort blindly by one, then by the other if the first is zero
    const msg_key_t *ka = a;
    const msg_key_t *kb = b;
    // (imaildir_process_status_resp depends on sorting by uid_up first)
    int result = jsw_cmp_uint(&ka->uid_up, &kb->uid_up);
    if(result != 0) return result;
    return jsw_cmp_uint(&ka->uid_local, &kb->uid_local);
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
}

void imap_cmd_cb_free(imap_cmd_cb_t *cb){
    if(!cb) return;
    ie_dstr_free(cb->tag);
    cb->tag = NULL;
}

/* seq_set_builder_t */

static const void *ssbe_jsw_get_n1(const jsw_anode_t *node){
    const seq_set_builder_elem_t *ssbe =
        CONTAINER_OF(node, seq_set_builder_elem_t, node);
    return &ssbe->n1;
}

void seq_set_builder_prep(seq_set_builder_t *ssb){
    jsw_ainit(ssb, jsw_cmp_uint, ssbe_jsw_get_n1);
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
