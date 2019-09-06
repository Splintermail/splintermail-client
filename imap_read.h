#ifndef IMAP_READ_H
#define IMAP_READ_H

#include "common.h"
#include "imap_scan.h"
#include "imap_parse.h"

typedef struct {
    imap_parser_t parser;
    imap_scanner_t scanner;
    imap_parser_cb_t cb;
    void *cb_data;
} imap_reader_t;

derr_t imap_reader_init(imap_reader_t *reader, imap_parser_cb_t cb,
        void *cb_data);

void imap_reader_free(imap_reader_t *reader);

derr_t imap_read(imap_reader_t *reader, const dstr_t *input);

#endif // IMAP_READ_H
