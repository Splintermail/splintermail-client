#include "libweb/libweb.h"

#include <string.h>

// recorded error message is based on sem.loc and buf, and is written to errbuf
static void url_handle_error(
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

    FMT_QUIET(errbuf, "invalid url: ");
    // aim for 80 characters of context
    get_token_context(errbuf, sem.loc, 80);
}

bool parse_url_ex(const dstr_t *text, url_t *out, dstr_t *errbuf){
    bool ok = true;
    *out = (url_t){0};

    bool hex = false;
    web_scanner_t s = web_scanner(text);
    WEB_ONSTACK_PARSER(p, WEB_URI_MAX_CALLSTACK, WEB_URI_MAX_SEMSTACK);

    while(true){
        web_scanned_t scanned = web_scanner_next(&s, hex);
        hex = false;
        web_status_e status = web_parse_uri(
            &p,
            text,
            &hex,
            errbuf,
            scanned.token,
            scanned.loc,
            out,
            NULL,
            url_handle_error
        );
        switch(status){
            case WEB_STATUS_OK: continue;
            case WEB_STATUS_DONE: goto done;
            case WEB_STATUS_SYNTAX_ERROR:
                // message was written in url_handle_error
                ok = false;
                goto done;

            case WEB_STATUS_CALLSTACK_OVERFLOW:
                if(errbuf){
                    FMT_QUIET(errbuf, "web_parse_uri() CALLSTACK_OVERFLOW");
                }
                ok = false;
                goto done;

            case WEB_STATUS_SEMSTACK_OVERFLOW:
                if(errbuf){
                    FMT_QUIET(errbuf, "web_parse_uri() SEMSTACK_OVERFLOW");
                }
                ok = false;
                goto done;
        }
    }

done:
    web_parser_reset(&p);
    return ok;
}

derr_t parse_url(const dstr_t *text, url_t *out){
    derr_t e = E_OK;

    DSTR_VAR(errbuf, 512);

    bool ok = parse_url_ex(text, out, &errbuf);
    if(!ok){
        ORIG(&e, E_PARAM, "\n%x", FD(errbuf));
    }

    return e;
}

