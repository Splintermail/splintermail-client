#include "imap_read.h"

#include "logger.h"
#include "imap_parse.tab.h"

derr_t imap_read_literal(imap_reader_t *reader, size_t len, bool keep){
    reader->in_literal = true;
    reader->literal_len = len;
    reader->keep_literal = keep;
    if(keep){
        PROP( dstr_new(&reader->literal_temp, len) );
    }
    return E_OK;
}

derr_t imap_read_rfc822_literal(imap_reader_t *reader, size_t len){
    reader->in_literal = true;
    reader->fetch_literal = true;
    reader->literal_len = len;
    return E_OK;
}

derr_t imap_read_append_literal(imap_reader_t *reader, size_t len){
    reader->in_literal = true;
    reader->fetch_literal = true;
    reader->literal_len = len;
    return E_OK;
}

derr_t imap_reader_init(imap_reader_t *reader,
                        imap_parse_hooks_dn_t hooks_dn,
                        imap_parse_hooks_up_t hooks_up,
                        void *hook_data){
    derr_t error;

    PROP( imap_scanner_init(&reader->scanner) );

    PROP_GO( imap_parser_init(&reader->parser, reader, hooks_dn, hooks_up,
                              &hook_data), fail_scanner);

    // init literal-tracking stuff
    reader->in_literal = false;
    reader->keep_literal = false;
    reader->fetch_literal = false;
    reader->literal_len = 0;
    reader->literal_temp = (dstr_t){0};
    reader->literal_len = 0;

    reader->hooks_up = hooks_up;
    reader->hooks_dn = hooks_dn;
    reader->hook_data = hook_data;

    return E_OK;

fail_scanner:
    imap_scanner_free(&reader->scanner);
    return error;
}

void imap_reader_free(imap_reader_t *reader){
    imap_scanner_free(&reader->scanner);
    imap_parser_free(&reader->parser);
    dstr_free(&reader->literal_temp);
}

derr_t imap_read(imap_reader_t *reader, const dstr_t *input){
    //for(size_t i = 0; i < inputs->len; i++){

    // append the input to the scanner's buffer
    PROP( dstr_append(&reader->scanner.bytes, input) );

    int token_type;
    bool more;

    while(true){
        // check if we are in a literal
        if(reader->in_literal){
            dstr_t stolen = steal_bytes(&reader->scanner, reader->literal_len);
            LOG_ERROR("literal bytes: '%x'\n", FD(&stolen));
            // are we building this literal to pass back to the parser?
            if(reader->keep_literal){
                PROP( dstr_append(&reader->literal_temp, &stolen) );
            }
            // are we passing this literal directly to the decrypter?
            else if(reader->fetch_literal){
                LOG_ERROR("fetched: %x\n", FD(&stolen));
            }
            // remember how many bytes we have stolen from the stream
            reader->literal_len -= stolen.len;
            // if we still need more literal, wait for more input
            if(reader->literal_len){
                return E_OK;
            }
            // otherwise, indicate to the parser that we are finished
            PROP( imap_literal(&reader->parser, reader->literal_temp) );
            // reset the values associated with the literal
            reader->in_literal = false;
            reader->keep_literal = false;
            reader->fetch_literal = false;
            reader->literal_len = 0;
            reader->literal_temp = (dstr_t){0};
        }

        // try to scan a token
        scan_mode_t scan_mode = reader->parser.scan_mode;
        LOG_INFO("---------------------\n"
                 "mode is %x\n",
                 FD(scan_mode_to_dstr(scan_mode)));

        dstr_t scannable = get_scannable(&reader->scanner);
        LOG_DEBUG("scannable is: '%x'\n", FD(&scannable));

        PROP( imap_scan(&reader->scanner, scan_mode, &more, &token_type) );
        if(more == true){
            // done with this input buffer
            break;
        }

        // print the token
        dstr_t token = get_token(&reader->scanner);
        LOG_INFO("token is '%x' (%x)\n", FD_DBG(&token), FI(token_type));

        // call parser, which will call context-specific actions
        PROP( imap_parse(&reader->parser, token_type, &token) );
    }
    return E_OK;
}
