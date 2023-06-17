#include "libweb/libweb.h"

// recorded error message is based on sem.loc and buf, and is written to errbuf
static void weblink_handle_error(
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

    weblinks_t *wp = data;

    FMT_QUIET(&wp->errbuf, "invalid link: ");
    // aim for 80 characters of context
    get_token_context(&wp->errbuf, sem.loc, 80);
}

// returns bool ok
static bool parse_more(weblinks_t *p){
    // we already returned the last thing
    if(p->done) return false;

    // mark our starting point
    size_t nurl = p->nurl;
    size_t nlabel = p->nlabel;

    // no need to store hex on weblinks_t snkince we never end on hex=true
    bool hex = false;

    while(true){
        web_scanned_t scanned = web_scanner_next(&p->scanner, hex);
        hex = false;
        web_status_e status = web_parse_weblink(
            &p->p,
            p->text,
            &hex,
            p,
            scanned.token,
            scanned.loc,
            NULL,
            weblink_handle_error
        );
        switch(status){
            case WEB_STATUS_OK:
                // did we hit an error?
                if(p->etype){
                    p->done = true;
                    return false;
                }
                // did we finish something returnable?
                if(p->nurl != nurl || p->nlabel != nlabel) return true;
                // otherwise keep parsing
                continue;

            case WEB_STATUS_DONE:
                p->done = true;
                return true;

            case WEB_STATUS_SYNTAX_ERROR:
                p->etype = E_PARAM;
                p->done = true;
                return false;

            case WEB_STATUS_CALLSTACK_OVERFLOW:
                FMT_QUIET(&p->errbuf, "CALLSTACK_OVERFLOW");
                p->etype = E_INTERNAL;
                p->done = true;
                return false;

            case WEB_STATUS_SEMSTACK_OVERFLOW:
                FMT_QUIET(&p->errbuf, "SEMSTACK_OVERFLOW");
                p->etype = E_INTERNAL;
                p->done = true;
                return false;
        }
    }
}

url_t *weblinks_iter(weblinks_t *p, const dstr_t *in){
    *p = (weblinks_t){0};
    p->scanner = web_scanner(in);
    web_parser_prep(
        &p->p,
        p->callstack,
        sizeof(p->callstack)/sizeof(*p->callstack),
        p->semstack,
        sizeof(p->semstack)/sizeof(*p->semstack)
    );
    DSTR_WRAP_ARRAY(p->buf, p->_buf);
    DSTR_WRAP_ARRAY(p->errbuf, p->_errbuf);
    return weblinks_next(p);
}

url_t *weblinks_next(weblinks_t *p){
    if(p->etype) return NULL;
    size_t nurl = p->nurl;
    while(p->nurl == nurl){
        bool ok = parse_more(p);
        if(!ok) return NULL;
    }
    return &p->url;
}

weblink_param_t *weblinks_next_param(weblinks_t *p){
    if(p->etype) return NULL;
    if(!p->nlabel) return NULL;
    // parse until either nlabel is bumped
    size_t nlabel = p->nlabel;
    while(p->nlabel == nlabel){
        bool ok = parse_more(p);
        if(!ok) return NULL;
    }
    // out of labels?
    if(!p->nlabel) return NULL;
    // if nurl bumped, return that instead
    return &p->param;
}

// after iterating, check for errors
derr_type_t weblinks_status(weblinks_t *p){
    return p->etype;
}

// in error cases, get the error buffer
dstr_t weblinks_errbuf(weblinks_t *p){
    return p->errbuf;
}
