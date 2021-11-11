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

void imf_handle_error(
    imf_parser_t *p,
    derr_t *E,
    const dstr_t *bytes,
    imf_token_e imf_token,
    imf_sem_t sem,
    const unsigned char *expected_mask,
    const char *loc_summary
){
    (void)p; (void)E;

    // aim for 80 characters of context
    size_t head_len = MIN(sem.loc.start, 40);
    size_t token_len = MIN(sem.loc.len, 80 - head_len);
    size_t tail_len = 80 - MIN(80, head_len + token_len);

    dstr_t head = dstr_sub2(*bytes, sem.loc.start - head_len, sem.loc.start);
    dstr_t token = dstr_sub2(*bytes, sem.loc.start, sem.loc.start + token_len);
    dstr_t tail = dstr_sub2(*bytes, sem.loc.start + token_len, sem.loc.start + token_len + tail_len);

    // longest DBG char is \xNN, or 4 chars, and we have two lines of len 80
    // 80 * 4 = 320
    DSTR_VAR(buf, 512);
    DROP_CMD( FMT(&buf, "    %x", FD_DBG(&head)) );
    size_t nspaces = buf.len;
    DROP_CMD( FMT(&buf, "%x", FD_DBG(&token)) );
    size_t ncarets = buf.len - nspaces;
    DROP_CMD( FMT(&buf, "%x\n", FD_DBG(&tail)) );

    // spaces
    size_t oldlen = buf.len;
    for(size_t i = 0; i < nspaces && oldlen + i < buf.size; i++){
        buf.data[buf.len++] = ' ';
    }

    // carets
    oldlen = buf.len;
    for(size_t i = 0; i < ncarets && oldlen + i < buf.size; i++){
        buf.data[buf.len++] = '^';
    }

    LOG_ERROR("Error parsing email message:%x\n%x\n", FS(loc_summary), FD(&buf));
    // reuse buf
    int l = imf_snprint_mask(buf.data, buf.size, expected_mask, ",");
    if(l > 0 && (size_t)l < buf.size){
        buf.len = (size_t)l;
        LOG_ERROR(
            "Got %x, but expected one of [%x]\n",
            FS(imf_token_name(imf_token)), FD(&buf)
        );
    }
}

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

