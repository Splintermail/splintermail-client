#include <stdlib.h>

#include <libimap/libimap.h>


static void imf_hdr_arg_free(imf_hdr_type_e type, imf_hdr_arg_u arg){
    switch(type){
        case IMF_HDR_UNSTRUCT: /* nothing to free */ (void)arg; break;
    }
}


imf_hdr_t *imf_hdr_new(
    derr_t *e,
    dstr_off_t bytes,
    dstr_off_t name,
    imf_hdr_type_e type,
    imf_hdr_arg_u arg
){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, imf_hdr_t, hdr, fail);

    hdr->bytes = bytes;
    hdr->name = name;
    hdr->type = type;
    hdr->arg = arg;

    return hdr;

fail:
    imf_hdr_arg_free(type, arg);
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
    imf_hdr_arg_free(hdr->type, hdr->arg);
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

static void imf_body_arg_free(imf_body_type_e type, imf_body_arg_u arg){
    switch(type){
        case IMF_BODY_UNSTRUCT: /* nothing to free */ (void)arg; break;
    }
}

imf_body_t *imf_body_new(
    derr_t *e,
    dstr_off_t bytes,
    imf_body_type_e type,
    imf_body_arg_u arg
){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, imf_body_t, body, fail);

    body->bytes = bytes;
    body->type = type;
    body->arg = arg;

    return body;

fail:
    imf_body_arg_free(type, arg);
    return NULL;
}

void imf_body_free(imf_body_t *body){
    if(!body) return;
    imf_body_arg_free(body->type, body->arg);
    free(body);
}

imf_t *imf_new(
    derr_t *e,
    dstr_off_t bytes,
    imf_hdrs_t *hdrs,
    imf_body_t *body
){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, imf_t, imf, fail);

    imf->bytes = bytes;
    imf->hdrs = hdrs;
    imf->body = body;

    return imf;

fail:
    imf_hdrs_free(hdrs);
    imf_body_free(body);
    return NULL;
}

void imf_free(imf_t *imf){
    if(!imf) return;
    imf_hdrs_free(imf->hdrs);
    imf_body_free(imf->body);
    free(imf);
}

// scanner

derr_t imf_scanner_init(
    imf_scanner_t *scanner,
    // bytes can be reallocated but it must not otherwise change
    const dstr_t *bytes,
    // if read_fn is not NULL, it should extend *bytes and return amnt_read
    derr_t (*read_fn)(void*, size_t*),
    void *read_fn_data
){
    derr_t e = E_OK;

    *scanner = (imf_scanner_t){
        .bytes = bytes,
        .start_idx = 0,
        .read_fn = read_fn,
        .read_fn_data = read_fn_data,
    };

    return e;
}

void imf_scanner_free(imf_scanner_t *scanner){
    *scanner = (imf_scanner_t){0};
}

dstr_t imf_get_scannable(imf_scanner_t *scanner){
    return dstr_sub(scanner->bytes, scanner->start_idx, 0);
}

#include <imf.tab.h>

// rfc 5322 (IMF) and rfc 5234 (ABNF)

/*  notes:
      - for expediency, the hdrname allows any printable character
        except SP, including UTF8. RFC 5322 does not permit this.

      - also for expediency, the visual character (VCHAR) specified
        in ABNF used to define the IMF "unstructured" type has been
        expanded to allow 8-byte values

      - old versions of this used the EOF feature (with '\0' as the eof, which
        seemed dangerous to actually distribute to users), and it tried using
        multiple blocks at some point, and it tried using --reusable at some
        point, and it tried the non-generic API at some point.

        It has now been rewritten to be much simpler and using none of the
        fancy features, but rather based on server/from.c, which was compatible
        with older builds of re2c
*/

static char peek(const dstr_t *text, size_t idx){
    return idx < text->len ? text->data[idx] : '\0';
}

