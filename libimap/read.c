#include "libimap/libimap.h"

derr_t imap_reader_init(
    imap_reader_t *r,
    extensions_t *exts,
    imap_cb cb,
    void *cb_data,
    bool is_client
){
    derr_t e = E_OK;

    *r = (imap_reader_t){
        .args = {
            .cb = cb,
            .cb_data = cb_data,
            .exts = exts,
            .is_client = is_client,
        }
    };

    // responses have a fixed size, but not commands; search keys are recursive
    r->p = imap_parser_new(
        2*IMAP_RESPONSE_LINE_MAX_CALLSTACK,
        2*IMAP_RESPONSE_LINE_MAX_SEMSTACK
    );
    if(!r->p){
        ORIG(&e, E_NOMEM, "nomem");
    }

    return e;
}

void imap_reader_free(imap_reader_t *r){
    imap_parser_free(&r->p);
    ie_dstr_free(STEAL(ie_dstr_t, &r->args.errmsg));
    *r = (imap_reader_t){0};
}

derr_t imap_read(imap_reader_t *r, const dstr_t *input){
    derr_t e = E_OK;

    imap_feed(&r->scanner, *input);

    imap_status_e (*parse_fn)(
        imap_parser_t *p,
        derr_t* E,
        const dstr_t *dtoken,
        imap_args_t* a,
        imap_token_e token
    ) = r->args.is_client ? imap_parse_response_line : imap_parse_command_line;

    while(true){
        // try to scan a token
        scan_mode_t scan_mode = r->args.scan_mode;
        // LOG_INFO("---------------------\n"
        //          "mode is %x\n",
        //          FD(scan_mode_to_dstr(scan_mode)));

        // DSTR_VAR(scannable, 256);
        // get_scannable(&r->scanner, &scannable);
        // LOG_DEBUG("scannable is: '%x'\n", FD_DBG(&scannable));

        imap_scanned_t s = imap_scan(&r->scanner, scan_mode);
        if(s.more){
            // done with this input buffer
            break;
        }

        // LOG_INFO(
        //     "token is '%x' (%x)\n",
        //     FD_DBG(&s.token),
        //     FS(imap_token_name(s.type))
        // );

        // parse the token
        imap_status_e status = parse_fn(r->p, &e, &s.token, &r->args, s.type);
        PROP_VAR(&e, &e);
        switch(status){
            // if we are halfway through a message, continue
            case IMAP_STATUS_OK: continue;
            // heck, even if we finish a message, continue
            case IMAP_STATUS_DONE: continue;

            case IMAP_STATUS_SYNTAX_ERROR:
                // the imap grammar should prevent this
                ORIG(&e, E_INTERNAL, "imap got syntax error");

            case IMAP_STATUS_SEMSTACK_OVERFLOW:
                ORIG(&e, E_FIXEDSIZE, "semstack overflow parsing imap");

            case IMAP_STATUS_CALLSTACK_OVERFLOW:
                ORIG(&e, E_FIXEDSIZE, "callstack overflow parsing imap");
        }
    }

    return e;
}

void set_scanner_to_literal_mode(imap_args_t *a, size_t len){
    imap_reader_t *r = CONTAINER_OF(a, imap_reader_t, args);
    r->scanner.literal_len = len;
}
