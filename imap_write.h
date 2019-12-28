#ifndef IMAP_WRITE
#define IMAP_WRITE

/* A re-entrant writer of chunks of IMAP-speak.  It has a minimum chunk size of
   2 bytes.
*/

#include "imap_expression.h"
#include "imap_extension.h"

// *skip is an input and output, *want is just output.
derr_t imap_cmd_write(const imap_cmd_t *cmd, dstr_t *out, size_t *skip,
        size_t *want, const extensions_t *exts);

derr_t imap_resp_write(const imap_resp_t *resp, dstr_t *out, size_t *skip,
        size_t *want, const extensions_t *exts);

// wrappers around the same code which expect to write complete objects at once
derr_t imap_cmd_print(const imap_cmd_t *cmd, dstr_t *out,
        const extensions_t *exts);
derr_t imap_resp_print(const imap_resp_t *resp, dstr_t *out,
        const extensions_t *exts);

#endif // IMAP_WRITE