// returns 0 on success, nonzero on eof or error
static int fill(derr_t *e, imf_scanner_t *scanner){
    if(scanner->read_fn == NULL){
        // no read_fn provided
        return -1;
    }

    size_t amnt_read;
    IF_PROP(e, scanner->read_fn(scanner->read_fn_data, &amnt_read) ){
        return -1;
    }

    return amnt_read == 0;
}

derr_t imf_scan(
    imf_scanner_t *scanner,
    imf_scan_mode_t mode,
    dstr_off_t *token_out,
    int *type
){
    size_t idx = scanner->start_idx;

    /*!re2c
        re2c:flags:input = custom;
        re2c:define:YYCTYPE = int;

        eol             = "\r"?"\n";
        ws              = [ \x09]+;
        hdrname         = [^\x00-\x20:]+;
        unstruct        = [^\x00-\x19]+;
        body            = [\x00-\xff]+;
    */

    #define YYPEEK() peek(scanner->bytes, idx)
    #define YYSKIP() peek(scanner->bytes, ++idx)
    #define YYFILL() fill(&e2, scanner)
    // this is a buffer limit check, not an EOF check
    #define YYLESSTHAN(n) scanner->bytes->len - idx < n
    // Pass errors to bison
    #define INVALID_TOKEN_ERROR { *type = INVALID_TOKEN; goto done; }

    derr_t e = E_OK;
    derr_t e2 = E_OK;
    switch(mode){
        case IMF_SCAN_HDR:
            /*!re2c
                re2c:flags:input = custom;
                re2c:define:YYCTYPE = char;

                // it's ok for \0 to appear in the input;
                // it just triggers a length check
                re2c:eof = 0;

                *               { INVALID_TOKEN_ERROR; }
                $               { *type = DONE; goto done; }
                eol             { *type = EOL; goto done; }
                hdrname         { *type = HDRNAME; goto done; }
                ":"             { *type = scanner->bytes->data[scanner->start_idx]; goto done; }
                ws              { *type = WS; goto done; }
            */
            break;

        case IMF_SCAN_UNSTRUCT:
            /*!re2c
                *               { INVALID_TOKEN_ERROR; }
                $               { *type = DONE; goto done; }
                unstruct        { *type = UNSTRUCT; goto done; }
                eol             { *type = EOL; goto done; }
            */
            break;

        case IMF_SCAN_BODY:
            /*!re2c
                $               { *type = DONE; goto done; }
                body            { *type  = BODY; goto done; }
            */
            break;

        default:
            ORIG(&e, E_INTERNAL, "invalid imf_scan_mode");
    }

done:
    // check for read errors before committing index updates to scanner
    PROP_VAR(&e, &e2);

    // get the token bounds
    *token_out = (dstr_off_t){
        .buf = scanner->bytes,
        .start = scanner->start_idx,
        .len = idx - scanner->start_idx,

    };

    // mark everything done until here
    scanner->start_idx = idx;

    return e;
}

// parser

void imfyyerror(dstr_off_t *imfyyloc, imf_parser_t *parser, char const *s){
    (void)imfyyloc;
    (void)parser;
    LOG_ERROR("ERROR: %x\n", FS(s));
}

static derr_t imf_parser_init(imf_parser_t *parser, imf_scanner_t *scanner){
    derr_t e = E_OK;

    *parser = (imf_parser_t){
        .error = E_OK,
        .scan_mode = IMF_SCAN_HDR,
        .scanner = scanner,
    };

    // init the bison parser
    parser->imfyyps = imfyypstate_new();
    if(parser->imfyyps == NULL){
        ORIG(&e, E_NOMEM, "unable to allocate imfyypstate");
    }

    return e;
}

static void imf_parser_free(imf_parser_t *parser){
    imfyypstate_delete(parser->imfyyps);
    DROP_VAR(&parser->error);
    imf_free(parser->imf);
    imf_hdrs_free(parser->hdrs);
    *parser = (imf_parser_t){0};
}

