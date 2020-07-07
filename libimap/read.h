typedef struct {
    imap_parser_t parser;
    imap_scanner_t scanner;
    extensions_t *exts;
    imap_parser_cb_t cb;
    void *cb_data;
} imap_reader_t;

derr_t imap_reader_init(imap_reader_t *reader, extensions_t *exts,
        imap_parser_cb_t cb, void *cb_data, bool is_client);

void imap_reader_free(imap_reader_t *reader);

derr_t imap_read(imap_reader_t *reader, const dstr_t *input);