bool parse_url_reference_ex(const dstr_t *text, url_t *out, dstr_t *errbuf){
    bool ok = true;
    *out = (url_t){0};

    bool hex = false;
    web_scanner_t s = web_scanner(text);
    WEB_ONSTACK_PARSER(p,
        WEB_URI_REFERENCE_MAX_CALLSTACK,
        WEB_URI_REFERENCE_MAX_SEMSTACK
    );

    while(true){
        web_scanned_t scanned = web_scanner_next(&s, hex);
        hex = false;
        web_status_e status = web_parse_uri_reference(
            &p,
            text,
            &hex,
            errbuf,
            scanned.token,
            scanned.loc,
            out,
            NULL,
            url_handle_error
        );
        switch(status){
            case WEB_STATUS_OK: continue;
            case WEB_STATUS_DONE: goto done;
            case WEB_STATUS_SYNTAX_ERROR:
                // message was written in url_handle_error
                ok = false;
                goto done;

            case WEB_STATUS_CALLSTACK_OVERFLOW:
                if(errbuf){
                    FMT_QUIET(errbuf,
                        "web_parse_uri_reference() CALLSTACK_OVERFLOW"
                    );
                }
                ok = false;
                goto done;

            case WEB_STATUS_SEMSTACK_OVERFLOW:
                if(errbuf){
                    FMT_QUIET(errbuf,
                        "web_parse_uri_reference() SEMSTACK_OVERFLOW"
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

derr_t parse_url_reference(const dstr_t *text, url_t *out){
    derr_t e = E_OK;

    DSTR_VAR(errbuf, 512);

    bool ok = parse_url_reference_ex(text, out, &errbuf);
    if(!ok){
        ORIG(&e, E_PARAM, "invalid url: %x", FD(errbuf));
    }

    return e;
}

url_t must_parse_url(const dstr_t *text){
    url_t out;
    DSTR_VAR(errbuf, 512);
    if(!parse_url_ex(text, &out, &errbuf)){
        LOG_FATAL("failed to parse url (%x): %x\n", FD(*text), FD(errbuf));
    }
    return out;
}

// get the string behind the url
dstr_t url_text(url_t url){
    return dstr_from_off(dstr_off_extend(url.scheme, url.fragment));
}

//

bool parse_addrspec_ex(const dstr_t *text, addrspec_t *out, dstr_t *errbuf){
    bool ok = true;
    *out = (addrspec_t){0};

    bool hex = false;
    web_scanner_t s = web_scanner(text);
    WEB_ONSTACK_PARSER(p,
        WEB_ADDRSPEC_MAX_CALLSTACK,
        WEB_ADDRSPEC_MAX_SEMSTACK
    );

    while(true){
        web_scanned_t scanned = web_scanner_next(&s, hex);
        hex = false;
        web_status_e status = web_parse_addrspec(
            &p,
            text,
            &hex,
            errbuf,
            scanned.token,
            scanned.loc,
            out,
            NULL,
            url_handle_error
        );
        switch(status){
            case WEB_STATUS_OK: continue;
            case WEB_STATUS_DONE: goto done;
            case WEB_STATUS_SYNTAX_ERROR:
                // message was written in url_handle_error
                ok = false;
                goto done;

            case WEB_STATUS_CALLSTACK_OVERFLOW:
                if(errbuf){
                    FMT_QUIET(errbuf,
                        "web_parse_addrspec() CALLSTACK_OVERFLOW"
                    );
                }
                ok = false;
                goto done;

            case WEB_STATUS_SEMSTACK_OVERFLOW:
                if(errbuf){
                    FMT_QUIET(errbuf,
                        "web_parse_addrspec() SEMSTACK_OVERFLOW"
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

derr_t parse_addrspec(const dstr_t *text, addrspec_t *out){
    derr_t e = E_OK;

    DSTR_VAR(errbuf, 512);

    bool ok = parse_addrspec_ex(text, out, &errbuf);
    if(!ok){
        ORIG(&e, E_PARAM, "invalid addrspec:\n%x", FD(errbuf));
    }

    return e;
}

addrspec_t must_parse_addrspec(const dstr_t *text){
    addrspec_t out;
    DSTR_VAR(errbuf, 512);
    if(!parse_addrspec_ex(text, &out, &errbuf)){
        LOG_FATAL(
            "failed to parse addrspec (%x):\n%x\n", FD(*text), FD(errbuf)
        );
    }
    return out;
}

// get the string behind the addrspec
dstr_t addrspec_text(addrspec_t spec){
    return dstr_from_off(dstr_off_extend(spec.scheme, spec.port));
}

// out must be freed with freeaddrinfo
derr_t getaddrspecinfo(
    const addrspec_t spec, bool passive, struct addrinfo **out
){
    derr_t e = E_OK;

    *out = NULL;

    char hostbuf[256];
    if(spec.host.len + 1 > sizeof(hostbuf)){
        ORIG(&e, E_PARAM, "spec host too long");
    }
    memcpy(hostbuf, spec.host.buf->data + spec.host.start, spec.host.len);
    hostbuf[spec.host.len] = '\0';
    char *host = spec.host.len ? hostbuf : NULL;
    // detect IP_LITERAL form: [...]
    if(
        spec.host.len > 2
        && hostbuf[0] == '['
        && hostbuf[spec.host.len - 1] == ']'
    ){
        // let getaddrinfo interpret the ip literal
        hostbuf[spec.host.len - 1] = '\0';
        host++;
    }

    char portbuf[32];
    if(spec.port.len + 1 > sizeof(portbuf)){
        ORIG(&e, E_PARAM, "spec port too long");
    }
    memcpy(portbuf, spec.port.buf->data + spec.port.start, spec.port.len);
    portbuf[spec.port.len] = '\0';

    // prepare for getaddrinfo
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = (passive * AI_PASSIVE);

    // get address of host
    int ret = getaddrinfo(host, portbuf, &hints, out);
    if(ret){
        ORIG(&e,
            E_OS,
            "getaddrinfo(name=%x, service=%x): %x\n",
            FS(hostbuf),
            FS(portbuf),
            FS(gai_strerror(ret))
        );
    }
    if(!*out){
        ORIG(&e, E_OS, "getaddrinfo did not fail but didn't return anything");
    }

    return e;
}
