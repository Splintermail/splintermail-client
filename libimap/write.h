/* A re-entrant writer of chunks of IMAP-speak.  It has a minimum chunk size of
   2 bytes.
*/

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

/* builder_api: raise an error if the cmd/resp is not writable, so you don't
   get asynchronous stack traces that are hard to debug */
imap_cmd_t *imap_cmd_assert_writable(derr_t *e, imap_cmd_t *cmd,
        const extensions_t *exts);
imap_resp_t *imap_resp_assert_writable(derr_t *e, imap_resp_t *resp,
        const extensions_t *exts);

typedef struct {
    fmt_i iface;
    const imap_cmd_t *cmd;
} _fmt_icmd_t;

typedef struct {
    fmt_i iface;
    const imap_resp_t *resp;
} _fmt_iresp_t;

derr_type_t _fmt_icmd(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_iresp(const fmt_i *iface, writer_i *out);

// FDICMD assumes all extensions are allowed
// prints just the first 80 characters
#define FICMD(cmd) (&(_fmt_icmd_t){ {_fmt_icmd}, cmd }.iface)

// FDIRESP assumes all extensions are allowed
// prints just the first 80 characters
#define FIRESP(resp) (&(_fmt_iresp_t){ {_fmt_iresp}, resp }.iface)
