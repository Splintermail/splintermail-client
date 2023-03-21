#include "libimap/libimap.h"

derr_t imap_cmd_reader_init(imap_cmd_reader_t *r, extensions_t *exts){
    derr_t e = E_OK;

    *r = (imap_cmd_reader_t){
        .args = {
            .exts = exts,
            .is_client = false,
        }
    };
    r->args.scanner = &r->scanner;

    // responses have a fixed size
    r->p = imap_parser_new(
        IMAP_RESPONSE_LINE_MAX_CALLSTACK,
        IMAP_RESPONSE_LINE_MAX_SEMSTACK
    );
    if(!r->p){
        ORIG(&e, E_NOMEM, "nomem");
    }

    return e;
}

derr_t imap_resp_reader_init(imap_resp_reader_t *r, extensions_t *exts){
    derr_t e = E_OK;

    *r = (imap_resp_reader_t){
        .args = {
            .exts = exts,
            .is_client = true,
        }
    };
    r->args.scanner = &r->scanner;

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

void imap_cmd_reader_free(imap_cmd_reader_t *r){
    imap_parser_free(&r->p);
    ie_dstr_free(STEAL(ie_dstr_t, &r->args.errmsg));
    *r = (imap_cmd_reader_t){0};
}

void imap_resp_reader_free(imap_resp_reader_t *r){
    imap_parser_free(&r->p);
    ie_dstr_free(STEAL(ie_dstr_t, &r->args.errmsg));
    *r = (imap_resp_reader_t){0};
}

static derr_t do_read(
    imap_parser_t *p,
    imap_scanner_t *scanner,
    imap_args_t *args,
    const dstr_t input,
    link_t *out,
    bool starttls,
    size_t *skip,
    imap_status_e (*parse_fn)(
        imap_parser_t *p,
        derr_t* E,
        const dstr_t *dtoken,
        imap_args_t* a,
        imap_token_e token
    )
){
    derr_t e = E_OK;

    if(skip) *skip = SIZE_MAX;
    args->out = out;

    imap_feed(scanner, input);

    while(true){
        // try to scan a token
        scan_mode_t scan_mode = args->scan_mode;
        // LOG_INFO("---------------------\n"
        //          "mode is %x\n",
        //          FD(scan_mode_to_dstr(scan_mode)));

        // DSTR_VAR(scannable, 256);
        // get_scannable(scanner, &scannable);
        // LOG_DEBUG("scannable is: '%x'\n", FD_DBG(&scannable));

        imap_scanned_t s = imap_scan(scanner, scan_mode);
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
        imap_status_e status = parse_fn(p, &e, &s.token, args, s.type);
        PROP_VAR(&e, &e);
        switch(status){
            // if we are halfway through a message, continue
            case IMAP_STATUS_OK: continue;
            // after finishing a message, we stop only in the STARTTLS case
            case IMAP_STATUS_DONE:
                if(!starttls) continue;
                // check if the last cmd was a STARTTLS command
                link_t *link = out->prev;
                imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
                if(cmd->type != IMAP_CMD_STARTTLS) continue;
                if(skip) *skip = get_starttls_skip(scanner);
                return e;

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

static derr_t _imap_cmd_read(
    imap_cmd_reader_t *r,
    const dstr_t input,
    link_t *out,
    bool starttls,
    size_t *skip
){
    derr_t e = E_OK;

    link_t temp = {0};
    link_t *link;

    PROP_GO(&e,
        do_read(
            r->p,
            &r->scanner,
            &r->args,
            input,
            &temp,
            starttls,
            skip,
            imap_parse_command_line
        ),
    fail);

    link_list_append_list(out, &temp);

    return e;

fail:
    while((link = link_list_pop_last(&temp))){
        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
        imap_cmd_free(cmd);
    }
    return e;
}

// populates out with imap_cmd_t's
derr_t imap_cmd_read(
    imap_cmd_reader_t *r, const dstr_t input, link_t *out
){
    derr_t e = E_OK;
    PROP(&e, _imap_cmd_read(r, input, out, false, NULL) );
    return e;
}

// stop at the first STARTTLS and treat the rest as a handshake
derr_t imap_cmd_read_starttls(
    imap_cmd_reader_t *r, const dstr_t input, link_t *out, size_t *skip
){
    derr_t e = E_OK;
    PROP(&e, _imap_cmd_read(r, input, out, true, skip) );
    return e;
}


// populates out with imap_resp_t's
derr_t imap_resp_read(
    imap_resp_reader_t *r, const dstr_t input, link_t *out
){
    derr_t e = E_OK;

    link_t temp = {0};
    link_t *link;

    PROP_GO(&e,
        do_read(
            r->p,
            &r->scanner,
            &r->args,
            input,
            &temp,
            false,
            NULL,
            imap_parse_response_line
        ),
    fail);

    link_list_append_list(out, &temp);

    return e;

fail:
    while((link = link_list_pop_last(&temp))){
        imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);
        imap_resp_free(resp);
    }
    return e;
}
