#ifndef IMAP_SCAN_H
#define IMAP_SCAN_H

#include "common.h"
#include "imap_read_types.h"
#include <imap_parse.tab.h>

dstr_t* scan_mode_to_dstr(scan_mode_t mode);

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
