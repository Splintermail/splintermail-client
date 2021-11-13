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

    LOG_ERROR("Error parsing email message: %x\n%x\n", FS(loc_summary), FD(&buf));
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
    if(dstr_icmp2(name, d_##x) == 0){ \
        if(!x##_field.len){ \
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
        else DETECT_HDR(subj)
        else DETECT_HDR(from)
        else DETECT_HDR(sender)
        else DETECT_HDR(reply_to)
        else DETECT_HDR(to)
        else DETECT_HDR(cc)
        else DETECT_HDR(bcc)
        else DETECT_HDR(in_reply_to)
        else DETECT_HDR(msg_id)
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

// MIME message parsing

mime_content_type_t *read_mime_info(
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
        else DETECT_HDR(content_type)
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
                FD(&version_field)
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

// returns a substring owned by the content_type
static dstr_t get_mime_boundary(
    const mime_content_type_t *content_type, bool *missing
){
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
            FD_DBG(&boundary)
        );
        *missing = true;
        goto done;
    }

done:
    return boundary;
}

dstr_off_t get_multipart_index(
    dstr_off_t bytes,
    const dstr_t boundary,
    unsigned int index,
    bool *missing
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
    for(size_t i = bytes.start; i < bytes.start + bytes.len; i++){
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
// mime_hdrs: the content of the mime headers for any submessage part.
// imf: if the submsg has an imf format, it is automatically parsed for you.
// heap_imf: when returning false, if heap_imf is set you'll need to free it.
bool imf_get_submessage(
    derr_t *e,
    const imf_t *root_imf,
    const ie_sect_part_t *sect_part,
    dstr_off_t *bytes_out,
    imf_hdrs_t **mime_hdrs_out,
    const imf_t **imf_out,
    imf_t **heap_imf_out
){
    *bytes_out = (dstr_off_t){0};
    *mime_hdrs_out = NULL;
    *imf_out = NULL;
    *heap_imf_out = NULL;
    mime_content_type_t *content_type = NULL;
    if(is_error(*e)) return false;

    bool missing = false;

    DSTR_STATIC(d_text, "text");
    DSTR_STATIC(d_plain, "plain");
    DSTR_STATIC(d_message, "message");
    DSTR_STATIC(d_rfc822, "rfc822");

    // otherwise, descend into the message part-by-part
    const imf_t *imf = root_imf;
    imf_t *heap_imf = NULL;
    dstr_off_t bytes = root_imf->bytes;
    imf_hdrs_t *mime_hdrs = NULL;
    bool ismessage = true;
    bool isrfc822 = true;
    bool ismultipart = false;
    bool isdigest = false;
    bool allow_sect_part_on_terminal = false;
    #define UPDATE_CONTENT_TYPE(content_type) do { \
        ismessage = content_type->type == MIME_TYPE_MESSAGE; \
        isrfc822 = content_type->subtype == MIME_SUBTYPE_RFC822; \
        ismultipart = content_type->type == MIME_TYPE_MULTIPART; \
        isdigest = content_type->subtype == MIME_SUBTYPE_DIGEST; \
    } while(0)

    while(sect_part){
        CHECK_GO(e, fail);
        // handle message/rfc822 which have nested subparts:
        if(ismessage && isrfc822){
            mime_content_type_free(content_type);
            content_type = read_mime_info(e, imf->hdrs, d_text, d_plain);
            CHECK_GO(e, fail);
            UPDATE_CONTENT_TYPE(content_type);
            bytes = imf->body;
            imf = NULL;
            imf_free(STEAL(imf_t, &heap_imf));
            /* Don't advance sect_part!  When processing a subpart of a
               message/rfc822, we strip two layers, not just one; the imf
               headers as well as the multipart encoding of the imf body.
               We accomplish this by modifying bytes and content_type but not
               modifying sect_part. */

            /* there's a slight difference in the layer just after the
               message/rfc822; that's the only case where a terminal type is
               allowed to have a sect_part (but only part 1) */
            allow_sect_part_on_terminal = true;

            /* the standard doesn't say what to do if somebody asks for the
               MIME subpart of a message/rfc822.  I think that since you can't
               ask for .MIME on the top-level message, probably it is not
               allowed.  So rather than fabricating some MIME headers out of
               the rfc822 headers, we'll just invalidate whatever mime_hdrs we
               previously had. */
            imf_hdrs_free(mime_hdrs);
            mime_hdrs = NULL;

        // handle multipart/* messages, which have nested subparts:
        }else if(ismultipart){
            const dstr_t boundary = get_mime_boundary(content_type, &missing);
            if(missing) break;
            bytes = get_multipart_index(
                bytes, boundary, sect_part->n, &missing
            );
            if(missing) break;

            // get the mime headers from the subpart
            imf_hdrs_free(mime_hdrs);
            PROP_GO(e, imf_hdrs_parse_sub(bytes, &mime_hdrs), fail);

            mime_content_type_free(content_type);
            if(isdigest){
                // multipart/digest changes the deault Content-Type
                content_type = read_mime_info(e,
                    mime_hdrs, d_message, d_rfc822
                );
            }else{
                content_type = read_mime_info(e, mime_hdrs, d_text, d_plain);
            }
            CHECK_GO(e, fail);
            UPDATE_CONTENT_TYPE(content_type);

            // remove the mime headers from the bytes we're considering
            bytes.start += mime_hdrs->bytes.len;
            bytes.len -= mime_hdrs->bytes.len;

            sect_part = sect_part->next;
            allow_sect_part_on_terminal = false;

        // terminal content types can only have a sect_part in one case...
        }else if(allow_sect_part_on_terminal){
            if(sect_part->n != 1){
                // request for other-than-1 subpart of a non-multipart message
                missing = true;
                break;
            }
            sect_part = sect_part->next;
            allow_sect_part_on_terminal = false;

        // ...otherwise terminal content types have no subparts at all
        }else{
            missing = true;
            break;
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
        imf_hdrs_free(STEAL(imf_hdrs_t, &mime_hdrs));
        imf_free(STEAL(imf_t, &heap_imf));
        imf = NULL;
    }

    *bytes_out = bytes;
    *mime_hdrs_out = mime_hdrs;
    *imf_out = imf;
    *heap_imf_out = heap_imf;
    mime_content_type_free(content_type);
    return missing;

fail:
    imf_hdrs_free(mime_hdrs);
    imf_free(heap_imf);
    mime_content_type_free(content_type);
    return false;
}
