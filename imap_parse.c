#include "imap_parse.h"
#include "logger.h"

#include <imap_parse.tab.h>

void yyerror(imap_parser_t *parser, char const *s){
    (void)parser;
    printf("ERROR: %s\n", s);
}

derr_t imap_parser_init(imap_parser_t *parser, imap_scanner_t *scanner,
                        imap_parser_cb_t cb, void *cb_data){
    derr_t e = E_OK;

    // init the bison parser
    parser->yyps = yypstate_new();
    if(parser->yyps == NULL){
        ORIG(&e, E_NOMEM, "unable to allocate yypstate");
    }

    // initial state details
    parser->scan_mode = SCAN_MODE_TAG;
    parser->error = E_OK;
    parser->keep = false;
    parser->keep_st_text = false;

    parser->cb = cb;
    parser->cb_data = cb_data;
    parser->scanner = scanner;

    return e;
}

void imap_parser_free(imap_parser_t *parser){
    yypstate_delete(parser->yyps);
}

derr_t imap_parse(imap_parser_t *parser, int type, const dstr_t *token){
    derr_t e = E_OK;
    parser->token = token;
    int yyret = yypush_parse(parser->yyps, type, NULL, parser);
    switch(yyret){
        case 0:
            // YYACCEPT: parsing completed successful; parser is reset
            return e;
        case YYPUSH_MORE:
            // parsing incomplete, but valid; parser not reset
            return e;
        case 1:
            // YYABORT or syntax invalid; parser is reset
            ORIG(&e, E_PARAM, "invalid input");
        case 2:
            // memory exhaustion; parser is reset
            ORIG(&e, E_NOMEM, "memory exhaustion during yypush_parse");
        default:
            // this should never happen
            TRACE(&e, "yypush_parse() returned %x\n", FI(yyret));
            ORIG(&e, E_INTERNAL, "unexpected yypush_parse() return value");
    }
}

void set_scanner_to_literal_mode(imap_parser_t *parser, size_t len){
    parser->scanner->in_literal = true;
    parser->scanner->literal_len = len;
}
