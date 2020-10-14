#include "libimap.h"

#include <imap_parse.tab.h>

void imapyyerror(imap_parser_t *parser, char const *s){
    if(parser->freeing) return;
    PFMT("ERROR: %x: %x\n", FS(s), FD(parser->token));
}

derr_t imap_parser_init(
    imap_parser_t *parser,
    imap_scanner_t *scanner,
    extensions_t *exts,
    imap_parser_cb_t cb,
    void *cb_data,
    bool is_client
){
    derr_t e = E_OK;

    *parser = (imap_parser_t){
        .cb = cb,
        .cb_data = cb_data,
        .is_client = is_client,
        .scanner = scanner,
        .exts = exts,

        .error = E_OK,
        .keep = false,
        .keep_st_text = false,
        .scan_mode = SCAN_MODE_TAG,
    };

    // init the bison parser
    parser->imapyyps = imapyypstate_new();
    if(parser->imapyyps == NULL){
        ORIG(&e, E_NOMEM, "unable to allocate imapyypstate");
    }

    return e;
}

static void free_cmd_cb(void *cb_data, derr_t error, imap_cmd_t *cmd){
    (void)cb_data;
    DROP_VAR(&error);
    imap_cmd_free(cmd);
}

static void free_resp_cb(void *cb_data, derr_t error, imap_resp_t *resp){
    (void)cb_data;
    DROP_VAR(&error);
    imap_resp_free(resp);
}

static imap_parser_cb_t free_cbs = {
    .cmd = free_cmd_cb,
    .resp = free_resp_cb,
};

void imap_parser_free(imap_parser_t *parser){
    /* calling yypstate_delete while the parse is in the middle of a rule
       results in a memory leak, so we pass an invalid token to ensure the
       parser always ends in a finished state.

       This is pretty weird that we need to do more allocations in order to
       not leak memory... but I believe that's the case.  If we are already
       under memory exhaustion, then these calls should trigger bison to call
       any necessary destructors, if we can rely on bison at all. */
    parser->freeing = true;
    parser->cb = free_cbs;
    derr_t e = E_OK;
    DSTR_STATIC(token, "");
    PROP_GO(&e, imap_parse(parser, INVALID_TOKEN, &token), done);
    PROP_GO(&e, imap_parse(parser, EOL, &token), done);
done:
    DROP_VAR(&e);
    imapyypstate_delete(parser->imapyyps);
    DROP_VAR(&parser->error);
}

derr_t imap_parse(imap_parser_t *parser, int type, const dstr_t *token){
    derr_t e = E_OK;
    parser->token = token;
    int yyret = imapyypush_parse(parser->imapyyps, type, NULL, parser);
    switch(yyret){
        case 0:
            // YYACCEPT: parsing completed successfully; parser is reset
            break;
        case YYPUSH_MORE:
            // parsing incomplete, but valid; parser not reset
            break;
        case 1:
            // YYABORT or syntax invalid; parser is reset
            ORIG(&e, E_PARAM, "invalid input");
        case 2:
            // memory exhaustion; parser is reset
            ORIG(&e, E_NOMEM, "memory exhaustion during imapyypush_parse");
        default:
            // this should never happen
            TRACE(&e, "imapyypush_parse() returned %x\n", FI(yyret));
            ORIG(&e, E_INTERNAL, "unexpected imapyypush_parse() return value");
    }
    PROP_VAR(&e, &parser->error);
    return e;
}

void set_scanner_to_literal_mode(imap_parser_t *parser, size_t len){
    parser->scanner->in_literal = true;
    parser->scanner->literal_len = len;
}