static derr_t do_imf_hdrs_parse(
    const dstr_t *msg,
    imf_scanner_t *s,
    imf_hdrs_t **out
){
    derr_t e = E_OK;
    *out = NULL;

    IMF_ONSTACK_PARSER(p, IMF_HDRS_MAX_CALLSTACK, IMF_HDRS_MAX_SEMSTACK);

    imf_token_e token_type = 0;
    do {
        dstr_off_t token;
        PROP(&e, imf_scan(s, &token, &token_type) );

        imf_status_e status = imf_parse_hdrs(
            &p, &e, msg, token_type, token, out, NULL
        );
        CHECK_GO(&e, done);

        switch(status){
            case IMF_STATUS_OK: continue;
            case IMF_STATUS_DONE: goto done;
            case IMF_STATUS_SYNTAX_ERROR:
                ORIG_GO(&e, E_PARAM, "syntax error", done);

            case IMF_STATUS_CALLSTACK_OVERFLOW:
                ORIG_GO(&e,
                    E_INTERNAL, "imf_parse_headers() CALLSTACK_OVERFLOW", done
                );

            case IMF_STATUS_SEMSTACK_OVERFLOW:
                ORIG_GO(&e,
                    E_INTERNAL, "imf_parse_headers() SEMSTACK_OVERFLOW", done
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

static derr_t do_imf_parse(
    const dstr_t *msg,
    size_t start_idx,
    const size_t *fixed_length,
    imf_hdrs_t **hdrs,  // optional, to provide pre-parsed headers. Consumed.
    imf_t **out
){
    derr_t e = E_OK;
    *out = NULL;

    imf_hdrs_t *_hdrs = hdrs ? STEAL(imf_hdrs_t, hdrs) : NULL;
    if(_hdrs == NULL){
        // haven't parsed headers yet
        imf_scanner_t s = imf_scanner_prep(
            msg, start_idx, fixed_length, NULL, NULL
        );
        PROP(&e, do_imf_hdrs_parse(msg, &s, &_hdrs));
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

static derr_t _read_all(derr_t (*read_fn)(void*, size_t*), void *read_fn_data){
    derr_t e = E_OK;

    if(read_fn == NULL) return e;

    size_t amnt_read;
    do {
        PROP(&e, read_fn(read_fn_data, &amnt_read) );
    } while(amnt_read > 0);

    return e;
}

// parse a whole message in a dstr_t (with possible read_fn)
derr_t imf_parse(
    const dstr_t *msg,
    derr_t (*read_fn)(void*, size_t*),  // NULL for fully-loaded msg
    void *read_fn_data,                 // NULL for fully-loaded msg
    imf_hdrs_t **hdrs,  // optional, to provide pre-parsed headers. Consumed.
    imf_t **out
){
    derr_t e = E_OK;

    // we'll need the whole message no matter what
    PROP(&e, _read_all(read_fn, read_fn_data) );

    PROP(&e, do_imf_parse(msg, 0, NULL, hdrs, out) );
    return e;
}

// parse a whole message in a dstr_off_t
derr_t imf_parse_sub(
    const dstr_off_t *bytes,
    imf_hdrs_t **hdrs,  // optional, to provide pre-parsed headers. Consumed.
    imf_t **out
){
    derr_t e = E_OK;
    PROP(&e,
        do_imf_parse(
            bytes->buf, bytes->start, &bytes->len, hdrs, out
        )
    );
    return e;
}

// parse headers in a dstr_t (with possible read_fn)
derr_t imf_hdrs_parse(
    const dstr_t *msg,
    derr_t (*read_fn)(void*, size_t*),
    void *read_fn_data,
    imf_hdrs_t **out
){
    derr_t e = E_OK;

    imf_scanner_t s = imf_scanner_prep(msg, 0, NULL, read_fn, read_fn_data);
    PROP(&e, do_imf_hdrs_parse(msg, &s, out) );

    return e;
}

// parse headers in a dstr_off_t
derr_t imf_hdrs_parse_sub(const dstr_off_t *bytes, imf_hdrs_t **out){
    derr_t e = E_OK;

    imf_scanner_t s = imf_scanner_prep(
        bytes->buf, bytes->start, &bytes->len, NULL, NULL
    );
    PROP(&e, do_imf_hdrs_parse(bytes->buf, &s, out) );

    return e;
}

// individual field handling

ie_envelope_t *read_envelope_info(derr_t *e, const imf_hdrs_t *hdrs){
    if(is_error(*e)) return NULL;

    dstr_t date_field = {0};
    dstr_t subj_field = {0};
    dstr_t from_field = {0};
    dstr_t sender_field = {0};
    dstr_t reply_to_field = {0};
    dstr_t to_field = {0};
    dstr_t cc_field = {0};
    dstr_t bcc_field = {0};
    dstr_t in_reply_to_field = {0};
    dstr_t msg_id_field = {0};

    DSTR_STATIC(d_date, "date");
    DSTR_STATIC(d_subj, "subject");
    DSTR_STATIC(d_from, "from");
    DSTR_STATIC(d_sender, "sender");
    DSTR_STATIC(d_reply_to, "reply-to");
    DSTR_STATIC(d_to, "to");
    DSTR_STATIC(d_cc, "cc");
    DSTR_STATIC(d_bcc, "bcc");
    DSTR_STATIC(d_in_reply_to, "in-reply-to");
    DSTR_STATIC(d_msg_id, "message-id");

    // first pass: just locate headers we plan to read.
    size_t need = 10;
    for(imf_hdr_t *hdr = hdrs->hdr; hdr && need; hdr = hdr->next){
        dstr_t name = dstr_from_off(hdr->name);
        // ignore empty fields
        if(!hdr->value.len) continue;
        #define DETECT_HDR(x) \
            if(dstr_icmp2(name, d_##x) == 0){ \
                if(!x##_field.len){ \
                    x##_field = dstr_from_off(hdr->value); \
                    need--; \
                } \
            }
        DETECT_HDR(date)
        else DETECT_HDR(subj)
        else DETECT_HDR(from)
        else DETECT_HDR(sender)
        else DETECT_HDR(reply_to)
        else DETECT_HDR(to)
        else DETECT_HDR(cc)
        else DETECT_HDR(bcc)
        else DETECT_HDR(in_reply_to)
        else DETECT_HDR(msg_id)
        #undef DETECT_HDR
    }

    // defaulting step: sender and reply-to fields default to from field
    if(!sender_field.len) sender_field = from_field;
    if(!reply_to_field.len) reply_to_field = from_field;

    // second pass: parse each header
    ie_dstr_t *date = NULL;
    ie_dstr_t *subj = NULL;
    ie_addr_t *from = NULL;
    ie_addr_t *sender = NULL;
    ie_addr_t *reply_to = NULL;
    ie_addr_t *to = NULL;
    ie_addr_t *cc = NULL;
    ie_addr_t *bcc = NULL;
    ie_dstr_t *in_reply_to = NULL;
    ie_dstr_t *msg_id = NULL;

    /* imf has a recursive grammar, but only in comments.
       wtf would you nest comments so many times to hit a stackoverflow in my
       parser?  I don't know, but I'm happy to report those fields as NIL and
       call it a day. */
    IMF_ONSTACK_PARSER(p, 20, 100);

    #define imf_parse_or_nil(e, out, name, parse_fn) do { \
        if(is_error(*e)) break; \
        if(!out##_field.len) break; \
        imf_scanner_t s = imf_scanner_prep(&out##_field, 0, NULL, NULL, NULL); \
        bool should_continue = true; \
        imf_token_e token_type; \
        do { \
            dstr_off_t token; \
            imf_scan_builder(e, &s, &token, &token_type); \
            if(is_error(*e)) break; \
            imf_status_e status = parse_fn( \
                &p, e, &out##_field, token_type, token, &out, NULL \
            ); \
            if(is_error(*e)) break; \
            switch(status){ \
                case IMF_STATUS_OK: continue; \
                case IMF_STATUS_DONE: should_continue = false; break; \
                case IMF_STATUS_SYNTAX_ERROR: \
                    LOG_WARN("syntax error parsing " name " field, skipping.\n"); \
                    LOG_WARN(name ": %x\n", FD_DBG(&out##_field)); \
                    should_continue = false; \
                    break; \
                \
                case IMF_STATUS_SEMSTACK_OVERFLOW: \
                    LOG_ERROR("semstack overflow parsing " name " field, skipping.\n"); \
                    LOG_ERROR(name ": %x\n", FD_DBG(&out##_field)); \
                    should_continue = false; \
                    break; \
                \
                case IMF_STATUS_CALLSTACK_OVERFLOW: \
                    LOG_ERROR("callstack overflow parsing " name " field, skipping.\n"); \
                    LOG_ERROR(name "contents: %x\n", FD_DBG(&out##_field)); \
                    should_continue = false; \
                    break; \
            } \
        } while(should_continue && token_type != IMF_EOF); \
        imf_parser_reset(&p); \
    } while(0);

    // parse the structured fields
    imf_parse_or_nil(e, from, "From", imf_parse_from_field);
    imf_parse_or_nil(e, sender, "Sender", imf_parse_sender_field);
    imf_parse_or_nil(e, reply_to, "Reply-To", imf_parse_reply_to_field);
    imf_parse_or_nil(e, to, "To", imf_parse_to_field);
    imf_parse_or_nil(e, cc, "Cc", imf_parse_cc_field);
    imf_parse_or_nil(e, bcc, "Bcc", imf_parse_bcc_field);
    imf_parse_or_nil(e, in_reply_to, "In-Reply-To", imf_parse_in_reply_to_field);
    imf_parse_or_nil(e, msg_id, "Message-Id", imf_parse_msg_id_field);

    // note: date is unstructured becase the IMAP ENVELOPE uses nstring
    // note: present-but-empty date/subj/in-reply-to/msg-id should be non-NIL
    #define stripws(in) dstr_strip_chars(in, ' ', '\t', '\r', '\n')
    if(date_field.data) date = ie_dstr_new2(e, stripws(date_field));
    if(subj_field.data) subj = ie_dstr_new2(e, stripws(subj_field));
    // if there was a syntax error, return the whole field, stripped
    if(!in_reply_to && in_reply_to_field.data)
        in_reply_to = ie_dstr_new2(e, stripws(in_reply_to_field));
    if(!msg_id && msg_id_field.data)
        msg_id = ie_dstr_new2(e, stripws(msg_id_field));
    #undef stripws

    return ie_envelope_new(e,
        date,
        subj,
        from,
        sender,
        reply_to,
        to,
        cc,
        bcc,
        in_reply_to,
        msg_id
    );
}
