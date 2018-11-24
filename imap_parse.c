#include "imap_parse.h"
#include "logger.h"

#include <imap_parse.tab.h>

void yyerror(imap_parser_t *parser, char const *s){
    (void)parser;
    printf("ERROR: %s\n", s);
}

DSTR_STATIC(scan_mode_tag_dstr, "SCAN_MODE_TAG");
DSTR_STATIC(scan_mode_default_dstr, "SCAN_MODE_DEFAULT");
DSTR_STATIC(scan_mode_qstring_dstr, "SCAN_MODE_QSTRING");
DSTR_STATIC(scan_mode_num_dstr, "SCAN_MODE_NUM");
DSTR_STATIC(scan_mode_command_dstr, "SCAN_MODE_COMMAND");
DSTR_STATIC(scan_mode_atom_dstr, "SCAN_MODE_ATOM");
DSTR_STATIC(scan_mode_flag_dstr, "SCAN_MODE_FLAG");
DSTR_STATIC(scan_mode_status_code_check_dstr, "SCAN_MODE_STATUS_CODE_CHECK");
DSTR_STATIC(scan_mode_status_code_dstr, "SCAN_MODE_STATUS_CODE");
DSTR_STATIC(scan_mode_status_text_dstr, "SCAN_MODE_STATUS_TEXT");
DSTR_STATIC(scan_mode_mailbox_dstr, "SCAN_MODE_MAILBOX");
DSTR_STATIC(scan_mode_nqchar_dstr, "SCAN_MODE_NQCHAR");
DSTR_STATIC(scan_mode_unk_dstr, "unknown scan mode");

dstr_t* scan_mode_to_dstr(scan_mode_t mode){
    switch(mode){
        case SCAN_MODE_TAG: return &scan_mode_tag_dstr;
        case SCAN_MODE_DEFAULT: return &scan_mode_default_dstr;
        case SCAN_MODE_QSTRING: return &scan_mode_qstring_dstr;
        case SCAN_MODE_NUM: return &scan_mode_num_dstr;
        case SCAN_MODE_COMMAND: return &scan_mode_command_dstr;
        case SCAN_MODE_ATOM: return &scan_mode_atom_dstr;
        case SCAN_MODE_FLAG: return &scan_mode_flag_dstr;
        case SCAN_MODE_STATUS_CODE_CHECK: return &scan_mode_status_code_check_dstr;
        case SCAN_MODE_STATUS_CODE: return &scan_mode_status_code_dstr;
        case SCAN_MODE_STATUS_TEXT: return &scan_mode_status_text_dstr;
        case SCAN_MODE_MAILBOX: return &scan_mode_mailbox_dstr;
        case SCAN_MODE_NQCHAR: return &scan_mode_nqchar_dstr;
        default: return &scan_mode_unk_dstr;
    }
}

derr_t imap_parser_init(imap_parser_t *parser, imap_parse_hooks_up_t hooks_up,
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
        case KEEP_ATOM:
        case KEEP_ASTR_ATOM:
        case KEEP_TAG:
        case KEEP_TEXT:
            // no escapes or fancy shit necessary, just append
            PROP( dstr_append(&parser->temp, parser->token) );
            break;
        case KEEP_QSTRING:
            // unescape \" and \\ sequences
            PROP( dstr_recode(parser->token, &parser->temp, &find, &repl, true) );
            break;
        case KEEP_LITERAL:
            ORIG(E_INTERNAL, "KEEP should not be called for a LITERAL");
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
