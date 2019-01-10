#ifndef IMAP_READ_H
#define IMAP_READ_H

#include "common.h"
#include "imap_parse.h"
#include "imap_scan.h"

typedef struct imap_reader_t {
    imap_parser_t parser;
    imap_scanner_t scanner;
    scan_mode_t scan_mode;
    bool in_literal;
    bool keep_literal;
    bool fetch_literal;
    size_t literal_len;
    dstr_t literal_temp;
    imap_parse_hooks_dn_t hooks_dn;
    imap_parse_hooks_up_t hooks_up;
    void *hook_data;
} imap_reader_t;

derr_t imap_reader_init(imap_reader_t *reader,
                        imap_parse_hooks_dn_t hooks_dn,
                        imap_parse_hooks_up_t hooks_up,
                        void *hook_data);

void imap_reader_free(imap_reader_t *reader);

derr_t imap_read(imap_reader_t *reader, const dstr_t *input);

// for handling literals, these are hooks called by the parser
derr_t imap_read_literal(imap_reader_t *reader, size_t len, bool keep);
derr_t imap_read_rfc822_literal(imap_reader_t *reader, size_t len);
derr_t imap_read_append_literal(imap_reader_t *reader, size_t len);

#endif // IMAP_READ_H
