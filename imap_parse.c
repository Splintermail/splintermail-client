#include "imap_parse.h"
#include "logger.h"

#include <imap_parse.tab.h>

void yyerror(imap_parser_t *parser, char const *s){
    (void)parser;
    printf("ERROR: %s\n", s);
}

DSTR_STATIC(scan_mode_tag_dstr, "SCAN_MODE_TAG");
DSTR_STATIC(scan_mode_default_dstr, "SCAN_MODE_DEFAULT");
DSTR_STATIC(scan_mode_astring_dstr, "SCAN_MODE_ASTRING");
DSTR_STATIC(scan_mode_qstring_dstr, "SCAN_MODE_QSTRING");
DSTR_STATIC(scan_mode_num_dstr, "SCAN_MODE_NUM");
DSTR_STATIC(scan_mode_command_dstr, "SCAN_MODE_COMMAND");
DSTR_STATIC(scan_mode_status_code_check_dstr, "SCAN_MODE_STATUS_CODE_CHECK");
DSTR_STATIC(scan_mode_status_code_dstr, "SCAN_MODE_STATUS_CODE");
DSTR_STATIC(scan_mode_status_text_dstr, "SCAN_MODE_STATUS_TEXT");
DSTR_STATIC(scan_mode_unk_dstr, "unknown scan mode");

dstr_t* scan_mode_to_dstr(scan_mode_t mode){
    switch(mode){
        case SCAN_MODE_TAG: return &scan_mode_tag_dstr;
        case SCAN_MODE_DEFAULT: return &scan_mode_default_dstr;
        case SCAN_MODE_ASTRING: return &scan_mode_astring_dstr;
        case SCAN_MODE_QSTRING: return &scan_mode_qstring_dstr;
        case SCAN_MODE_NUM: return &scan_mode_num_dstr;
        case SCAN_MODE_COMMAND: return &scan_mode_command_dstr;
        case SCAN_MODE_STATUS_CODE_CHECK: return &scan_mode_status_code_check_dstr;
        case SCAN_MODE_STATUS_CODE: return &scan_mode_status_code_dstr;
        case SCAN_MODE_STATUS_TEXT: return &scan_mode_status_text_dstr;
        default: return &scan_mode_unk_dstr;
    }
}

derr_t imap_parser_init(imap_parser_t *parser, imap_parse_hooks_up_t hooks_up,
                        void *hook_data){
    // init the bison parser
    parser->yyps = yypstate_new();
    if(parser->yyps == NULL){
        ORIG(E_NOMEM, "unable to allocate yypstate");
    }

    // initial state details
    parser->scan_mode = SCAN_MODE_TAG;
    parser->error = E_OK;

    parser->hooks_up = hooks_up;
    parser->hook_data = hook_data;

    return E_OK;
}

void imap_parser_free(imap_parser_t *parser){
    yypstate_delete(parser->yyps);
}

derr_t imap_parse(imap_parser_t *parser, int token){
    int yyret = yypush_parse(parser->yyps, token, NULL, parser);
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
