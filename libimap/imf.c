#include <stdlib.h>

#include "libimap/libimap.h"

DSTR_STATIC(D_TEXT, "text");
DSTR_STATIC(D_PLAIN, "plain");
DSTR_STATIC(D_MESSAGE, "message");
DSTR_STATIC(D_RFC822, "rfc822");

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

    imf_hdr_t **last = &list;
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

mime_version_t mime_version_parse(
    dstr_off_t major, dstr_off_t minor, bool *ok
){
    derr_t e = E_OK;

    dstr_t d_major = dstr_from_off(major);
    dstr_t d_minor = dstr_from_off(minor);

    unsigned int u_major;
    unsigned int u_minor;

    PROP_GO(&e, dstr_tou(&d_major, &u_major, 10), fail);
    PROP_GO(&e, dstr_tou(&d_minor, &u_minor, 10), fail);

    if(ok) *ok = true;
    return (mime_version_t){ .major = u_major, .minor = u_minor };

fail:
    DROP_VAR(&e);
    if(ok) *ok = false;
    return (mime_version_t){0};
}

mime_param_t *mime_param_new(derr_t *e, ie_dstr_t *key, ie_dstr_t *val){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, mime_param_t, p, fail);

    p->key = key;
    p->val = val;
    p->next = NULL;

    return p;

fail:
    ie_dstr_free(key);
    ie_dstr_free(val);
    return NULL;
}

void mime_param_free(mime_param_t *p){
    if(!p) return;
    mime_param_free(p->next);
    ie_dstr_free(p->key);
    ie_dstr_free(p->val);
    free(p);
}

mime_param_t *mime_param_add(derr_t *e, mime_param_t *list, mime_param_t *new){
    if(is_error(*e)) goto fail;

    mime_param_t **last = &list;
    while(*last != NULL) last = &(*last)->next;
    *last = new;

    return list;

fail:
    mime_param_free(list);
    mime_param_free(new);
    return NULL;
}

mime_content_type_t *mime_content_type_new(
    derr_t *e,
    ie_dstr_t *typestr,
    ie_dstr_t *subtypestr,
    mime_param_t *params
){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, mime_content_type_t, content_type, fail);

    if(dstr_icmp2(typestr->dstr, DSTR_LIT("message")) == 0){
        content_type->type = MIME_TYPE_MESSAGE;
    }else if(dstr_icmp2(typestr->dstr, DSTR_LIT("multipart")) == 0){
        content_type->type = MIME_TYPE_MULTIPART;
    }else if(dstr_icmp2(typestr->dstr, DSTR_LIT("text")) == 0){
        content_type->type = MIME_TYPE_TEXT;
    }else{
        content_type->type = MIME_TYPE_OTHER;
    }

    if(dstr_icmp2(subtypestr->dstr, DSTR_LIT("rfc822")) == 0){
        content_type->subtype = MIME_SUBTYPE_RFC822;
    }else if(dstr_icmp2(subtypestr->dstr, DSTR_LIT("digest")) == 0){
        content_type->subtype = MIME_SUBTYPE_DIGEST;
    }else{
        content_type->subtype = MIME_SUBTYPE_OTHER;
    }

    content_type->typestr = typestr;
    content_type->subtypestr = subtypestr;
    content_type->params = params;

    return content_type;

fail:
    ie_dstr_free(typestr);
    ie_dstr_free(subtypestr);
    mime_param_free(params);
    return NULL;
}

void mime_content_type_free(mime_content_type_t *content_type){
    if(!content_type) return;
    ie_dstr_free(content_type->typestr);
    ie_dstr_free(content_type->subtypestr);
    mime_param_free(content_type->params);
    free(content_type);
}

// parser

void imf_handle_error(
    imf_parser_t *p,
    derr_t *E,
    const dstr_t *bytes,
    size_t skip,
    imf_token_e imf_token,
    imf_sem_t sem,
    const unsigned char *expected_mask,
    const char *loc_summary
){
    (void)p; (void)E; (void)skip;

    // aim for 80 characters of context
    size_t head_len = MIN(sem.loc.start, 40);
    size_t token_len = MIN(sem.loc.len, 80 - head_len);
    size_t tail_len = 80 - MIN(80, head_len + token_len);

    dstr_t head = dstr_sub2(*bytes, sem.loc.start - head_len, sem.loc.start);
    dstr_t token = dstr_sub2(*bytes, sem.loc.start, sem.loc.start + token_len);
    dstr_t tail = dstr_sub2(
        *bytes, sem.loc.start + token_len, sem.loc.start + token_len + tail_len
    );

    // longest DBG char is \xNN, or 4 chars, and we have two lines of len 80
    // 80 * 4 = 320
    DSTR_VAR(buf, 512);
    DROP_CMD( FMT(&buf, "    %x", FD_DBG(head)) );
    size_t nspaces = buf.len;
    DROP_CMD( FMT(&buf, "%x", FD_DBG(token)) );
    size_t ncarets = MAX(buf.len - nspaces, 1);
    DROP_CMD( FMT(&buf, "%x\n", FD_DBG(tail)) );

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

    LOG_ERROR(
        "Error parsing email message: %x\n%x\n", FS(loc_summary), FD(buf)
    );
    // reuse buf
    int l = imf_snprint_mask(buf.data, buf.size, expected_mask, ",");
    if(l > 0 && (size_t)l < buf.size){
        buf.len = (size_t)l;
        LOG_ERROR(
            "Got %x, but expected one of [%x]\n",
            FS(imf_token_name(imf_token)), FD(buf)
        );
    }
}

