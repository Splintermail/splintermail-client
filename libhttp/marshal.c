#include "libhttp/libhttp.h"

http_pairs_t *_http_pair_chain(
    http_pairs_t *prev, http_pairs_t **pairs, size_t npairs
){
    if(!npairs) return NULL;
    pairs[0]->prev = prev;
    for(size_t i = 1; i < npairs; i++){
        pairs[i]->prev = pairs[i-1];
    }
    return pairs[npairs-1];
}

dstr_t http_method_to_dstr(http_method_e method){
    switch(method){
        case HTTP_METHOD_GET: return DSTR_LIT("GET");
        case HTTP_METHOD_HEAD: return DSTR_LIT("HEAD");
        case HTTP_METHOD_POST: return DSTR_LIT("POST");
        case HTTP_METHOD_PUT: return DSTR_LIT("PUT");
        case HTTP_METHOD_DELETE: return DSTR_LIT("DELETE");
    }
    return DSTR_LIT("unknown");
}

// convenience macros for shorter lines
#define STATIC_SKIP_FILL(_in) \
    dstr_skip_fill(sf, DSTR_LIT(_in))

/* note, this skip_fill_t implementation skips _output_ bytes, which is
   different from the original imap marshaler that skips _input_ bytes */
typedef struct {
    dstr_t *out;
    size_t skip;   // output bytes left to skip, this will go to zero
    size_t passed; // output bytes handled, will become skip on next run
    size_t want;   // output bytes needed to finish, just for information
} skip_fill_t;

// The base skip_fill.  Skip some bytes, then fill a buffer with what remains.
static void dstr_skip_fill(skip_fill_t *sf, const dstr_t in){
    if(sf->want > 0){
        sf->want += in.len;
    }

    // handle skip
    size_t skip = MIN(sf->skip, in.len);
    sf->skip -= skip;
    sf->passed += skip;

    // don't try to write more than what might fit
    size_t space = sf->out->size - sf->out->len;

    dstr_t appendme = dstr_sub2(in, skip, skip + space);

    if(appendme.len > 0){
        // this can't fail because appendme.len can't exceed space
        derr_type_t e = dstr_append_quiet(sf->out, &appendme);
        if(e != E_NONE){
            LOG_FATAL("appended too much\n");
        }
    }

    sf->passed += appendme.len;

    // did we want to pass more bytes?
    if(skip + space < in.len){
        sf->want += in.len - skip - space;
    }
}


static void path_skip_fill(skip_fill_t *sf, url_t url){
    dstr_t path = dstr_from_off(url.path);

    if(path.len == 0){
        STATIC_SKIP_FILL("/");
    }else{
        dstr_skip_fill(sf, path);
    }
}

static void char_skip_fill(skip_fill_t *sf, char c){
    // are we already counting wants?
    if(sf->want > 0){
        sf->want++;
        return;
    }

    // are we counting down skips still?
    if(sf->skip){
        sf->skip--;
        sf->passed++;
        return;
    }

    // are we counting down skips still?
    if(sf->out->len < sf->out->size){
        sf->out->data[sf->out->len++] = c;
        sf->passed++;
    }else{
        sf->want++;
    }
}

// note that x-www-form-urlencoded requires uppercase hex letters
static char hex_nibble(char nibble){
    switch(nibble){
        case 0: return '0';
        case 1: return '1';
        case 2: return '2';
        case 3: return '3';
        case 4: return '4';
        case 5: return '5';
        case 6: return '6';
        case 7: return '7';
        case 8: return '8';
        case 9: return '9';
        case 10: return 'A';
        case 11: return 'B';
        case 12: return 'C';
        case 13: return 'D';
        case 14: return 'E';
        case 15: return 'F';
    }
    LOG_FATAL("bad nibble: %x\n", FI(nibble));
}

/* Simplified application/x-www-form-urlencoded encoding algorithm.  This
   implementation assumes that all inputs are utf8 strings already.

   See: https://www.w3.org/TR/2014/REC-html5-20141028/forms.html
           #application/x-www-form-urlencoded-encoding-algorithm */
