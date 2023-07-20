#include "libweb/libweb.h"

static void retry_after_handle_error(
    web_parser_t *p,
    const dstr_t *buf,
    bool *hex,
    void *data,
    web_token_e web_token,
    web_sem_t sem,
    const unsigned char *expected_mask,
    const char *loc_summary
){
    (void)p;
    (void)buf;
    (void)hex;
    (void)web_token;
    (void)expected_mask;
    (void)loc_summary;

    dstr_t *errbuf = data;

    // only generate an error if the user gave a buffer for it
    if(errbuf == NULL) return;
    errbuf->len = 0;

    FMT_QUIET(errbuf, "invalid retry-after: ");
    // aim for 80 characters of context
    get_token_context(errbuf, sem.loc, 80);
}

// returns bool ok, errbuf can be NULL if you don't want a rendered error
// recommended errbuf size is 512
bool parse_retry_after_ex(const dstr_t text, time_t *out, dstr_t *errbuf){
    bool ok = true;
    *out = 0;

    bool hex = false;
    web_scanner_t s = web_scanner(&text);
    WEB_ONSTACK_PARSER(p,
        WEB_RETRY_AFTER_MAX_CALLSTACK,
        WEB_RETRY_AFTER_MAX_SEMSTACK
    );

    while(true){
        web_scanned_t scanned = web_scanner_next(&s, hex);
        hex = false;
        web_status_e status = web_parse_retry_after(
            &p,
            &text,
            &hex,
            errbuf,
            scanned.token,
            scanned.loc,
            out,
            NULL,
            retry_after_handle_error
        );
        switch(status){
            case WEB_STATUS_OK: continue;
            case WEB_STATUS_DONE: goto done;
            case WEB_STATUS_SYNTAX_ERROR:
                // message was written in retry_after_handle_error
                ok = false;
                goto done;

            case WEB_STATUS_CALLSTACK_OVERFLOW:
                if(errbuf){
                    FMT_QUIET(
                        errbuf, "web_parse_retry_after() CALLSTACK_OVERFLOW"
                    );
                }
                ok = false;
                goto done;

            case WEB_STATUS_SEMSTACK_OVERFLOW:
                if(errbuf){
                    FMT_QUIET(
                        errbuf, "web_parse_retry_after() SEMSTACK_OVERFLOW"
                    );
                }
                ok = false;
                goto done;
        }
    }

done:
    web_parser_reset(&p);
    return ok;
}

// throws E_PARAM on failed parse
derr_t parse_retry_after(const dstr_t text, time_t *out){
    derr_t e = E_OK;

    DSTR_VAR(errbuf, 512);

    bool ok = parse_retry_after_ex(text, out, &errbuf);
    if(!ok) ORIG(&e, E_PARAM, "\n%x", FD(errbuf));

    return e;
}
