#include "libdstr/libdstr.h"
#include "libparsing/libparsing.h"
#include "liburl/liburl.h"
#include "libhttp/libhttp.h"

const http_pair_t *http_pairs_iter(
    http_pairs_iter_t *it, http_pairs_t *pairs
){
    *it = (http_pairs_iter_t){ .pairs = pairs, .nread = 0 };
    return http_pairs_next(it);
}

const http_pair_t *http_pairs_next(http_pairs_iter_t *it){
    return it->pairs->next(it->pairs, it->nread++);
}

// Create a reader that will expect to find all headers in the provided buffer.
// Each time the buffer is filled, you should http_read() again
void http_reader_init(http_reader_t *r, const dstr_t *buf){
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
}

// read the next header in the http message.
// status is one of 0="incomplete read", 1="header found", 2="no more headers"
derr_t http_read(http_reader_t *r, http_pair_t *pair, int *status_out){
    derr_t e = E_OK;
    *status_out = 0;

    while(true){
        // try to scan
        http_scanned_t scanned = http_scanner_next(&r->s);
        if(scanned.wantmore){
            // incomplete read
            return e;
        }

        // feed token to parser
        http_status_e status;
        if(r->code == 0){
            // still parsing status line
            http_status_line_t status_line;
            status = http_parse_status_line(
                &r->p, r->buf, scanned.token, scanned.loc, &status_line, NULL
            );
            if(status == HTTP_STATUS_DONE){
                // finished the status line
                r->code = status_line.code;
                r->reason = status_line.reason;
                // continue parsing the first header
                continue;
            }
        }else{
            // parsing headers
            http_pair_t hdr_line;
            status = http_parse_hdr_line(
                &r->p, r->buf, scanned.token, scanned.loc, &hdr_line, NULL
            );
            if(status == HTTP_STATUS_DONE){
                // finished a header line, or end-of-headers
                *status_out = hdr_line.key.len ? 1 : 2;
                *pair = hdr_line;
                return e;
            }
        }

        switch(status){
            case HTTP_STATUS_OK: break;
            case HTTP_STATUS_DONE: break; // not possible; handled above

            case HTTP_STATUS_SYNTAX_ERROR:
                // XXX write http_handle_error
                // message was written in http_handle_error
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