static derr_t do_imf_hdrs_parse(
    const dstr_t *msg,
    imf_scanner_t *s,
    imf_hdrs_t **out
){
    derr_t e = E_OK;
    *out = NULL;

    IMF_ONSTACK_PARSER(p, IMF_HDRS_MAX_CALLSTACK, IMF_HDRS_MAX_SEMSTACK);

    size_t skip = s->start_idx;

    imf_token_e token_type = 0;
    do {
        dstr_off_t token;
        PROP(&e, imf_scan(s, &token, &token_type) );

        imf_status_e status = imf_parse_hdrs(
            &p, &e, msg, skip, token_type, token, out, NULL
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

    // we either use the submsg length or the length of the whole msg buffer
    size_t msg_len = fixed_length ? *fixed_length : msg->len;

    dstr_off_t bytes = {
        .buf = msg,
        .start = _hdrs->bytes.start,
        .len = msg_len
    };
    dstr_off_t body = {
        .buf = msg,
        .start = _hdrs->bytes.start + _hdrs->bytes.len,
        .len = msg_len - _hdrs->bytes.len,
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
    const dstr_off_t bytes,
    imf_hdrs_t **hdrs,  // optional, to provide pre-parsed headers. Consumed.
    imf_t **out
){
    derr_t e = E_OK;
    PROP(&e,
        do_imf_parse(
            bytes.buf, bytes.start, &bytes.len, hdrs, out
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
derr_t imf_hdrs_parse_sub(const dstr_off_t bytes, imf_hdrs_t **out){
    derr_t e = E_OK;

    imf_scanner_t s = imf_scanner_prep(
        bytes.buf, bytes.start, &bytes.len, NULL, NULL
    );
    PROP(&e, do_imf_hdrs_parse(bytes.buf, &s, out) );

    return e;
}

// individual field handling
#define DETECT_HDR(x) \
    if(x##_field.len == 0){ \
        if(dstr_icmp2(name, d_##x) == 0){ \
            x##_field = dstr_from_off(hdr->value); \
            need--; \
        } \
    }

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
            &p, e, &out##_field, 0, token_type, token, &out, NULL \
        ); \
        if(is_error(*e)) break; \
        switch(status){ \
            case IMF_STATUS_OK: continue; \
            case IMF_STATUS_DONE: should_continue = false; break; \
            case IMF_STATUS_SYNTAX_ERROR: \
                LOG_WARN("syntax error parsing " name " field, skipping.\n"); \
                LOG_WARN(name ": %x\n", FD_DBG(out##_field)); \
                should_continue = false; \
                break; \
            \
            case IMF_STATUS_SEMSTACK_OVERFLOW: \
                LOG_ERROR( \
                    "semstack overflow parsing " name " field, skipping.\n" \
                ); \
                LOG_ERROR(name ": %x\n", FD_DBG(out##_field)); \
                should_continue = false; \
                break; \
            \
            case IMF_STATUS_CALLSTACK_OVERFLOW: \
                LOG_ERROR( \
                    "callstack overflow parsing " name " field, skipping.\n" \
                ); \
                LOG_ERROR(name "contents: %x\n", FD_DBG(out##_field)); \
                should_continue = false; \
                break; \
        } \
    } while(should_continue && token_type != IMF_EOF); \
    imf_parser_reset(&p); \
} while(0);

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
        DETECT_HDR(date)
        DETECT_HDR(subj)
        DETECT_HDR(from)
        DETECT_HDR(sender)
        DETECT_HDR(reply_to)
        DETECT_HDR(to)
        DETECT_HDR(cc)
        DETECT_HDR(bcc)
        DETECT_HDR(in_reply_to)
        DETECT_HDR(msg_id)
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

    // note: date is unstructured becase the IMAP ENVELOPE uses nstring
    imf_parse_or_nil(e, date, "Date", imf_parse_unstruct_field);
    imf_parse_or_nil(e, subj, "Subject", imf_parse_subj_field);
    imf_parse_or_nil(e, from, "From", imf_parse_from_field);
    imf_parse_or_nil(e, sender, "Sender", imf_parse_sender_field);
    imf_parse_or_nil(e, reply_to, "Reply-To", imf_parse_reply_to_field);
    imf_parse_or_nil(e, to, "To", imf_parse_to_field);
    imf_parse_or_nil(e, cc, "Cc", imf_parse_cc_field);
    imf_parse_or_nil(e, bcc, "Bcc", imf_parse_bcc_field);
    imf_parse_or_nil(e, in_reply_to, "In-Reply-To", imf_parse_in_reply_to_field);
    imf_parse_or_nil(e, msg_id, "Message-Id", imf_parse_msg_id_field);

    #define stripws(in) dstr_strip_chars(in, ' ', '\t', '\r', '\n')
    // note: present-but-empty in-reply-to/msg-id should be non-NIL
    // so if there was a syntax error, return the whole field, stripped
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

// MIME message parsing

mime_content_type_t *read_mime_content_type(
    derr_t *e,
    const imf_hdrs_t *hdrs,
    dstr_t default_type,
    dstr_t default_subtype
){
    if(is_error(*e)) return NULL;

    dstr_t version_field = {0};
    dstr_t content_type_field = {0};

    DSTR_STATIC(d_version, "mime-version");
    DSTR_STATIC(d_content_type, "content-type");

    // first pass: just locate headers we plan to read.
    size_t need = 2;
    for(imf_hdr_t *hdr = hdrs->hdr; hdr && need; hdr = hdr->next){
        dstr_t name = dstr_from_off(hdr->name);
        // ignore empty fields
        if(!hdr->value.len) continue;
        DETECT_HDR(version)
        DETECT_HDR(content_type)
    }

    // second pass: parse each header
    mime_version_t version = {0};
    mime_content_type_t *content_type = NULL;

    /* imf has a recursive grammar, but only in comments.
       wtf would you nest comments so many times to hit a stackoverflow in my
       parser?  I don't know, but I'm happy to report those fields as NIL and
       call it a day. */
    IMF_ONSTACK_PARSER(p, 20, 100);

    // parse the structured fields
    imf_parse_or_nil(e, version, "MIME-Version", imf_parse_mime_version_field);
    imf_parse_or_nil(e, content_type, "Content-Type", imf_parse_content_type_field);

    // whenever the mime version is present but not exactly 1.0, just log it
    if(!is_error(*e) && version_field.data){
        // version field is present, should parse as exactly 1.0
        if(version.major != 1 || version.minor != 0){
            LOG_WARN(
                "unrecognized MIME-Version (\"%x\"), continuing anyway...\n",
                FD(version_field)
            );
        }
    }

    if(content_type == NULL){
        // apply default type and subtype
        content_type = mime_content_type_new(e,
            ie_dstr_new2(e, default_type),
            ie_dstr_new2(e, default_subtype),
            NULL
        );
    }

    return content_type;
}


derr_t read_bodystruct_info(
    const imf_hdrs_t *hdrs,
    dstr_t default_type,
    dstr_t default_subtype,
    mime_content_type_t **content_type_out,
    ie_dstr_t **content_id_out,
    ie_dstr_t **description_out,
    ie_dstr_t **content_transfer_encoding_out,
    ie_dstr_t **md5_out,
    ie_body_disp_t **disposition_out,
    ie_dstr_t **lang_out,
    ie_dstr_t **location_out
){
    *content_type_out = NULL;
    *content_id_out = NULL;
    *description_out = NULL;
    *content_transfer_encoding_out = NULL;
    *md5_out = NULL;
    *disposition_out = NULL;
    *lang_out = NULL;
    *location_out = NULL;

    derr_t e = E_OK;

    dstr_t version_field = {0};
    dstr_t content_type_field = {0};
    dstr_t content_id_field = {0};
    dstr_t description_field = {0};
    dstr_t content_transfer_encoding_field = {0};
    dstr_t md5_field = {0};
    dstr_t disposition_field = {0};
    dstr_t lang_field = {0};
    dstr_t location_field = {0};

    DSTR_STATIC(d_version, "mime-version");
    DSTR_STATIC(d_content_type, "content-type");
    DSTR_STATIC(d_content_id, "content-id");
    DSTR_STATIC(d_description, "contentmdescription");
    DSTR_STATIC(d_content_transfer_encoding, "content-transfer-encoding");
    DSTR_STATIC(d_md5, "content-md5");
    DSTR_STATIC(d_disposition, "content-disposition");
    DSTR_STATIC(d_lang, "content-language");
    DSTR_STATIC(d_location, "content-location");

    // first pass: just locate headers we plan to read.
    size_t need = 9;
    for(imf_hdr_t *hdr = hdrs->hdr; hdr && need; hdr = hdr->next){
        dstr_t name = dstr_from_off(hdr->name);
        // ignore empty fields
        if(!hdr->value.len) continue;
        DETECT_HDR(version)
        DETECT_HDR(content_type)
        DETECT_HDR(content_id)
        DETECT_HDR(description)
        DETECT_HDR(content_transfer_encoding)
        DETECT_HDR(md5)
        DETECT_HDR(disposition)
        DETECT_HDR(lang)
        DETECT_HDR(location)
    }

    // second pass: parse each header
    mime_version_t version = {0};
    mime_content_type_t *content_type = NULL;
    ie_dstr_t *content_id = NULL;
    ie_dstr_t *description = NULL;
    ie_dstr_t *content_transfer_encoding = NULL;
    ie_dstr_t *md5 = NULL;
    ie_body_disp_t *disposition = NULL;
    ie_dstr_t *lang = NULL;
    ie_dstr_t *location = NULL;

    /* imf has a recursive grammar, but only in comments.
       wtf would you nest comments so many times to hit a stackoverflow in my
       parser?  I don't know, but I'm happy to report those fields as NIL and
       call it a day. */
    IMF_ONSTACK_PARSER(p, 20, 100);

    // parse the structured fields
    imf_parse_or_nil(&e, version, "MIME-Version", imf_parse_mime_version_field);
    imf_parse_or_nil(&e, content_type, "Content-Type", imf_parse_content_type_field);
    imf_parse_or_nil(&e, content_id, "Content-Id", imf_parse_content_id_field);
    imf_parse_or_nil(&e, description, "Content-Description", imf_parse_content_description_field);
    imf_parse_or_nil(&e,
        content_transfer_encoding,
        "Content-Transfer-Encoding",
        imf_parse_content_transfer_encoding_field
    );
    imf_parse_or_nil(&e, md5, "Content-MD5", imf_parse_content_md5_field);
    imf_parse_or_nil(&e, disposition, "Content-Disposition", imf_parse_content_disposition_field);
    imf_parse_or_nil(&e, lang, "Content-Lanaguage", imf_parse_content_language_field);
    imf_parse_or_nil(&e, location, "Content-Location", imf_parse_content_location_field);

    CHECK_GO(&e, fail);

    // whenever the mime version is present but not exactly 1.0, just log it
    if(version_field.data){
        // version field is present, should parse as exactly 1.0
        if(version.major != 1 || version.minor != 0){
            LOG_WARN(
                "unrecognized MIME-Version (\"%x\"), continuing anyway...\n",
                FD(version_field)
            );
        }
    }

    if(content_type == NULL){
        // apply default type and subtype
        content_type = mime_content_type_new(&e,
            ie_dstr_new2(&e, default_type),
            ie_dstr_new2(&e, default_subtype),
            NULL
        );
        CHECK_GO(&e, fail);
    }

    *content_type_out = content_type;
    *content_id_out = content_id;
    *description_out = description;
    *content_transfer_encoding_out = content_transfer_encoding;
    *md5_out = md5;
    *disposition_out = disposition;
    *lang_out = lang;
    *location_out = location;

    return e;

fail:
    mime_content_type_free(content_type);
    ie_dstr_free(content_id);
    ie_dstr_free(description);
    ie_dstr_free(content_transfer_encoding);
    ie_dstr_free(md5);
    ie_body_disp_free(disposition);
    ie_dstr_free(lang);
    ie_dstr_free(location);

    return e;
}

// returns a substring owned by the content_type
static dstr_t get_mime_boundary(
    const mime_content_type_t *content_type, bool *missing
){
    *missing = false;
    dstr_t boundary = {0};

    if(!content_type){
        *missing = true;
        goto done;
    }

    // find the boundary parameter
    DSTR_STATIC(d_boundary, "boundary");
    for(mime_param_t *p = content_type->params; p; p = p->next){
        if(dstr_icmp2(p->key->dstr, d_boundary) == 0){
            boundary = p->val->dstr;
            break;
        }
    }

    // boundary is a required parameter
    if(boundary.data == NULL){
        LOG_WARN(
            "boundary parameter not found in multipart content type: %x\n",
            FD_DBG(boundary)
        );
        *missing = true;
        goto done;
    }

done:
    return boundary;
}

dstr_off_t get_multipart_index(
    dstr_off_t bytes,
    size_t skip,
    const dstr_t boundary,
    unsigned int index,
    bool *missing,
    size_t *next_skip
){
    *missing = false;
    dstr_off_t subpart_bytes = {0};

    // enforce 1-indexing
    if(index == 0){
        LOG_WARN("subparts must be one-indexed; section 0 is undefined\n");
        *missing = true;
        goto done;
    }

    DSTR_STATIC(doubledash, "--");

    // as with the imf parser, we support \r, \n, \r\n, and \n\r line endings
    bool in_line_ending = true;  // let the first character try to match
    bool in_tgt = false;
    bool tgtdone = false;
    size_t tgtstart = 0;
    size_t tgtlen = 0;
    for(size_t i = bytes.start + skip; i < bytes.start + bytes.len; i++){
        char c = bytes.buf->data[i];
        // check for line endings
        if(c == '\n' || c == '\r'){
            in_line_ending = true;
            continue;
        }

        // ignore normal characters completely
        if(!in_line_ending) continue;

        // start of a non-empty new line!
        in_line_ending = false;
        dstr_t linestart = dstr_sub2(*bytes.buf, i, i+2);
        if(dstr_cmp2(linestart, doubledash) != 0) continue;
        dstr_t boundarystart = dstr_sub2(*bytes.buf, i+2, i+2+boundary.len);
        if(dstr_cmp2(boundarystart, boundary) != 0) continue;

        // we have a boundary match!

        if(in_tgt){
            // this boundary (end or otherwise) is the end of our search.
            tgtdone = true;
            tgtlen = i - tgtstart;
            if(next_skip) *next_skip = i - bytes.start;
            break;
        }

        // is it an end-boundary, terminating in "--"?
        dstr_t afterboundary = dstr_sub2(
            *bytes.buf, i+2+boundary.len, i+2+boundary.len+2
        );
        if(dstr_cmp2(afterboundary, DSTR_LIT("--")) == 0) break;

        // normal boundary
        if(--index > 0) continue;

        // this is the target boundary!
        tgtstart = i + 2 + boundary.len;
        in_tgt = true;

        // no need to reprocess the boundary characters
        i += (2 + boundary.len) - 1;
    }

    if(!tgtdone){
        *missing = true;
        goto done;
    }

    // post-process, part 1: ignore the first EOL following the boundary
    size_t ltrim = 0;
    bool have_lf = false;
    bool have_cr = false;
    for(; ltrim < tgtlen; ltrim++){
        char c = bytes.buf->data[tgtstart + ltrim];
        if(c == '\r'){
            if(have_cr) break;
            have_cr = true;
        }else if(c == '\n'){
            if(have_lf) break;
            have_lf = true;
        }else if(have_lf || have_cr){
            // first char after first EOL
            break;
        }
        /* we trim any extra characters on the boundary line, since the rfc
           explicitly says implementors should consider it a match even if
           there are extra characters afterwards */
    }

    // post-process, part 2: ignore the final EOL preceeding the final boundary
    size_t rtrim = 0;
    have_lf = false;
    have_cr = false;
    for(; rtrim + ltrim < tgtlen; rtrim++){
        char c = bytes.buf->data[tgtstart + tgtlen - rtrim - 1];
        if(c == '\r'){
            if(have_cr) break;
            have_cr = true;
        }else if(c == '\n'){
            if(have_lf) break;
            have_lf = true;
        }else{
            // last char before final EOL
            break;
        }
    }

    subpart_bytes = (dstr_off_t){
        .buf = bytes.buf,
        .start = tgtstart + ltrim,
        .len = tgtlen - ltrim - rtrim,
    };

done:
    return subpart_bytes;
}

// Returns true if the submessage is missing.
// root_imf: the imf_t for the entire email message.
// sect_part: the section part we are looking for (may be NULL).
// bytes: the actual content of the submessage.
// mime_hdrs: when returning false, the bytes of the relevant mime headers.
// imf: if the submsg has an imf format, it is automatically parsed for you.
// heap_imf: when returning false, if heap_imf is set you'll need to free it.
bool imf_get_submessage(
    derr_t *e,
    const imf_t *root_imf,
    const ie_sect_part_t *sect_part,
    dstr_off_t *bytes_out,
    dstr_off_t *mime_hdrs_out,
    const imf_t **imf_out,
    imf_t **heap_imf_out
){
    *bytes_out = (dstr_off_t){0};
    *mime_hdrs_out = (dstr_off_t){0};
    *imf_out = NULL;
    *heap_imf_out = NULL;
    mime_content_type_t *content_type = NULL;
    if(is_error(*e)) return false;

    bool missing = false;

    // otherwise, descend into the message part-by-part
    const imf_t *imf = root_imf;
    imf_t *heap_imf = NULL;
    dstr_off_t bytes = root_imf->bytes;
    dstr_off_t mime_hdrs_bytes = root_imf->hdrs->bytes;
    imf_hdrs_t *mime_hdrs = NULL;
    bool ismessage = true;
    bool isrfc822 = true;
    bool ismultipart = false;
    bool isdigest = false;
    bool isterminal = false;
    #define UPDATE_CONTENT_TYPE(content_type) do { \
        ismessage = content_type->type == MIME_TYPE_MESSAGE; \
        isrfc822 = content_type->subtype == MIME_SUBTYPE_RFC822; \
        ismultipart = content_type->type == MIME_TYPE_MULTIPART; \
        isdigest = content_type->subtype == MIME_SUBTYPE_DIGEST; \
        isterminal = !ismultipart && !(ismessage && isrfc822); \
    } while(0)

    for(; sect_part; sect_part = sect_part->next){
        CHECK_GO(e, fail);

        // terminal content types have no subparts
        if(isterminal){
            missing = true;
            break;
        }

        if(ismessage && isrfc822){
            mime_content_type_free(content_type);
            content_type = read_mime_content_type(e,
                imf->hdrs, D_TEXT, D_PLAIN
            );
            CHECK_GO(e, fail);
            UPDATE_CONTENT_TYPE(content_type);
            bytes = imf->body;

            /* What would return as .MIME for this part body (in the case that
               this message/rfc822's body isn't a multipart content type)?  The
               IMAP standard seems to leave this case undefined, since the MIME
               header is only associated with part bodies of multipart
               messages.  But there _is_ MIME info for this submessage; it's in
               this message/rfc822's headers.  Dovecot (reasonably) decides to
               return those headers as the MIME output, and we will do the
               same. */
            mime_hdrs_bytes = imf->hdrs->bytes;
            imf = NULL;
            imf_free(STEAL(imf_t, &heap_imf));

            /* Special case handling: you can still request part 1 of a
               message/rfc822 with a terminal body (but only part 1).  Normally
               terminal messages aren't allowed in this loop, so we handle that
               special case here. */
            if(isterminal){
                if(sect_part->n != 1 || sect_part->next != NULL){
                    missing = true;
                    break;
                }
                break;
            }
        }

        /* note: if the message/rfc822 code ran, we're now operating on the
           body of that message */

        // handle multipart/* messages, which have nested subparts:
        if(ismultipart){
            const dstr_t boundary = get_mime_boundary(content_type, &missing);
            if(!missing){
                bytes = get_multipart_index(
                    bytes, 0, boundary, sect_part->n, &missing, NULL
                );
            }
            if(missing){
                if(sect_part->n == 1 && !sect_part->next){
                    /* - "the missing boundary case": the boundary does not
                       exist so we have no way whatsoever to figure out what is
                       contained within, or
                       - "the zero multiparts case": there simply aren't any
                       body parts.
                       Technically, this message has zero valid parts, so any
                       part requested is missing, and should return with a NO
                       response.  However, since the IMAP standard has no way
                       to express a multipart BODY with zero parts, it is
                       possible that we have returned a valid-looking BODY that
                       says that part 1 is an empty text/plain.  Just in case a
                       dumb imap client is fetching every body we said existed,
                       we allow part 1 to be requested, but nothing else. */
                    missing = false;
                    // Content-Type: text/plain; charset=us-ascii
                    mime_content_type_free(content_type);
                    content_type = mime_content_type_new(e,
                        ie_dstr_new2(e, D_TEXT),
                        ie_dstr_new2(e, D_PLAIN),
                        mime_param_new(e,
                            ie_dstr_new2(e, DSTR_LIT("charset")),
                            ie_dstr_new2(e, DSTR_LIT("us-ascii"))
                        )
                    );
                    bytes.len = 0;
                    mime_hdrs_bytes = bytes;
                }
                break;
            }

            // get the mime headers from the subpart
            imf_hdrs_free(mime_hdrs);
            PROP_GO(e, imf_hdrs_parse_sub(bytes, &mime_hdrs), fail);

            mime_content_type_free(content_type);
            if(isdigest){
                // multipart/digest changes the deault Content-Type
                content_type = read_mime_content_type(e,
                    mime_hdrs, D_MESSAGE, D_RFC822
                );
            }else{
                content_type = read_mime_content_type(e,
                    mime_hdrs, D_TEXT, D_PLAIN
                );
            }
            CHECK_GO(e, fail);
            UPDATE_CONTENT_TYPE(content_type);

            mime_hdrs_bytes = mime_hdrs->bytes;

            // remove the mime headers from the bytes we're considering
            bytes.start += mime_hdrs->bytes.len;
            bytes.len -= mime_hdrs->bytes.len;
        }

        if(ismessage && isrfc822){
            // parse an imf-type message
            PROP_GO(e, imf_parse_sub(bytes, NULL, &heap_imf), fail);
            imf = heap_imf;
        }
    }
    #undef UPDATE_CONTENT_TYPE

    if(missing){
        bytes = (dstr_off_t){0};
        mime_hdrs_bytes = (dstr_off_t){0};
        imf_hdrs_free(STEAL(imf_hdrs_t, &mime_hdrs));
        imf_free(STEAL(imf_t, &heap_imf));
        imf = NULL;
    }

    *bytes_out = bytes;
    *mime_hdrs_out = mime_hdrs_bytes;
    *imf_out = imf;
    *heap_imf_out = heap_imf;
    imf_hdrs_free(mime_hdrs);
    mime_content_type_free(content_type);
    return missing;

fail:
    imf_hdrs_free(mime_hdrs);
    imf_free(heap_imf);
    mime_content_type_free(content_type);
    return false;
}

static size_t count_lines(dstr_t text){
    // count CRLF, LFCR, or LF line endings
    size_t nlines = dstr_count2(text, DSTR_LIT("\n"));
    if(nlines == 0){
        // assume CR line endings
        nlines = dstr_count2(text, DSTR_LIT("\r"));
    }
    // don't check worry about a partial line at the end; dovecot doesn't
    return nlines;
}

static derr_t read_bodystructure(
    const imf_hdrs_t *hdrs,
    const dstr_off_t body_bytes,
    const dstr_t default_type,
    const dstr_t default_subtype,
    ie_body_t **body_out
){
    mime_content_type_t *content_type = NULL;
    ie_dstr_t *content_id = NULL;
    ie_dstr_t *description = NULL;
    ie_dstr_t *content_transfer_encoding = NULL;
    ie_dstr_t *md5 = NULL;
    ie_body_disp_t *disposition = NULL;
    ie_dstr_t *lang = NULL;
    ie_dstr_t *location = NULL;

    ie_body_t *parts = NULL;
    imf_hdrs_t *part_hdrs = NULL;
    imf_t *imf = NULL;
    ie_body_t *msgbody = NULL;

    derr_t e = E_OK;

    PROP_GO(&e,
        read_bodystruct_info(
            hdrs, default_type, default_subtype,
            &content_type,
            &content_id,
            &description,
            &content_transfer_encoding,
            &md5,
            &disposition,
            &lang,
            &location
        ),
    cu);

    if(content_type->type == MIME_TYPE_MULTIPART){
        // read the bodystructure for all parts of this message
        ie_body_t *parts = NULL;
        size_t skip = 0;

        // multipart/digest changes the deault Content-Type
        bool isdigest = content_type->subtype == MIME_SUBTYPE_DIGEST;
        const dstr_t default_part_type = isdigest ? D_MESSAGE : D_TEXT;
        const dstr_t default_part_subtype = isdigest ? D_RFC822 : D_PLAIN;

        bool missing = false;
        const dstr_t boundary = get_mime_boundary(content_type, &missing);

        while(!missing){
            // read the next part
            dstr_off_t part_bytes = get_multipart_index(
                body_bytes, skip, boundary, 1, &missing, &skip
            );
            // was there a next part?
            if(missing) break;

            // parse the mime headers of this part
            PROP_GO(&e, imf_hdrs_parse_sub(part_bytes, &part_hdrs), cu);

            // the part body is the whole part without the headers
            dstr_off_t part_body_bytes = {
                .buf = part_bytes.buf,
                .start = part_bytes.start + part_hdrs->bytes.len,
                .len = part_bytes.len - part_hdrs->bytes.len,
            };

            // recurse
            ie_body_t *new_part;
            // delay error handling until ie_body_add
            e = read_bodystructure(
                part_hdrs,
                part_body_bytes,
                default_part_type,
                default_part_subtype,
                &new_part
            );
            parts = ie_body_add(&e, parts, new_part);
            CHECK_GO(&e, cu);

            // done with part_hdrs
            imf_hdrs_free(STEAL(imf_hdrs_t, &part_hdrs));
        }

        /* - "the missing boundary case": the boundary does not exist so we
           have no way whatsoever to figure out what is contained within, or
           - "the zero multiparts case": there simply aren't any body parts.
           The IMAP standard does not allow us to return 0 body parts.  Dovecot
           returns an ie_body_t for an empty, headerless text/plain message,
           so we will too. */
        if(parts == NULL){
            parts = ie_body_text_new(&e,
                mime_content_type_new(&e,
                    ie_dstr_new2(&e, D_TEXT),
                    ie_dstr_new2(&e, D_PLAIN),
                    mime_param_new(&e,
                        ie_dstr_new2(&e, DSTR_LIT("charset")),
                        ie_dstr_new2(&e, DSTR_LIT("us-ascii"))
                    )
                ),
                NULL, // content-id
                NULL, // content-description
                NULL, // content-transfer-encoding
                0, // nbytes
                0, // nlines
                NULL, // content-md5
                NULL, // content-disposition
                NULL, // content-language
                NULL // content-location
            );
            CHECK_GO(&e, cu);
        }

        // build an ie_body_t
        *body_out = ie_body_multi_new(&e,
            STEAL(mime_content_type_t, &content_type),
            STEAL(ie_body_t, &parts),
            STEAL(ie_body_disp_t, &disposition),
            STEAL(ie_dstr_t, &lang),
            STEAL(ie_dstr_t, &location)
        );
        CHECK_GO(&e, cu);

    }else if(content_type->type == MIME_TYPE_TEXT){
        size_t nbytes = body_bytes.len;
        size_t nlines = count_lines(dstr_from_off(body_bytes));
        if(nbytes > UINT_MAX || nlines > UINT_MAX){
            ORIG_GO(&e, E_INTERNAL, "somehow a message is too long", cu);
        }

        *body_out = ie_body_text_new(&e,
            STEAL(mime_content_type_t, &content_type),
            STEAL(ie_dstr_t, &content_id),
            STEAL(ie_dstr_t, &description),
            STEAL(ie_dstr_t, &content_transfer_encoding),
            (unsigned int)nbytes,
            (unsigned int)nlines,
            STEAL(ie_dstr_t, &md5),
            STEAL(ie_body_disp_t, &disposition),
            STEAL(ie_dstr_t, &lang),
            STEAL(ie_dstr_t, &location)
        );
        CHECK_GO(&e, cu);

    }else if(
        content_type->type == MIME_TYPE_MESSAGE
        && content_type->subtype == MIME_SUBTYPE_RFC822
    ){
        // first parse the imf message
        PROP_GO(&e, imf_parse_sub(body_bytes, NULL, &imf), cu);

        // recurse into the body of the imf message
        PROP_GO(&e,
            read_bodystructure(
                imf->hdrs, imf->body, D_TEXT, D_PLAIN, &msgbody
            ),
        cu);

        // count lines and bytes of the body of the imf message
        size_t nbytes = imf->bytes.len;
        size_t nlines = count_lines(dstr_from_off(imf->bytes));
        if(nbytes > UINT_MAX || nlines > UINT_MAX){
            ORIG_GO(&e, E_INTERNAL, "somehow a message is too long", cu);
        }

        ie_envelope_t *envelope = read_envelope_info(&e, imf->hdrs);

        *body_out = ie_body_msg_new(&e,
            STEAL(mime_content_type_t, &content_type),
            STEAL(ie_dstr_t, &content_id),
            STEAL(ie_dstr_t, &description),
            STEAL(ie_dstr_t, &content_transfer_encoding),
            (unsigned int)nbytes,
            envelope,
            STEAL(ie_body_t, &msgbody),
            (unsigned int)nlines,
            STEAL(ie_dstr_t, &md5),
            STEAL(ie_body_disp_t, &disposition),
            STEAL(ie_dstr_t, &lang),
            STEAL(ie_dstr_t, &location)
        );
        CHECK_GO(&e, cu);

    }else{
        size_t nbytes = body_bytes.len;
        if(nbytes > UINT_MAX){
            ORIG_GO(&e, E_INTERNAL, "somehow a message is too long", cu);
        }

        *body_out = ie_body_basic_new(&e,
            STEAL(mime_content_type_t, &content_type),
            STEAL(ie_dstr_t, &content_id),
            STEAL(ie_dstr_t, &description),
            STEAL(ie_dstr_t, &content_transfer_encoding),
            (unsigned int)nbytes,
            STEAL(ie_dstr_t, &md5),
            STEAL(ie_body_disp_t, &disposition),
            STEAL(ie_dstr_t, &lang),
            STEAL(ie_dstr_t, &location)
        );
        CHECK_GO(&e, cu);
    }

cu:
    mime_content_type_free(content_type);
    ie_dstr_free(content_id);
    ie_dstr_free(description);
    ie_dstr_free(content_transfer_encoding);
    ie_dstr_free(md5);
    ie_body_disp_free(disposition);
    ie_dstr_free(lang);
    ie_dstr_free(location);
    ie_body_free(parts);
    imf_hdrs_free(part_hdrs);
    imf_free(imf);
    ie_body_free(msgbody);

    return e;
}

ie_body_t *imf_bodystructure(derr_t *e, const imf_t *root_imf){
    if(is_error(*e)) return NULL;

    ie_body_t *body;

    PROP_GO(e,
        read_bodystructure(
            root_imf->hdrs, root_imf->body, D_TEXT, D_PLAIN, &body
        ),
    fail);

    return body;

fail:
    return NULL;
}

#define IMF_PARSE_LOOP(e, p, parse_fn, in, out, what, label) do { \
    imf_scanner_t s = imf_scanner_prep(&in, 0, NULL, NULL, NULL); \
    bool should_continue = true; \
    imf_token_e token_type; \
    do { \
        dstr_off_t token; \
        imf_scan_builder(e, &s, &token, &token_type); \
        if(is_error(*e)) break; \
        imf_status_e status = parse_fn( \
            p, e, &in, 0, token_type, token, out, NULL \
        ); \
        if(is_error(*e)) break; \
        switch(status){ \
            case IMF_STATUS_OK: continue; \
            case IMF_STATUS_DONE: should_continue = false; break; \
            case IMF_STATUS_SYNTAX_ERROR: \
                imf_parser_reset(p); \
                TRACE(e, "input: %x\n", FD_DBG(in)); \
                ORIG_GO(e, E_PARAM, "syntax error parsing " what, label); \
            \
            case IMF_STATUS_SEMSTACK_OVERFLOW: \
                imf_parser_reset(p); \
                TRACE(e, "input: %x\n", FD_DBG(in)); \
                ORIG_GO(e, \
                    E_FIXEDSIZE, "semstack overflow parsing " what, label \
                ); \
            \
            case IMF_STATUS_CALLSTACK_OVERFLOW: \
                imf_parser_reset(p); \
                TRACE(e, "input: %x\n", FD_DBG(in)); \
                ORIG_GO(e, \
                    E_FIXEDSIZE, "callstack overflow parsing" what, label \
                ); \
        } \
    } while(should_continue && token_type != IMF_EOF); \
    imf_parser_reset(p); \
} while(0)

imap_time_t imf_parse_date_builder(derr_t *e, const dstr_t in){
    imap_time_t out = {0};
    if(is_error(*e)) return out;

    IMF_ONSTACK_PARSER(p, 20, 100);

    IMF_PARSE_LOOP(e, &p, imf_parse_date_field, in, &out, "date", done);

done:
    return out;
}

derr_t imf_parse_date(const dstr_t in, imap_time_t *out){
    derr_t e = E_OK;

    *out = imf_parse_date_builder(&e, in);
    CHECK(&e);

    return e;
}
