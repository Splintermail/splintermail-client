#include "libweb/libweb.h"

// recorded error message is based on sem.loc, and is written to E
void http_handle_error(
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

    derr_t *E = data;

    // skip if E was not provided
    if(!E) return;

    // longest DBG char is \xNN, or 4 chars, and we have two lines of len 80
    // 80 * 4 * 2 = 640
    DSTR_VAR(context, 1024);
    get_token_context(&context, sem.loc, 80);

    TRACE_ORIG(E, E_RESPONSE, "invalid http:\n%x", FD(context));
}

// Create a reader that will expect to find all headers in the provided buffer.
// Each time the buffer is filled, you should http_read() again
void http_reader_init(http_reader_t *r, dstr_t *buf){
    *r = (http_reader_t){
        .buf = buf,
        .s = web_scanner(buf),
        .p = (web_parser_t){
            .callstack = r->callstack,
            .callsmax = sizeof(r->callstack) / sizeof(*r->callstack),
            .semstack = r->semstack,
            .semsmax = sizeof(r->semstack) / sizeof(*r->semstack),
        }
    };
    DSTR_WRAP_ARRAY(r->reason, r->_reason);
}

void http_reader_free(http_reader_t *r){
    web_parser_reset(&r->p);
}

// read the next header in the http message.
/* state is one of -2="incomplete read", -1="header found", or the index of
   the first byte of the body */
derr_t http_read(http_reader_t *r, http_pair_t *pair, int *state_out){
    derr_t e = E_OK;
    *state_out = -2;
    bool hex = false;

    while(true){
        // try to scan (http scanner never uses hexmode)
        web_scanned_t scanned = web_scanner_next(&r->s, hex);
        if(scanned.wantmore){
            // incomplete read
            if(r->consumed){
                // left-shift the buffer
                dstr_leftshift(r->buf, r->consumed);
                r->consumed = 0;
                // reset scanner state
                r->s = web_scanner(r->buf);
                // any tokens we've built up are now invalid
                web_parser_reset(&r->p);
            }
            return e;
        }

        // feed token to parser
        web_status_e status;
        if(r->status == 0){
            // still parsing status line
            http_status_line_t status_line;
            status = web_parse_status_line(
                &r->p,
                r->buf,
                &hex,
                &e,
                scanned.token,
                scanned.loc,
                &status_line,
                NULL,
                http_handle_error
            );
            if(status == WEB_STATUS_DONE){
                // finished the status line
                r->status = status_line.code;
                // copy the reason into stable memory
                dstr_t sub = dstr_sub2(
                    status_line.reason, 0, r->reason.size-1
                );
                NOFAIL_GO(&e, E_FIXEDSIZE,
                    dstr_append(&r->reason, &sub),
                fail);
                NOFAIL_GO(&e, E_FIXEDSIZE,
                    dstr_null_terminate(&r->reason),
                fail);
                r->consumed = r->s.used;
                // continue parsing the first header
                continue;
            }
        }else{
            // parsing headers
            http_pair_t hdr_line;
            status = web_parse_hdr_line(
                &r->p,
                r->buf,
                &hex,
                &e,
                scanned.token,
                scanned.loc,
                &hdr_line,
                NULL,
                http_handle_error
            );
            if(status == WEB_STATUS_DONE){
                if(hdr_line.key.len){
                    // finished a header line
                    *state_out = -1;
                    r->consumed = r->s.used;
                }else{
                    // end of headers
                    if(r->s.used > INT_MAX){
                        ORIG_GO(&e,
                            E_FIXEDSIZE, "headers are way too long",
                        fail);
                    }
                    *state_out = (int)r->s.used;
                }
                *pair = hdr_line;
                return e;
            }
        }

        switch(status){
            case WEB_STATUS_OK: break;
            case WEB_STATUS_DONE: break; // not possible; handled above

            case WEB_STATUS_SYNTAX_ERROR:
                // allow a pre-formatted error
                if(is_error(e)) goto fail;
                ORIG_GO(&e, E_RESPONSE, "invalid http response", fail);

            case WEB_STATUS_CALLSTACK_OVERFLOW:
                ORIG_GO(&e,
                    E_INTERNAL, "http parser CALLSTACK_OVERFLOW",
                fail);

            case WEB_STATUS_SEMSTACK_OVERFLOW:
                ORIG_GO(&e,
                    E_INTERNAL, "http parser SEMSTACK_OVERFLOW",
                fail);
        }
    }

fail:
    web_parser_reset(&r->p);
    return e;
}

derr_type_t http_quoted_string_decode(const dstr_t in, dstr_t *out){
    for(size_t i = 0; i < in.len; i++){
        char c = in.data[i];
        if(c != '\\'){
            derr_type_t etype = dstr_append_char(out, c);
            if(etype) return etype;
            continue;
        }
        if(++i == in.len) return E_PARAM;
        /* from rfc7230, http syntax: "Recipients that process the value of a
           quoted-string MUST handle a quoted-pair as if it were replaced by
           the octet following the backslash." */
        char c2 = in.data[i];
        derr_type_t etype = dstr_append_char(out, c2);
        if(etype) return etype;
    }
    return E_NONE;
}
