#include "imap_parse.h"
#include "logger.h"

#include <imap_parse.tab.h>

void yyerror(imap_parser_t *parser, char const *s){
    (void)parser;
    printf("ERROR: %s\n", s);
}

DSTR_STATIC(scan_mode_TAG_dstr, "SCAN_MODE_TAG");
DSTR_STATIC(scan_mode_QSTRING_dstr, "SCAN_MODE_QSTRING");
DSTR_STATIC(scan_mode_NUM_dstr, "SCAN_MODE_NUM");
DSTR_STATIC(scan_mode_COMMAND_dstr, "SCAN_MODE_COMMAND");
DSTR_STATIC(scan_mode_ATOM_dstr, "SCAN_MODE_ATOM");
DSTR_STATIC(scan_mode_FLAG_dstr, "SCAN_MODE_FLAG");
DSTR_STATIC(scan_mode_STATUS_CODE_CHECK_dstr, "SCAN_MODE_STATUS_CODE_CHECK");
DSTR_STATIC(scan_mode_STATUS_CODE_dstr, "SCAN_MODE_STATUS_CODE");
DSTR_STATIC(scan_mode_STATUS_TEXT_dstr, "SCAN_MODE_STATUS_TEXT");
DSTR_STATIC(scan_mode_MAILBOX_dstr, "SCAN_MODE_MAILBOX");
DSTR_STATIC(scan_mode_ASTRING_dstr, "SCAN_MODE_ASTRING");
DSTR_STATIC(scan_mode_NQCHAR_dstr, "SCAN_MODE_NQCHAR");
DSTR_STATIC(scan_mode_NSTRING_dstr, "SCAN_MODE_NSTRING");
DSTR_STATIC(scan_mode_ST_ATTR_dstr, "SCAN_MODE_ST_ATTR");
DSTR_STATIC(scan_mode_MSG_ATTR_dstr, "SCAN_MODE_MSG_ATTR");
DSTR_STATIC(scan_mode_INTDATE_dstr, "SCAN_MODE_INTDATE");
DSTR_STATIC(scan_mode_WILDCARD_dstr, "SCAN_MODE_WILDCARD");
DSTR_STATIC(scan_mode_SEQSET_dstr, "SCAN_MODE_SEQSET");
DSTR_STATIC(scan_mode_STORE_dstr, "SCAN_MODE_STORE");
DSTR_STATIC(scan_mode_unk_dstr, "unknown scan mode");

dstr_t* scan_mode_to_dstr(scan_mode_t mode){
    switch(mode){
        case SCAN_MODE_TAG: return &scan_mode_TAG_dstr;
        case SCAN_MODE_QSTRING: return &scan_mode_QSTRING_dstr;
        case SCAN_MODE_NUM: return &scan_mode_NUM_dstr;
        case SCAN_MODE_COMMAND: return &scan_mode_COMMAND_dstr;
        case SCAN_MODE_ATOM: return &scan_mode_ATOM_dstr;
        case SCAN_MODE_FLAG: return &scan_mode_FLAG_dstr;
        case SCAN_MODE_STATUS_CODE_CHECK: return &scan_mode_STATUS_CODE_CHECK_dstr;
        case SCAN_MODE_STATUS_CODE: return &scan_mode_STATUS_CODE_dstr;
        case SCAN_MODE_STATUS_TEXT: return &scan_mode_STATUS_TEXT_dstr;
        case SCAN_MODE_MAILBOX: return &scan_mode_MAILBOX_dstr;
        case SCAN_MODE_ASTRING: return &scan_mode_ASTRING_dstr;
        case SCAN_MODE_NQCHAR: return &scan_mode_NQCHAR_dstr;
        case SCAN_MODE_NSTRING: return &scan_mode_NSTRING_dstr;
        case SCAN_MODE_ST_ATTR: return &scan_mode_ST_ATTR_dstr;
        case SCAN_MODE_MSG_ATTR: return &scan_mode_MSG_ATTR_dstr;
        case SCAN_MODE_INTDATE: return &scan_mode_INTDATE_dstr;
        case SCAN_MODE_WILDCARD: return &scan_mode_WILDCARD_dstr;
        case SCAN_MODE_SEQSET: return &scan_mode_SEQSET_dstr;
        case SCAN_MODE_STORE: return &scan_mode_STORE_dstr;
        default: return &scan_mode_unk_dstr;
    }
}

