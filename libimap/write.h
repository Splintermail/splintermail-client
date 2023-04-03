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


// FDICMD assumes all extensions are allowed
// prints just the first 80 characters
derr_type_t fmthook_imap_cmd(dstr_t* out, const void* arg);
static inline fmt_t FICMD(const imap_cmd_t *arg){
    return (fmt_t){FMT_EXT, {.ext = {.arg = (const void*)arg,
                                     .hook = fmthook_imap_cmd} } };
}

// FDIRESP assumes all extensions are allowed
// prints just the first 80 characters
derr_type_t fmthook_imap_resp(dstr_t* out, const void* arg);
static inline fmt_t FIRESP(const imap_resp_t *arg){
    return (fmt_t){FMT_EXT, {.ext = {.arg = (const void*)arg,
                                     .hook = fmthook_imap_resp} } };
}
