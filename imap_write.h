#ifndef IMAP_WRITE
#define IMAP_WRITE

/* A re-entrant writer of chunks of IMAP-speak.  It has a minimum chunk size of
   2 bytes.
*/

#include "imap_expression.h"

// *skip is an input and output, *want is just output.
derr_t imap_cmd_write(const imap_cmd_t *cmd, dstr_t *out, size_t *skip,
        size_t *want);

derr_t imap_resp_write(const imap_resp_t *resp, dstr_t *out, size_t *skip,
        size_t *want);

#endif // IMAP_WRITE