static void url_encode_skip_fill(skip_fill_t *sf, const dstr_t s){
    for(size_t i = 0; i < s.len; i++){
        char c = s.data[i];
        switch(c){
            // allowed characters are:
            // 0x2A, 0x2D, 0x2E, 0x30 to 0x39, 0x41 to 0x5A, 0x5F, 0x61 to 0x7A
            case '*':
            case '-':
            case '.':
            case '_':
            // DIGIT
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
            // ALPHA
            case 'a':
            case 'b':
            case 'c':
            case 'd':
            case 'e':
            case 'f':
            case 'g':
            case 'h':
            case 'i':
            case 'j':
            case 'k':
            case 'l':
            case 'm':
            case 'n':
            case 'o':
            case 'p':
            case 'q':
            case 'r':
            case 's':
            case 't':
            case 'u':
            case 'v':
            case 'w':
            case 'x':
            case 'y':
            case 'z':
            case 'A':
            case 'B':
            case 'C':
            case 'D':
            case 'E':
            case 'F':
            case 'G':
            case 'H':
            case 'I':
            case 'J':
            case 'K':
            case 'L':
            case 'M':
            case 'N':
            case 'O':
            case 'P':
            case 'Q':
            case 'R':
            case 'S':
            case 'T':
            case 'U':
            case 'V':
            case 'W':
            case 'X':
            case 'Y':
            case 'Z':
                char_skip_fill(sf, c);
                break;

            // space becomes '+'
            case ' ':
                char_skip_fill(sf, '+');
                break;

            default:
                // need percent-encoding
                char_skip_fill(sf, '%');
                char_skip_fill(sf, hex_nibble((c >> 4) & 0xf));
                char_skip_fill(sf, hex_nibble((c >> 0) & 0xf));
        }
    }
}

static void params_skip_fill(
    skip_fill_t *sf, url_t url, bool more_params, bool *need_amp
){
    dstr_t url_params = dstr_from_off(url.query);

    // do we have any params to write?
    if(!url_params.len && !more_params) return;

    STATIC_SKIP_FILL("?");

    dstr_skip_fill(sf, url_params);

    // will user-parameters need a '&' separator?
    *need_amp = url_params.len && url_params.data[url_params.len-1] != '&';
}

static void fragment_skip_fill(skip_fill_t *sf, url_t url){
    dstr_t fragment = dstr_from_off(url.fragment);

    if(fragment.len){
        STATIC_SKIP_FILL("#");
        dstr_skip_fill(sf, fragment);
    }
}

static void request_method_url_skip_fill(
    skip_fill_t *sf,
    http_method_e method,
    url_t url,
    bool more_params,
    bool *need_amp
){
    const dstr_t method_str = http_method_to_dstr(method);
    dstr_skip_fill(sf, method_str);

    STATIC_SKIP_FILL(" ");

    /* TODO: when talking to http proxies, the client must to marshal the full
             url rather than just the path at this point */

    path_skip_fill(sf, url);

    params_skip_fill(sf, url, more_params, need_amp);
}

static void request_fragment_version_skip_fill(
    skip_fill_t *sf, url_t url
){
    fragment_skip_fill(sf, url);
    STATIC_SKIP_FILL(" HTTP/1.1\r\n");
}

http_marshaler_t http_marshaler(
    http_method_e method,
    url_t url,
    http_pairs_t *params,
    http_pairs_t *hdrs,
    size_t body_len
){
    // not all methods have a payload
    bool payload = method == HTTP_METHOD_POST || method == HTTP_METHOD_PUT;
    return (http_marshaler_t){
        .method = method,
        .url = url,
        .params = params,
        .hdrs = hdrs,
        .body_len = body_len,
        // initial state
        .hdrptr = hdrs,
        .paramptr = params,
        // set content-length as "already written" when no payload will be sent
        .content_length = !payload,
    };
}

