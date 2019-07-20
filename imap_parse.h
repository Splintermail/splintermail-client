#ifndef IMAP_PARSE_H
#define IMAP_PARSE_H

/* imap_expression.h needs imap_parser_t before we enter imap_scan.h.  This is
   the result of the fact that bison will not let us generate our own tokens,
   so the scanner has to include the bison header, which includes an
   imap_parser_t parameter to the bison-generated parser. */
struct imap_parser_t;
typedef struct imap_parser_t imap_parser_t;

#include "imap_read_types.h"
#include "imap_scan.h"
#include "common.h"

void yyerror(imap_parser_t *parser, char const *s);

derr_t imap_parser_init(imap_parser_t *parser,
                        struct imap_reader_t *reader,
                        imap_hooks_dn_t hooks_dn,
                        imap_hooks_up_t hooks_up,
                        void *hook_data);
void imap_parser_free(imap_parser_t *parser);

derr_t imap_parse(imap_parser_t *parser, int type, const dstr_t *token);

void set_scanner_to_literal_mode(imap_parser_t *parser, size_t len);

/* Hook wrappers.  These are just interfaces between functional code and
   procedural code to make callbacks from the bison code. */

void login_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_dstr_t *user,
        ie_dstr_t *pass);
void select_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m);
void examine_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m);
void create_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m);
void delete_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m);
void rename_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *old,
        ie_mailbox_t *new);
void subscribe_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m);
void unsubscribe_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m);
void list_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m,
        ie_dstr_t *pattern);
void lsub_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m,
        ie_dstr_t *pattern);
void status_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m,
        unsigned char status_attr);
void append_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m,
        ie_flags_t *flags, imap_time_t time, ie_dstr_t *content);
void check_cmd(imap_parser_t *p, ie_dstr_t *tag);
void close_cmd(imap_parser_t *p, ie_dstr_t *tag);
void expunge_cmd(imap_parser_t *p, ie_dstr_t *tag);
void search_cmd(imap_parser_t *p, ie_dstr_t *tag, bool uid_mode,
        ie_dstr_t *charset, ie_search_key_t *search_key);
void fetch_cmd(imap_parser_t *p, ie_dstr_t *tag, bool uid_mode,
        ie_seq_set_t *seq_set, ie_fetch_attrs_t *attr);
void store_cmd(imap_parser_t *p, ie_dstr_t *tag, bool uid_mode,
        ie_seq_set_t *seq_set, int sign, bool silent, ie_flags_t *flags);
void copy_cmd(imap_parser_t *p, ie_dstr_t *tag, bool uid_mode,
        ie_seq_set_t *seq_set, ie_mailbox_t *m);

void status_type_resp(imap_parser_t *p, ie_dstr_t *tag, ie_status_t status,
        ie_st_code_t *code, ie_dstr_t *text);
void capa_resp(imap_parser_t *p, ie_dstr_t *capas);
void list_resp(imap_parser_t *p, ie_mflags_t *mflags, char ch, ie_mailbox_t *m);
void lsub_resp(imap_parser_t *p, ie_mflags_t *mflags, char ch, ie_mailbox_t *m);
void status_resp(imap_parser_t *p, ie_mailbox_t *m, ie_status_attr_resp_t sa);
void flags_resp(imap_parser_t *p, ie_flags_t *f);
void exists_resp(imap_parser_t *p, unsigned int num);
void recent_resp(imap_parser_t *p, unsigned int num);
void expunge_resp(imap_parser_t *p, unsigned int num);
void fetch_resp(imap_parser_t *p, unsigned int num, ie_fetch_resp_t *f);

#endif // IMAP_PARSE_H
