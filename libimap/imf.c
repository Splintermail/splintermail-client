#include <stdlib.h>

#include "libimap/libimap.h"

imf_hdr_t *imf_hdr_new(
    derr_t *e,
    dstr_off_t bytes,
    dstr_off_t name,
    dstr_off_t value
){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, imf_hdr_t, hdr, fail);

    hdr->bytes = bytes;
    hdr->name = name;
    hdr->value = value;

    return hdr;

fail:
    return NULL;
}

imf_hdr_t *imf_hdr_add(derr_t *e, imf_hdr_t *list, imf_hdr_t *new){
    if(is_error(*e)) goto fail;

    imf_hdr_t **last = &list->next;
    while(*last != NULL) last = &(*last)->next;
    *last = new;

    return list;

fail:
    imf_hdr_free(list);
    imf_hdr_free(new);
    return NULL;
}

void imf_hdr_free(imf_hdr_t *hdr){
    if(!hdr) return;
    imf_hdr_free(hdr->next);
    free(hdr);
}

imf_hdrs_t *imf_hdrs_new(
    derr_t *e,
    dstr_off_t bytes,
    dstr_off_t sep,
    imf_hdr_t *hdr
){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, imf_hdrs_t, hdrs, fail);

    hdrs->bytes = bytes;
    hdrs->sep = sep;
    hdrs->hdr = hdr;

    return hdrs;

fail:
    imf_hdr_free(hdr);
    return NULL;
}

void imf_hdrs_free(imf_hdrs_t *hdrs){
    if(!hdrs) return;
    imf_hdr_free(hdrs->hdr);
    free(hdrs);
}

imf_t *imf_new(
    derr_t *e,
    dstr_off_t bytes,
    imf_hdrs_t *hdrs,
    dstr_off_t body
){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, imf_t, imf, fail);

    imf->bytes = bytes;
    imf->hdrs = hdrs;
    imf->body = body;

    return imf;

fail:
    imf_hdrs_free(hdrs);
    return NULL;
}

void imf_free(imf_t *imf){
    if(!imf) return;
    imf_hdrs_free(imf->hdrs);
    free(imf);
}

// parser

// void imfyyerror(dstr_off_t *imfyyloc, imf_parser_t *parser, char const *s){
//     const dstr_t bytes = *parser->scanner->bytes;
//
//     // aim for 80 characters of context
//     size_t head_len = MIN(imfyyloc->start, 40);
//     size_t token_len = MIN(imfyyloc->len, 80 - head_len);
//     size_t tail_len = 80 - MIN(80, head_len + token_len);
//
//     dstr_t head = dstr_sub2(bytes, imfyyloc->start - head_len, imfyyloc->start);
//     dstr_t token = dstr_sub2(bytes, imfyyloc->start, imfyyloc->start + token_len);
//     dstr_t tail = dstr_sub2(bytes, imfyyloc->start + token_len, imfyyloc->start + token_len + tail_len);
//
//     // longest DBG char is \xNN, or 4 chars, and we have two lines of len 80
//     // 80 * 4 = 320
//     DSTR_VAR(buf, 512);
//     DROP_CMD( FMT(&buf, "    %x", FD_DBG(&head)) );
//     size_t nspaces = buf.len;
//     DROP_CMD( FMT(&buf, "%x", FD_DBG(&token)) );
//     size_t ncarets = buf.len - nspaces;
//     DROP_CMD( FMT(&buf, "%x\n", FD_DBG(&tail)) );
//
//     // spaces
//     size_t oldlen = buf.len;
//     for(size_t i = 0; i < nspaces && oldlen + i < buf.size; i++){
//         buf.data[buf.len++] = ' ';
//     }
//
//     // carets
//     oldlen = buf.len;
//     for(size_t i = 0; i < ncarets && oldlen + i < buf.size; i++){
//         buf.data[buf.len++] = '^';
//     }
//
//     LOG_ERROR("Error parsing email message: %x:\n%x\n", FS(s), FD(&buf));
// }

static derr_t _read_all(derr_t (*read_fn)(void*, size_t*), void *read_fn_data){
    derr_t e = E_OK;

    if(read_fn == NULL) return e;

    size_t amnt_read;
    do {
        PROP(&e, read_fn(read_fn_data, &amnt_read) );
    } while(amnt_read > 0);

    return e;
}

// completely parse an in-memory message
derr_t imf_parse(
    const dstr_t *msg,
    derr_t (*read_fn)(void*, size_t*),  // NULL for fully-loaded msg
    void *read_fn_data,                 // NULL for fully-loaded msg
    imf_hdrs_t **hdrs,  // optional, to provide pre-parsed headers. Consumed.
    imf_t **out
){
    derr_t e = E_OK;
    *out = NULL;

    // we'll need the whole message no matter what
    PROP(&e, _read_all(read_fn, read_fn_data) );

    imf_hdrs_t *_hdrs = hdrs ? STEAL(imf_hdrs_t, hdrs) : NULL;
    if(_hdrs == NULL){
        // already read the whole message
        PROP(&e, imf_hdrs_parse(msg, NULL, NULL, &_hdrs) );
    }

    dstr_off_t bytes = { .buf = msg, .start = 0, .len = msg->len };
    dstr_off_t body = {
        .buf = msg,
        .start = _hdrs->bytes.len,
        .len = msg->len - _hdrs->bytes.len,
    };

    *out = imf_new(&e, bytes, STEAL(imf_hdrs_t, &_hdrs), body);
    CHECK(&e);

    return e;
}

derr_t imf_hdrs_parse(
    const dstr_t *msg,
    derr_t (*read_fn)(void*, size_t*),
    void *read_fn_data,
    imf_hdrs_t **out
){
    derr_t e = E_OK;
    *out = NULL;

    IMF_ONSTACK_PARSER(p, IMF_HDRS_MAX_CALLSTACK, IMF_HDRS_MAX_SEMSTACK);
    imf_scanner_t s = imf_scanner_prep(msg, read_fn, read_fn_data);

    imf_token_e token_type = 0;
    do {
        dstr_off_t token;
        PROP(&e, imf_scan(&s, &token, &token_type) );

        imf_status_e status = imf_parse_hdrs(
            &p, &e, msg, token_type, token, out, NULL
        );
        CHECK_GO(&e, done);

        switch(status){
            case IMF_STATUS_OK: continue;
            case IMF_STATUS_DONE: goto done;
            case IMF_STATUS_SYNTAX_ERROR:
                ORIG_GO(&e, E_PARAM, "syntax error", done);

            case IMF_STATUS_SEMSTACK_OVERFLOW:
                ORIG_GO(&e,
                    E_INTERNAL, "imf_parse_headers() SEMSTACK_OVERFLOW", done
                );

            case IMF_STATUS_CALLSTACK_OVERFLOW:
                ORIG_GO(&e,
                    E_INTERNAL, "imf_parse_headers() CALLSTACK_OVERFLOW", done
                );
        }
    } while(token_type != IMF_EOF);

done:
    if(is_error(e)){
        imf_hdrs_free(STEAL(imf_hdrs_t, out));
    }
    imf_parser_reset(&p);

    return e;
}
