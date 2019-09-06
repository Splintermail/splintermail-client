#ifndef IMAP_EXPRESSION_PRINT_H
#define IMAP_EXPRESSION_PRINT_H

#include "common.h"
#include "imap_expression.h"

const dstr_t *ie_status_to_dstr(ie_status_t s);
const dstr_t *ie_status_attr_to_dstr(ie_status_attr_t sa);
const dstr_t *imap_cmd_type_to_dstr(imap_cmd_type_t type);
const dstr_t *imap_resp_type_to_dstr(imap_resp_type_t type);

// string formatters, will throw error if necessary
derr_t print_literal(dstr_t *out, const dstr_t *val);
derr_t print_qstring(dstr_t *out, const dstr_t *val);
derr_t print_string(dstr_t *out, const dstr_t *val);
derr_t print_astring(dstr_t *out, const dstr_t *val);
derr_t print_atom(dstr_t *out, const dstr_t *val);

// no leading or trailing spaces
derr_t print_ie_flags(dstr_t *out, ie_flags_t *flags);
derr_t print_ie_mflags(dstr_t *out, ie_mflags_t *mflags);
derr_t print_ie_pflags(dstr_t *out, ie_pflags_t *pflags);
derr_t print_ie_fflags(dstr_t *out, ie_fflags_t *fflags);
derr_t print_imap_time(dstr_t *out, imap_time_t time);
derr_t print_ie_seq_set(dstr_t *out, ie_seq_set_t *seq_set);
derr_t print_ie_mailbox(dstr_t *out, ie_mailbox_t *m);
derr_t print_ie_search_key(dstr_t *out, ie_search_key_t *search_key);
derr_t print_ie_fetch_attrs(dstr_t *out, ie_fetch_attrs_t *attr);
derr_t print_ie_st_code(dstr_t *out, ie_st_code_t *code);
derr_t print_atoms(dstr_t *out, ie_dstr_t *list);
derr_t print_nums(dstr_t *out, ie_nums_t *nums);

// full commands

derr_t print_login_cmd(dstr_t *out, ie_login_cmd_t *login);
derr_t print_rename_cmd(dstr_t *out, ie_rename_cmd_t *rename);
derr_t print_list_cmd(dstr_t *out, ie_list_cmd_t *list);
derr_t print_status_cmd(dstr_t *out, ie_status_cmd_t *status);
derr_t print_append_cmd(dstr_t *out, ie_append_cmd_t *append);
derr_t print_search_cmd(dstr_t *out, ie_search_cmd_t *search);
derr_t print_fetch_cmd(dstr_t *out, ie_fetch_cmd_t *fetch);
derr_t print_store_cmd(dstr_t *out, ie_store_cmd_t *store);
derr_t print_copy_cmd(dstr_t *out, ie_copy_cmd_t *copy);

derr_t print_imap_cmd(dstr_t *out, imap_cmd_t *cmd);

// full responses

derr_t print_st_resp(dstr_t *out, ie_st_resp_t *st);
derr_t print_list_resp(dstr_t *out, ie_list_resp_t *list);
derr_t print_status_resp(dstr_t *out, ie_status_resp_t *status);
derr_t print_fetch_resp(dstr_t *out, ie_fetch_resp_t *fetch);

derr_t print_imap_resp(dstr_t *out, imap_resp_t *resp);


#endif // IMAP_EXPRESSION_PRINT_H
