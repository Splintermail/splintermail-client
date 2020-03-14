#ifndef IMAP_PARSE_H
#define IMAP_PARSE_H

#include "expression.h"
#include "extension.h"
#include "scan.h"
#include "libdstr/libdstr.h"

typedef union {
    void (*cmd)(void *cb_data, derr_t error, imap_cmd_t *cmd);
    void (*resp)(void *cb_data, derr_t error, imap_resp_t *resp);
} imap_parser_cb_t;

typedef struct imap_parser_t {
    void *yyps;
    // for callbacks
    imap_parser_cb_t cb;
    void *cb_data;
    // for configuring the literal mode of the scanner
    imap_scanner_t *scanner;
    // for tracking errors returned by hooks
    derr_t error;
    // the mode the scanner should be in while scanning the next token
    scan_mode_t scan_mode;
    // the mode before the start of a qstring
    scan_mode_t preqstr_mode;
    // // for building status_type calls
    // status_type_t status_type;
    // status_code_t status_code;
    // the current token as a dstr_t, used in some cases by the parser
    const dstr_t *token;
    // should we keep the next thing we run across?
    bool keep;
    // should we keep the text at the end of the status-type response?
    bool keep_st_text;
    // imap extensions
    extensions_t *exts;
} imap_parser_t;

void yyerror(imap_parser_t *parser, char const *s);

derr_t imap_parser_init(imap_parser_t *parser, imap_scanner_t *scanner,
                        extensions_t *exts, imap_parser_cb_t cb,
                        void *cb_data);
void imap_parser_free(imap_parser_t *parser);

derr_t imap_parse(imap_parser_t *parser, int type, const dstr_t *token);

void set_scanner_to_literal_mode(imap_parser_t *parser, size_t len);

#endif // IMAP_PARSE_H
