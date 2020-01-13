#ifndef IMAP_UTIL_H
#define IMAP_UTIL_H

/*
A collection of structs and functions to help write imap clients and servers.
*/

#include "imap_expression.h"

/* imap_cmd_cb_t: a closure for handling responses to imap commands.

   call() may or may not happen, but free() will always happen */
struct imap_cmd_cb_t;
typedef struct imap_cmd_cb_t imap_cmd_cb_t;

typedef derr_t (*imap_cmd_cb_call_f)(imap_cmd_cb_t *cb,
        const ie_st_resp_t *st_resp);

typedef void (*imap_cmd_cb_free_f)(imap_cmd_cb_t *cb);

struct imap_cmd_cb_t {
    // the tag the command was issued with
    size_t tag;
    imap_cmd_cb_call_f call;
    imap_cmd_cb_free_f free;
    link_t link;
};
DEF_CONTAINER_OF(imap_cmd_cb_t, link, link_t);

void imap_cmd_cb_prep(imap_cmd_cb_t *cb, size_t tag, imap_cmd_cb_call_f call,
        imap_cmd_cb_free_f free);



/* There's a tricky little state machine for how to accurately calculate the
   HIGHESTMODSEQ value against which you have synchronized.  Following a
   successful SELECT (QRESYNC ...) command (and assuming you are handling all
   FETCH and VANISHED responses properly), then your HIGHESTMODSEQ can be
   calculated during every tagged response as:

     "the value of the HIGHESTMODSEQ untagged OK response since the last
     tagged response, or if there hasn't been one, the highest MODSEQ reported
     since the last tagged response, or if there hasn't been any of those
     either, then just whatever it was last time."

   Of course, if your SELECT (QRESYNC ...) failed due to UIDVALIDITY issues,
   the HIGHESTMODSEQ you see belongs to the server, but has nothing to do with
   your state.

   Therefore, you can update your own HIGHESTMODSEQ in the persistent cache to
   the value produced by this state machine any time you finish a FETCH command
   or successful SELECT (QRESYNC ...), provided you have no new messages to
   download. */

typedef struct {
    // the current value (as of last tagged response)
    unsigned long now;
    // first priority, if nonzero
    unsigned long from_ok_code;
    // second priority, if nonzero
    unsigned long from_fetch;
} himodseq_calc_t;

void hmsc_prep(himodseq_calc_t *hmsc, unsigned long starting_val);

unsigned long hmsc_now(himodseq_calc_t *hmsc);
void hmsc_saw_ok_code(himodseq_calc_t *hmsc, unsigned long val);
void hmsc_saw_fetch(himodseq_calc_t *hmsc, unsigned long val);

// gather whatever we saw since the last tagged response return if it changed
bool hmsc_step(himodseq_calc_t *hmsc);



/* seq_set_builder_t: a tool for building as-dense-as-possible sequence sets
   one value (or range of values) at a time (duplicate values are fine) */

typedef jsw_atree_t seq_set_builder_t;

typedef struct {
    unsigned int n1;
    unsigned int n2;
    jsw_anode_t node;
} seq_set_builder_elem_t;
DEF_CONTAINER_OF(seq_set_builder_elem_t, node, jsw_anode_t);

void seq_set_builder_prep(seq_set_builder_t *ssb);

// free all the nodes of the ssb
void seq_set_builder_free(seq_set_builder_t *ssb);

bool seq_set_builder_isempty(seq_set_builder_t *ssb);

derr_t seq_set_builder_add_range(seq_set_builder_t *ssb, unsigned int n1,
        unsigned int n2);

derr_t seq_set_builder_add_val(seq_set_builder_t *ssb, unsigned int n1);

// walk the sorted values and build a dense-as-possible sequence set
ie_seq_set_t *seq_set_builder_extract(derr_t *e, seq_set_builder_t *ssb);

#endif // IMAP_UTIL_H
