#ifndef IMAP_EXPRESSION_PRINT_H
#define IMAP_EXPRESSION_PRINT_H

#include "common.h"
#include "imap_expression.h"

const dstr_t *ie_status_to_dstr(ie_status_t s);
const dstr_t *ie_status_attr_to_dstr(ie_status_attr_t sa);

// string formatters, will throw error if necessary
derr_t print_literal(dstr_t *out, const dstr_t *val);
derr_t print_qstring(dstr_t *out, const dstr_t *val);
derr_t print_string(dstr_t *out, const dstr_t *val);
derr_t print_astring(dstr_t *out, const dstr_t *val);

// no leading or trailing spaces
derr_t print_ie_flags(dstr_t *out, ie_flags_t *flags);
derr_t print_ie_mflags(dstr_t *out, ie_mflags_t *mflags);
derr_t print_ie_pflags(dstr_t *out, ie_pflags_t *pflags);
derr_t print_ie_fflags(dstr_t *out, ie_fflags_t *fflags);
derr_t print_imap_time(dstr_t *out, imap_time_t time);
derr_t print_ie_seq_set(dstr_t *out, ie_seq_set_t *seq_set);
//derr_t print_ie_mailbox(dstr_t *out, ie_mailbox_t *m);
derr_t print_ie_search_key(dstr_t *out, ie_search_key_t *search_key);
derr_t print_ie_fetch_attrs(dstr_t *out, ie_fetch_attrs_t *attr);
derr_t print_ie_st_code(dstr_t *out, ie_st_code_t *code);
derr_t print_ie_dstr_list(dstr_t *out, ie_st_code_t *code);


#endif // IMAP_EXPRESSION_PRINT_H