derr_t imf_reader_new(
    imf_reader_t **out,
    const dstr_t *msg,
    derr_t (*read_fn)(void*, size_t*),
    void *read_fn_data
){
    derr_t e = E_OK;
    *out = NULL;

    imf_reader_t *r = DMALLOC_STRUCT_PTR(&e, r);
    CHECK(&e);

    PROP_GO(&e,
        imf_scanner_init(&r->scanner, msg, read_fn, read_fn_data),
    fail);

    PROP_GO(&e, imf_parser_init(&r->parser, &r->scanner), fail);

    *out = r;

    return e;

fail:
    imf_reader_free(&r);
    return e;
}

void imf_reader_free(imf_reader_t **old){
    imf_reader_t *r = *old;
    if(!r) return;
    imf_parser_free(&r->parser);
    imf_scanner_free(&r->scanner);
    free(r);
    *old = NULL;
}

static derr_t _imf_reader_parse(imf_reader_t *r, bool just_headers){
    derr_t e = E_OK;

    int token_type = 0;
    do {
        dstr_off_t token;
        dstr_t token_dstr;
        PROP(&e,
            imf_scan(&r->scanner, r->parser.scan_mode, &token, &token_type)
        );

        r->parser.token = &token;
        int yyret = imfyypush_parse(
            r->parser.imfyyps, token_type, NULL, &token, &r->parser
        );
        switch(yyret){
            case 0:
                // YYACCEPT: parsing completed successfully
                break;
            case YYPUSH_MORE:
                // parsing incomplete, keep going
                break;
            case 1:
                // YYABORT or syntax invalid
                token_dstr = dstr_from_off(token);
                TRACE(&e, "invalid input on token '%x' of type %x\n",
                        FD(&token_dstr), FI(token_type));
                ORIG(&e, E_PARAM, "invalid input");
            case 2:
                // memory exhaustion
                ORIG(&e, E_NOMEM, "invalid input");
            default:
                // this should never happen
                TRACE(&e, "imfyypush_parse() returned %x\n", FI(yyret));
                ORIG(&e, E_INTERNAL,
                        "unexpected imfyypush_parse() return value");
        }
        PROP_VAR(&e, &r->parser.error);
        // if all we wanted was headers, we might exit early.
        if(just_headers && r->parser.hdrs) break;
    } while(token_type != DONE);

    return e;
}

derr_t imf_reader_parse_headers(imf_reader_t *r, imf_hdrs_t **out){
    derr_t e = E_OK;
    *out = NULL;

    PROP(&e, _imf_reader_parse(r, true) );

    *out = STEAL(imf_hdrs_t, &r->parser.hdrs);

    return e;
}

// *in should be exactly the *out from parse_headers()
derr_t imf_reader_parse_body(imf_reader_t *r, imf_hdrs_t **in, imf_t **out){
    derr_t e = E_OK;
    *out = NULL;

    r->parser.hdrs = STEAL(imf_hdrs_t, in);

    PROP(&e, _imf_reader_parse(r, false) );

    *out = STEAL(imf_t, &r->parser.imf);

    return e;
}

// parses a static buffer
derr_t imf_parse(const dstr_t *msg, imf_t **out){
    imf_reader_t *r = NULL;
    imf_hdrs_t *hdrs = NULL;
    imf_t *imf = NULL;

    derr_t e = E_OK;
    *out = NULL;

    PROP_GO(&e, imf_reader_new(&r, msg, NULL, NULL), cu);
    PROP_GO(&e, imf_reader_parse_headers(r, &hdrs), cu);
    PROP_GO(&e, imf_reader_parse_body(r, &hdrs, &imf), cu);

    *out = STEAL(imf_t, &imf);

cu:
    imf_reader_free(&r);
    imf_hdrs_free(hdrs);
    imf_free(imf);

    return e;
}

imf_t *imf_parse_builder(derr_t *e, const ie_dstr_t *msg){
    if(is_error(*e)) goto fail;

    imf_t *imf;
    PROP_GO(e, imf_parse(&msg->dstr, &imf), fail);

    return imf;

fail:
    return NULL;
}