// reuse the config but reset the state
http_marshaler_t http_marshaler_reset(http_marshaler_t m){
    return http_marshaler(
        m.method,
        m.url,
        m.params,
        m.hdrs,
        m.body_len
    );
}

static void hdr_line_skip_fill(skip_fill_t *sf, http_pair_t pair){
    dstr_skip_fill(sf, pair.key);
    STATIC_SKIP_FILL(": ");
    dstr_skip_fill(sf, pair.value);
    STATIC_SKIP_FILL("\r\n");
}

// returns true when marshaling is complete
bool http_marshal_req(
    http_marshaler_t *m,
    dstr_t *buf
){
    // request-line, before params
    if(!m->request_method_url){
        skip_fill_t sf = { .out = buf, .skip = m->skip };
        request_method_url_skip_fill(
            &sf, m->method, m->url, !!m->params, &m->need_amp
        );
        if(sf.want){
            m->skip = sf.passed;
            return false;
        }
        m->request_method_url = true;
        m->skip = 0;
    }

    // every user param
    while(m->paramptr){
        skip_fill_t sf = { .out = buf, .skip = m->skip };
        if(m->need_amp){
            dstr_skip_fill(&sf, DSTR_LIT("&"));
        }else{
            m->need_amp = true;
        }
        http_pair_t pair = m->paramptr->pair;
        url_encode_skip_fill(&sf, pair.key);
        dstr_skip_fill(&sf, DSTR_LIT("="));
        url_encode_skip_fill(&sf, pair.value);
        if(sf.want){
            m->skip = sf.passed;
            return false;
        }
        m->paramptr = m->paramptr->prev;
        m->skip = 0;
    }

    // request-line after params
    if(!m->request_fragment_version){
        skip_fill_t sf = { .out = buf, .skip = m->skip };
        request_fragment_version_skip_fill(&sf, m->url);
        if(sf.want){
            m->skip = sf.passed;
            return false;
        }
        m->request_fragment_version = true;
        m->skip = 0;
    }

    // Host header
    if(!m->host){
        dstr_off_t host_port = dstr_off_extend(m->url.host, m->url.port);
        http_pair_t pair = {
            .key = DSTR_LIT("Host"),
            .value = dstr_from_off(host_port),
        };
        skip_fill_t sf = { .out = buf, .skip = m->skip };
        hdr_line_skip_fill(&sf, pair);
        if(sf.want){
            m->skip = sf.passed;
            return false;
        }
        m->host = true;
        m->skip = 0;
    }

    // Content-Length header
    // (http_marshaller() configures to skip this for non-payload requests)
    if(!m->content_length){
        DSTR_VAR(clbuf, 32);
        derr_type_t etype = FMT_QUIET(&clbuf, "%x", FU(m->body_len));
        if(etype != E_NONE){
            LOG_FATAL(
                "content length doesn't fit in buffer: %x\n", FU(m->body_len)
            );
        }
        http_pair_t pair = {
            .key = DSTR_LIT("Content-Length"),
            .value = clbuf,
        };
        skip_fill_t sf = { .out = buf, .skip = m->skip };
        hdr_line_skip_fill(&sf, pair);
        if(sf.want){
            m->skip = sf.passed;
            return false;
        }
        m->content_length = true;
        m->skip = 0;
    }

    // every user header
    while(m->hdrptr){
        skip_fill_t sf = { .out = buf, .skip = m->skip };
        hdr_line_skip_fill(&sf, m->hdrptr->pair);
        if(sf.want){
            m->skip = sf.passed;
            return false;
        }
        m->hdrptr = m->hdrptr->prev;
        m->skip = 0;
    }

    // final CRLF
    if(!m->eoh){
        skip_fill_t sf = { .out = buf, .skip = m->skip };
        dstr_skip_fill(&sf, DSTR_LIT("\r\n"));
        if(sf.want){
            m->skip = sf.passed;
            return false;
        }
        m->eoh = true;
        m->skip = 0;
    }

    return true;
}
