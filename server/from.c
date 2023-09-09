#include "libdstr/libdstr.h"
#include "libimap/libimap.h"


static derr_t from_parse(const dstr_t *msg, ie_addr_t **out){
    derr_t e = E_OK;
    *out = NULL;

    imf_scanner_t s = imf_scanner_prep(msg, 0, NULL, NULL, NULL);

    IMF_ONSTACK_PARSER(p, 100, 100);

    bool should_continue = true;
    imf_token_e token_type;
    do {
        dstr_off_t token;
        PROP_GO(&e, imf_scan(&s, &token, &token_type), cu);
        imf_status_e status = imf_parse_from_field(
            &p, &e, msg, 0, token_type, token, out, NULL
        );
        CHECK_GO(&e, cu);
        switch(status){
            case IMF_STATUS_OK: continue;
            case IMF_STATUS_DONE: should_continue = false; break;
            case IMF_STATUS_SYNTAX_ERROR:
                TRACE(&e, "syntax error parsing from field.\n");
                TRACE(&e, "From: %x\n", FD_DBG(*msg));
                ORIG_GO(&e, E_PARAM, "syntax error", cu);

            case IMF_STATUS_SEMSTACK_OVERFLOW:
                TRACE(&e, "semstack overflow parsing from field.\n");
                TRACE(&e, "From: %x\n", FD_DBG(*msg));
                ORIG_GO(&e, E_NOMEM, "semstack overflow", cu);

            case IMF_STATUS_CALLSTACK_OVERFLOW:
                TRACE(&e, "callstack overflow parsing from field.\n");
                TRACE(&e, "From: %x\n", FD_DBG(*msg));
                ORIG_GO(&e, E_NOMEM, "semstack overflow", cu);
        }
    } while(should_continue && token_type != IMF_EOF);

cu:
    imf_parser_reset(&p);
    if(is_error(e)) ie_addr_free(STEAL(ie_addr_t, out));
    return e;
}


static derr_t parse_stdin(void){
    derr_t e = E_OK;
    dstr_t in = {0};
    dstr_t out = {0};
    ie_addr_t *addr = NULL;

    PROP_GO(&e, dstr_new(&in, 4096), cu);
    while(!feof(stdin)){
        PROP_GO(&e, dstr_fread(stdin, &in, 2048, NULL), cu);
    }
    fclose(stdin);

    PROP_GO(&e, dstr_new(&out, in.len), cu);

    PROP_GO(&e, from_parse(&in, &addr), cu);

    // write the first mailbox to stdout
    PROP_GO(&e,
        FMT(&out, "%x@%x", FD(addr->mailbox->dstr), FD(addr->host->dstr)),
    cu);
    PROP_GO(&e, FFMT(stdout, "%x", FD(out)), cu);


cu:
    ie_addr_free(addr);
    dstr_free(&out);
    dstr_free(&in);
    return e;
}


int main(void){
    derr_t e = E_OK;
    PROP_GO(&e, logger_add_fileptr(LOG_LVL_ERROR, stderr), fail);
    PROP_GO(&e, parse_stdin(), fail);
    return 0;

fail:
    CATCH(&e, E_PARAM){
        // syntax error
        DROP_VAR(&e);
        return 1;
    }
    // any other error
    DUMP(e);
    DROP_VAR(&e);
    return 2;
}