derr_t imap_parser_init(imap_parser_t *parser, imap_parse_hooks_dn_t hooks_dn,
                        imap_parse_hooks_up_t hooks_up,
                        void *hook_data){
    // init dstr_t temp to zeros
    parser->temp = (dstr_t){0};

    // init the bison parser
    parser->yyps = yypstate_new();
    if(parser->yyps == NULL){
        ORIG(E_NOMEM, "unable to allocate yypstate");
    }

    // initial state details
    parser->scan_mode = SCAN_MODE_TAG;
    parser->error = E_OK;
    parser->keep = false;
    parser->keep_init = false;
    parser->keep_st_text = false;

    parser->hooks_dn = hooks_dn;
    parser->hooks_up = hooks_up;
    parser->hook_data = hook_data;

    return E_OK;
}

void imap_parser_free(imap_parser_t *parser){
    yypstate_delete(parser->yyps);
}

derr_t imap_parse(imap_parser_t *parser, int type, const dstr_t *token){
    parser->token = token;
    int yyret = yypush_parse(parser->yyps, type, NULL, parser);
    switch(yyret){
        case 0:             // parsing completed successful; parser is reset
            return E_OK;
        case YYPUSH_MORE:   // parsing incomplete, but valid; parser not reset
            return E_OK;
        case 1:             // invalid; parser is reset
            ORIG(E_PARAM, "invalid input");
        case 2:             // memory exhaustion; parser is reset
            ORIG(E_NOMEM, "memory exhaustion during yypush_parse");
        default:            // this should never happen
            LOG_ERROR("yypush_parse() returned %x\n", FI(yyret));
            ORIG(E_INTERNAL, "unexpected yypush_parse() return value");
    }
}

derr_t keep_init(void *data){
    // dereference the scanner, so we can read the token
    imap_parser_t *parser = data;
    // allocate the temp dstr_t
    PROP( dstr_new(&parser->temp, 64) );
    return E_OK;
}

derr_t keep(imap_parser_t *parser, keep_type_t type){
    // patterns for recoding the quoted strings
    LIST_STATIC(dstr_t, find, DSTR_LIT("\\\\"), DSTR_LIT("\\\""));
    LIST_STATIC(dstr_t, repl, DSTR_LIT("\\"),   DSTR_LIT("\""));
    switch(type){
        case KEEP_RAW:
            // no escapes or fancy shit necessary, just append
            PROP( dstr_append(&parser->temp, parser->token) );
            break;
        case KEEP_QSTRING:
            // unescape \" and \\ sequences
            PROP( dstr_recode(parser->token, &parser->temp, &find, &repl, true) );
            break;
    }
    return E_OK;
}

dstr_t keep_ref(imap_parser_t *parser){
    // just pass the token we built to the parser
    dstr_t retval = parser->temp;
    // never allow a double free, nor pass the same dstr twice
    parser->temp = (dstr_t){0};
    return retval;
}

derr_t imap_literal(imap_parser_t *parser, dstr_t literal){
    // store the literal in the *parser object, so keep_ref() can use it
    // note: if keep was false in the literal hook, this will be a (dstr_t){0}
    parser->temp = literal;
    if(parser->keep){
        /* we may need to set a variable that is associated with the KEEP_INIT
           macro in the bison scanner, so KEEP_REF works correctly */
        parser->keep_init = true;
    }
    // feed LITERAL_END token to the parser, to trigger end-of-literal actions
    dstr_t empty_token = (dstr_t){0};
    PROP( imap_parse(parser, LITERAL_END, &empty_token) );
    return E_OK;
}
