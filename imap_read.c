#include "imap_read.h"
#include "imap_parse.h"
#include "imap_scan.h"

#include "logger.h"
#include "imap_parse.tab.h"

derr_t imap_reader_init(imap_reader_t *reader, extensions_t *exts,
        imap_parser_cb_t cb, void *cb_data){
    derr_t e = E_OK;

    PROP(&e, imap_scanner_init(&reader->scanner) );

    PROP_GO(&e, imap_parser_init(&reader->parser, &reader->scanner, exts, cb,
                cb_data), fail_scanner);

    reader->cb = cb;
    reader->cb_data = cb_data;

    return e;

fail_scanner:
    imap_scanner_free(&reader->scanner);
    return e;
}

void imap_reader_free(imap_reader_t *reader){
    imap_scanner_free(&reader->scanner);
    imap_parser_free(&reader->parser);
}

derr_t imap_read(imap_reader_t *reader, const dstr_t *input){
    derr_t e = E_OK;

    // append the input to the scanner's buffer
    PROP(&e, dstr_append(&reader->scanner.bytes, input) );

    int token_type;
    dstr_t token;
    bool more;

    while(true){
        // try to scan a token
        scan_mode_t scan_mode = reader->parser.scan_mode;
        // LOG_INFO("---------------------\n"
        //          "mode is %x\n",
        //          FD(scan_mode_to_dstr(scan_mode)));

        // dstr_t scannable = get_scannable(&reader->scanner);
        // LOG_DEBUG("scannable is: '%x'\n", FD(&scannable));

        PROP(&e, imap_scan(&reader->scanner, scan_mode, &more, &token,
                    &token_type) );
        if(more == true){
            // done with this input buffer
            break;
        }

        // print the token
        // LOG_INFO("token is '%x' (%x)\n", FD_DBG(&token), FI(token_type));

        // call parser, which will call context-specific actions
        derr_t e2 = imap_parse(&reader->parser, token_type, &token);
        if(e2.type == E_PARAM){
            dstr_t scannable = get_scannable(&reader->scanner);
            TRACE(&e, "syntax error on message:\n%x%x\n(end of message)\n",
                    FD(&token), FD(&scannable));
        }
        PROP(&e, e2);
    }

    imap_scanner_shrink(&reader->scanner);

    return e;
}
