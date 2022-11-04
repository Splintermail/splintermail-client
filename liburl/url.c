#include "liburl/liburl.h"

bool parse_url_ex(const dstr_t *text, url_t *out, dstr_t *errbuf){
    bool ok = true;
    *out = (url_t){0};

    bool hex = false;
    url_scanner_t s = url_scanner(text);
    URL_ONSTACK_PARSER(p, URL_URI_MAX_CALLSTACK, URL_URI_MAX_SEMSTACK);

    while(true){
        url_scanned_t scanned = url_scanner_next(&s, hex);
        hex = false;
        url_status_e status = url_parse_uri(
            &p, text, &hex, errbuf, scanned.token, scanned.loc, out, NULL
        );
        switch(status){
            case URL_STATUS_OK: continue;
            case URL_STATUS_DONE: goto done;
            case URL_STATUS_SYNTAX_ERROR:
                // message was written in url_handle_error
                ok = false;
                goto done;

            case URL_STATUS_CALLSTACK_OVERFLOW:
                if(errbuf){
                    FMT_QUIET(errbuf, "url_parse_uri() CALLSTACK_OVERFLOW");
                }
                ok = false;
                goto done;

            case URL_STATUS_SEMSTACK_OVERFLOW:
                if(errbuf){
                    FMT_QUIET(errbuf, "url_parse_uri() SEMSTACK_OVERFLOW");
                }
                ok = false;
                goto done;
        }
    }

done:
    url_parser_reset(&p);
    return ok;
}

derr_t parse_url(const dstr_t *text, url_t *out){
    derr_t e = E_OK;

    DSTR_VAR(errbuf, 512);

    bool ok = parse_url_ex(text, out, &errbuf);
    if(!ok){
        ORIG(&e, E_PARAM, "invalid url: %x", FD(&errbuf));
    }

    return e;
}

bool parse_url_reference_ex(const dstr_t *text, url_t *out, dstr_t *errbuf){
    bool ok = true;
    *out = (url_t){0};

    bool hex = false;
    url_scanner_t s = url_scanner(text);
    URL_ONSTACK_PARSER(p,
        URL_URI_REFERENCE_MAX_CALLSTACK,
        URL_URI_REFERENCE_MAX_SEMSTACK
    );

    while(true){
        url_scanned_t scanned = url_scanner_next(&s, hex);
        hex = false;
        url_status_e status = url_parse_uri_reference(
            &p, text, &hex, errbuf, scanned.token, scanned.loc, out, NULL
        );
        switch(status){
            case URL_STATUS_OK: continue;
            case URL_STATUS_DONE: goto done;
            case URL_STATUS_SYNTAX_ERROR:
                // message was written in url_handle_error
                ok = false;
                goto done;

            case URL_STATUS_CALLSTACK_OVERFLOW:
                if(errbuf){
                    FMT_QUIET(errbuf,
                        "url_parse_uri_reference() CALLSTACK_OVERFLOW"
                    );
                }
                ok = false;
                goto done;

            case URL_STATUS_SEMSTACK_OVERFLOW:
                if(errbuf){
                    FMT_QUIET(errbuf,
                        "url_parse_uri_reference() SEMSTACK_OVERFLOW"
                    );
                }
                ok = false;
                goto done;
        }
    }

done:
    url_parser_reset(&p);
    return ok;
}

derr_t parse_url_reference(const dstr_t *text, url_t *out){
    derr_t e = E_OK;

    DSTR_VAR(errbuf, 512);

    bool ok = parse_url_reference_ex(text, out, &errbuf);
    if(!ok){
        ORIG(&e, E_PARAM, "invalid url: %x", FD(&errbuf));
    }

    return e;
}

url_t must_parse_url(const dstr_t *text){
    url_t out;
    DSTR_VAR(errbuf, 512);
    if(!parse_url_ex(text, &out, &errbuf)){
        LOG_FATAL("failed to parse url (%x): %x\n", FD(text), FD(&errbuf));
    }
    return out;
}
