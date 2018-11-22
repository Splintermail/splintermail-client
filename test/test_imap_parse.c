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

static void st_hook(void* data, const dstr_t *tag, status_type_t status,
                    status_code_t code, unsigned int code_extra,
                    const dstr_t *text){
    (void)data;
    (void)status;
    (void)code;
    (void)code_extra;
    LOG_ERROR("status_type response with tag %x and text %x\n",
              FD(tag), FD(text));
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


static derr_t do_test_scanner_and_parser(LIST(dstr_t) *inputs){
    derr_t error;

    // structs for configuring parser
    locals_t locals;

    imap_scanner_t scanner;
    PROP( imap_scanner_init(&scanner) );


    // prepare to init the parser
    imap_parse_hooks_up_t hooks_up = { st_hook };
    imap_parser_t parser;
    PROP_GO( imap_parser_init(&parser, hooks_up, &locals), cu_scanner);

    // store the scanner and the parser in the locals struct
    locals.scanner = &scanner;
    locals.parser = &parser;

    for(size_t i = 0; i < inputs->len; i++){
        // append the input to the scanner's buffer
        PROP_GO( dstr_append(&scanner.bytes, &inputs->data[i]), cu_scanner);

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
            if(more == true){
                // done with this input buffer
                break;
            }

            // print the token
            dstr_t token = get_token(&scanner);
            LOG_ERROR("token is '%x' (%x)\n",
                      FD_DBG(&token), FI(token_type));

            // call parser, which will call context-specific actions
            PROP_GO( imap_parse(&parser, token_type, &token), cu_parser);
        }
    }
cu_parser:
    imap_parser_free(&parser);
cu_scanner:
    imap_scanner_free(&scanner);
    return error;
}


static derr_t test_scanner_and_parser(void){
    {
        LIST_PRESET(dstr_t, inputs,
                DSTR_LIT("taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag "
                         "OK [ALERT bad] alert text\r\n"),
                DSTR_LIT("taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag "
                         "OK [ALERTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTT"
                         "TTTTTTTT] alert text \r\n"));
        PROP( do_test_scanner_and_parser(&inputs) );
    }
    return E_OK;
}


int main(int argc, char **argv){
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
