#include "libdstr/libdstr.h"
#include "libparsing/libparsing.h"
#include "liburl/liburl.h"
#include "libhttp/libhttp.h"

// Create a reader that will expect to find all headers in the provided buffer.
// Each time the buffer is filled, you should http_read() again
void http_reader_init(http_reader_t *r, dstr_t *buf){
    *r = (http_reader_t){
        .buf = buf,
        .s = http_scanner(buf),
        .p = (http_parser_t){
            .callstack = r->callstack,
            .callsmax = sizeof(r->callstack) / sizeof(*r->callstack),
            .semstack = r->semstack,
            .semsmax = sizeof(r->semstack) / sizeof(*r->semstack),
        }
    };
    DSTR_WRAP_ARRAY(r->reason, r->_reason);
}

void http_reader_free(http_reader_t *r){
    http_parser_reset(&r->p);
}

// read the next header in the http message.
/* status is one of -2="incomplete read", -1="header found", or the index of
   the first byte of the body */
derr_t http_read(http_reader_t *r, http_pair_t *pair, int *status_out){
    derr_t e = E_OK;
    *status_out = -2;

    while(true){
        // try to scan
        http_scanned_t scanned = http_scanner_next(&r->s);
        if(scanned.wantmore){
            // incomplete read
            if(r->consumed){
                // left-shift the buffer
                dstr_leftshift(r->buf, r->consumed);
                r->consumed = 0;
                // reset scanner state
                r->s = http_scanner(r->buf);
                // any tokens we've built up are now invalid
                http_parser_reset(&r->p);
            }
            return e;
        }

        // feed token to parser
        http_status_e status;
        if(r->code == 0){
            // still parsing status line
            http_status_line_t status_line;
            status = http_parse_status_line(
                &r->p,
                r->buf,
                &e,
                scanned.token,
                scanned.loc,
                &status_line,
                NULL
            );
            if(status == HTTP_STATUS_DONE){
                // finished the status line
                r->code = status_line.code;
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
            status = http_parse_hdr_line(
                &r->p, r->buf, &e, scanned.token, scanned.loc, &hdr_line, NULL
            );
            if(status == HTTP_STATUS_DONE){
                if(hdr_line.key.len){
                    // finished a header line
                    *status_out = -1;
                    r->consumed = r->s.used;
                }else{
                    // end of headers
                    if(r->s.used > INT_MAX){
                        ORIG_GO(&e,
                            E_FIXEDSIZE, "headers are way too long",
                        fail);
                    }
                    *status_out = (int)r->s.used;
                }
                *pair = hdr_line;
                return e;
            }
        }

        switch(status){
            case HTTP_STATUS_OK: break;
            case HTTP_STATUS_DONE: break; // not possible; handled above

            case HTTP_STATUS_SYNTAX_ERROR:
                // allow a pre-formatted error
                if(is_error(e)) goto fail;
                ORIG_GO(&e, E_RESPONSE, "invalid http response", fail);

            case HTTP_STATUS_CALLSTACK_OVERFLOW:
                ORIG_GO(&e,
                    E_INTERNAL, "http parser CALLSTACK_OVERFLOW",
                fail);

            case HTTP_STATUS_SEMSTACK_OVERFLOW:
                ORIG_GO(&e,
                    E_INTERNAL, "http parser SEMSTACK_OVERFLOW",
                fail);
        }
    }

fail:
    http_parser_reset(&r->p);
    return e;
}
