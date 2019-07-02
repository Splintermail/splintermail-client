#ifndef IMAP_SCAN_H
#define IMAP_SCAN_H

#include "common.h"
#include "imap_parse.h"

#define TRUE FLAG
#define FALSE ATOM

typedef struct {
    dstr_t bytes;
    char bytes_buffer[4096];
    // const char* limit;
    // continued scan start position
    const char* cursor;
    // continued scan backup position
    const char* marker;
    // clean scan start position
    const char* start;
    // previous scanner start position (start of the last token)
    const char* old_start;
    // part of continued scan state
    int state;
    bool accept;
    char yych;
    // for automatic continue handling
    bool continuing;
    // for scanning literals
    bool in_literal;
    size_t literal_len;
} imap_scanner_t;

derr_t imap_scanner_init(imap_scanner_t *scanner);
void imap_scanner_free(imap_scanner_t *scanner);

// utility function, returns substring from *start to the end of the buffer
dstr_t get_scannable(imap_scanner_t *scanner);

/* utility function, returns substring from *old_start to *start.  Only valid
   after imap_scan returns SCAN_STATUS_OK. */
dstr_t get_token(imap_scanner_t *scanner);

// steal bytes from the scan stream, like in the case of a literal
dstr_t steal_bytes(imap_scanner_t *scanner, size_t to_steal);

// *more is set to true if more input is needed, otherwise *type is set
derr_t imap_scan(imap_scanner_t *scanner, scan_mode_t mode, bool *more,
                 dstr_t *token_out, int *type);
/*  throws : E_PARAM (invalid input) */

#endif // IMAP_SCAN_H
