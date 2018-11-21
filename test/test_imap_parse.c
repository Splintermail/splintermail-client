#include <common.h>
#include <logger.h>
#include <imap_scan.h>
#include <imap_parse.h>
#include <imap_parse.tab.h>

#include "test_utils.h"

// the struct for the parse hooks' *data memeber
typedef struct {
    imap_parser_t *parser;
    imap_scanner_t *scanner;
    scan_mode_t scan_mode;
    keep_type_t keep_type;
} locals_t;

#define EXPECT(e, cmd) { \
    error = cmd; \
    CATCH(E_ANY){}; \
    if(error != e){ \
        LOG_ERROR("expected parser to return %x, but got %x\n", \
                  FD(error_to_dstr(e)), \
                  FD(error_to_dstr(error))); \
        ORIG_GO(E_VALUE, "value mismatch", cu_parser); \
    }\
}

static derr_t keep_init(void* data, keep_type_t type){
    // dereference the scanner, so we can read the token
    locals_t *locals = data;
    locals->keep_type = type;
    switch(type){
        case KEEP_ATOM:
            LOG_ERROR("KEEP_ATOM\n");
            break;
        case KEEP_LITERAL:
            LOG_ERROR("KEEP_LITERAL\n");
            break;
        case KEEP_QSTRING:
            LOG_ERROR("KEEP_QSTRING\n");
            break;
        case KEEP_ASTR_ATOM:
            LOG_ERROR("KEEP_ASTR_ATOM\n");
            break;
        case KEEP_TAG:
            LOG_ERROR("KEEP_TAG\n");
            break;
        case KEEP_TEXT:
            LOG_ERROR("KEEP_TEXT\n");
            break;
        case KEEP_NUM:
            LOG_ERROR("KEEP_NUM\n");
            break;
    }
    return E_OK;
}

static derr_t keep(void* data){
    // dereference the scanner, so we can read the token
    locals_t *locals = data;
    dstr_t token = get_token(locals->scanner);
    LOG_ERROR("KEEP: %x\n", FD(&token));
    return E_OK;
}

static imap_token_t keep_ref(void* data){
    (void)data;
    LOG_ERROR("KEEP_REF\n");
    return (imap_token_t){0};
}

static void keep_cancel(void* data){
    (void)data;
    LOG_ERROR("KEEP_CANCEL\n");
}


// TODO: fix this test

// static derr_t test_just_parser(void){
//     derr_t error;
//     imap_parser_t parser;
//     PROP( imap_parser_init(&parser) );
//
//     EXPECT(E_OK, imap_parse(&parser, TAG) );
//     EXPECT(E_OK, imap_parse(&parser, OK) );
//     EXPECT(E_OK, imap_parse(&parser, EOL) );
//
// cu_parser:
//     imap_parser_free(&parser);
//     return error;
// }


static derr_t test_scanner_and_parser(void){
    derr_t error;

    imap_scanner_t scanner;
    PROP( imap_scanner_init(&scanner) );

    // structs for configuring parser
    locals_t locals;
    locals.scanner = &scanner;

    imap_parse_hooks_up_t hooks_up = { keep_init, keep, keep_ref, keep_cancel };

    imap_parser_t parser;
    locals.parser = &parser;
    PROP_GO( imap_parser_init(&parser, hooks_up, &locals), cu_scanner);

    FMT(&scanner.bytes, "taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag "
                        "OK [ALERT] alert text\r\n");

    int token_type;
    bool more;

    while(true){
        // try to scan a token
        scan_mode_t scan_mode = parser.scan_mode;
        LOG_ERROR("---------------------\n"
                  "mode is %x\n",
                  FD(scan_mode_to_dstr(scan_mode)));

        PROP_GO( imap_scan(&scanner, scan_mode, &more, &token_type),
                 cu_parser);
        // we are only parsing the one buffer today
        if(more == true){
            break;
        }

        // print the token
        dstr_t token = get_token(&scanner);
        LOG_ERROR("token is '%x' (%x)\n",
                  FD_DBG(&token), FI(token_type));

        // call parser, which will call context-specific actions
        PROP_GO( imap_parse(&parser, token_type), cu_parser);
    }

cu_parser:
    imap_parser_free(&parser);
cu_scanner:
    imap_scanner_free(&scanner);
    return error;
}


int main(int argc, char** argv){
    derr_t error;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_DEBUG);

    // PROP_GO( test_just_parser(), test_fail);
    PROP_GO( test_scanner_and_parser(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    LOG_ERROR("FAIL\n");
    return 1;
}
