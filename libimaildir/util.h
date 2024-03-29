#ifndef IMAP_UTIL_H
#define IMAP_UTIL_H

/*
A collection of structs and functions to help write imap clients and servers.
*/

#include "libimap/libimap.h"

const void *msg_view_jsw_get_uid_dn(const jsw_anode_t *node);
const void *msg_mod_jsw_get_modseq(const jsw_anode_t *node);
const void *msg_jsw_get_msg_key(const jsw_anode_t *node);
const void *expunge_jsw_get_msg_key(const jsw_anode_t *node);
int jsw_cmp_msg_key(const void *a, const void *b);


// convert an index (such as from jsw_afind()) to a sequence number
derr_t index_to_seq_num(size_t index, unsigned int *seq_num);

typedef enum {
    SUBDIR_CUR = 0,
    SUBDIR_TMP = 1,
    SUBDIR_NEW = 2,
} subdir_type_e;

// Helper functions for automating access to cur/, tmp/, and new/
DSTR_STATIC(subdir_cur_dstr, "cur");
DSTR_STATIC(subdur_tmp_dstr, "tmp");
DSTR_STATIC(subdir_new_dstr, "new");

static inline string_builder_t CUR(const string_builder_t *path){
    return sb_append(path, SBD(subdir_cur_dstr));
}
static inline string_builder_t TMP(const string_builder_t *path){
    return sb_append(path, SBD(subdur_tmp_dstr));
}
static inline string_builder_t NEW(const string_builder_t *path){
    return sb_append(path, SBD(subdir_new_dstr));
}
static inline string_builder_t SUB(
    const string_builder_t *path, subdir_type_e type
){
    switch(type){
        case SUBDIR_CUR: return CUR(path);
        case SUBDIR_NEW: return NEW(path);
        case SUBDIR_TMP:
        default:         return TMP(path);
    }
}

/* imap_cmd_cb_t: a closure for handling responses to imap commands.

   call() may or may not happen, but free() will always happen */
struct imap_cmd_cb_t;
typedef struct imap_cmd_cb_t imap_cmd_cb_t;

typedef derr_t (*imap_cmd_cb_call_f)(imap_cmd_cb_t *cb,
        const ie_st_resp_t *st_resp);

typedef void (*imap_cmd_cb_free_f)(imap_cmd_cb_t *cb);

struct imap_cmd_cb_t {
    // the tag the command was issued with
    ie_dstr_t *tag;
    imap_cmd_cb_call_f call;
    // free() is always called, even if call() was not
    imap_cmd_cb_free_f free;
    link_t link;
};
DEF_CONTAINER_OF(imap_cmd_cb_t, link, link_t)

void imap_cmd_cb_init(derr_t *e, imap_cmd_cb_t *cb, const ie_dstr_t *tag,
        imap_cmd_cb_call_f call, imap_cmd_cb_free_f free);
void imap_cmd_cb_free(imap_cmd_cb_t *cb);

/* seq_set_builder_t: a tool for building as-dense-as-possible sequence sets
   one value (or range of values) at a time (duplicate values are fine) */

typedef jsw_atree_t seq_set_builder_t;

typedef struct {
    unsigned int n1;
    unsigned int n2;
    jsw_anode_t node;
} seq_set_builder_elem_t;
DEF_CONTAINER_OF(seq_set_builder_elem_t, node, jsw_anode_t)

void seq_set_builder_prep(seq_set_builder_t *ssb);

// free all the nodes of the ssb
void seq_set_builder_free(seq_set_builder_t *ssb);

bool seq_set_builder_isempty(seq_set_builder_t *ssb);

derr_t seq_set_builder_add_range(seq_set_builder_t *ssb, unsigned int n1,
        unsigned int n2);

derr_t seq_set_builder_add_val(seq_set_builder_t *ssb, unsigned int n1);

// del_val() will not work if you ever called add_range()
void seq_set_builder_del_val(seq_set_builder_t *ssb, unsigned int val);

// pop_val() will not work if you ever called add_range()
// returns 0 if there is nothing to pop
unsigned int seq_set_builder_pop_val(seq_set_builder_t *ssb);

// walk the sorted values and build a dense-as-possible sequence set
ie_seq_set_t *seq_set_builder_extract(derr_t *e, seq_set_builder_t *ssb);

#endif // IMAP_UTIL_H
