#include <stdlib.h>

#include <libimap/libimap.h>


static void imf_hdr_arg_free(imf_hdr_type_e type, imf_hdr_arg_u arg){
    switch(type){
        case IMF_HDR_UNSTRUCT: /* nothing to free */ (void)arg; break;
    }
}


imf_hdr_t *imf_hdr_new(derr_t *e, dstr_t bytes, dstr_t name,
        imf_hdr_type_e type, imf_hdr_arg_u arg){
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

static void imf_body_arg_free(imf_body_type_e type, imf_body_arg_u arg){
    switch(type){
        case IMF_BODY_UNSTRUCT: /* nothing to free */ (void)arg; break;
    }
}

imf_body_t *imf_body_new(derr_t *e, dstr_t bytes, imf_body_type_e type,
        imf_body_arg_u arg){
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

imf_t *imf_new(derr_t *e, dstr_t bytes, dstr_t hdr_bytes, imf_hdr_t *hdr,
        imf_body_t *body){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, imf_t, imf, fail);

    imf->bytes = bytes;
    imf->hdr_bytes = hdr_bytes;
    imf->hdr = hdr;
    imf->body = body;

    return imf;

fail:
    imf_hdr_free(hdr);
    imf_body_free(body);
    return NULL;
}

void imf_free(imf_t *imf){
    if(!imf) return;
    imf_hdr_free(imf->hdr);
    imf_body_free(imf->body);
    free(imf);
}

// scanner

derr_t imf_scanner_init(imf_scanner_t *scanner, const dstr_t *bytes){
    derr_t e = E_OK;

    // point to the bytes buffer
    scanner->bytes = bytes;

    // start position at beginning of buffer
    scanner->start = scanner->bytes->data;

    return e;
}

void imf_scanner_free(imf_scanner_t *scanner){
    // currently nothing to free, but I don't know if it'll stay like that
    *scanner = (imf_scanner_t){0};
}

dstr_t imf_get_scannable(imf_scanner_t *scanner){
    // this is always safe because "start" is always within the bytes buffer
    size_t offset = (size_t)(scanner->start - scanner->bytes->data);
    return dstr_sub(scanner->bytes, offset, 0);
}

#include <imf.tab.h>

// rfc 5322 (IMF) and rfc 5234 (ABNF)

/*  notes:
      - for expediency, the hdrname allows any printable character
        except SP, including UTF8. RFC 5322 does not permit this.

      - also for expediency, the visual character (VCHAR) specified
        in ABNF used to define the IMF "unstructured" type has been
        expanded to allow 8-byte values

      - this scanner uses the EOF feature of re2c which does not seem to play
        nicely with multiple blocks, so each block has to be in a different
        function.  That means re2c runs with the --reusable option.  It also
        means this scanner uses the default API instead of the general API.
        UPDATE: the default API has a end-of-buffer-overrun bug, so I've filed
        a bug and I'm using the generic API
*/

#define  YYPEEK()         idx >= scanner->bytes->len ? '\0' : scanner->bytes->data[idx]
#define  YYSKIP()         ++idx >= scanner->bytes->len ? '\0' : scanner->bytes->data[idx]
// #define  YYBACKUP()       YYMARKER = YYCURSOR
// #define  YYBACKUPCTX()    YYCTXMARKER = YYCURSOR
// #define  YYRESTORE()      YYCURSOR = YYMARKER
// #define  YYRESTORECTX()   YYCURSOR = YYCTXMARKER
// #define  YYRESTORERAG(t)  YYCURSOR = t
#define  YYLESSTHAN(n)    scanner->bytes->len - idx < n
// #define  YYSTAGP(t)       t = YYCURSOR
// #define  YYSTAGPD(t)      t = YYCURSOR - 1
// #define  YYSTAGN(t)       t = NULL

/*!rules:re2c
    eol             = "\r"?"\n";
    ws              = [ \x09]+;
    hdrname         = [^\x00-\x20:]+;
    unstruct        = [^\x00-\x19]+;
    body            = [\x00-\xff]+;
*/

// Pass errors to bison
#define INVALID_TOKEN_ERROR { *type = INVALID_TOKEN; goto done; }

static void scan_done(imf_scanner_t *scanner, const char *YYCURSOR,
        dstr_t *token_out){
    size_t start_offset, end_offset;

    // mark everything done until here
    const char *old_start = scanner->start;
    scanner->start = YYCURSOR;

    // get the token bounds
    // this is safe; start and old_start are always within the bytes buffer
    start_offset = (size_t)(old_start - scanner->bytes->data);
    end_offset = (size_t)(scanner->start - scanner->bytes->data);
    *token_out = dstr_sub(scanner->bytes, start_offset, end_offset);
}

static derr_t scan_hdr(imf_scanner_t *scanner, dstr_t *token_out, int *type){
    derr_t e = E_OK;

    size_t idx = (size_t)(scanner->start - scanner->bytes->data);

    /*!use:re2c
        re2c:flags:input = custom;
        re2c:define:YYCTYPE = char;
        re2c:yyfill:enable = 0;
        re2c:eof = 0;

        *               { INVALID_TOKEN_ERROR; }
        $               { *type = DONE; goto done; }
        eol             { *type = EOL; goto done; }
        hdrname         { *type = HDRNAME; goto done; }
        ":"             { *type = *scanner->start; goto done; }
        ws              { *type = WS; goto done; }
    */

done:
    scan_done(scanner, &scanner->bytes->data[idx], token_out);
    return e;
}

static derr_t scan_unstruct(imf_scanner_t *scanner, dstr_t *token_out,
        int *type){
    derr_t e = E_OK;

    size_t idx = (size_t)(scanner->start - scanner->bytes->data);

    /*!use:re2c
        re2c:flags:input = custom;
        re2c:define:YYCTYPE = char;
        re2c:yyfill:enable = 0;
        re2c:eof = 0;

        *               { INVALID_TOKEN_ERROR; }
        $               { *type = DONE; goto done; }
        unstruct        { *type = UNSTRUCT; goto done; }
        eol             { *type = EOL; goto done; }
    */

done:
    scan_done(scanner, &scanner->bytes->data[idx], token_out);
    return e;
}

static derr_t scan_body(imf_scanner_t *scanner, dstr_t *token_out,
        int *type){
    derr_t e = E_OK;

    size_t idx = (size_t)(scanner->start - scanner->bytes->data);

    /*!use:re2c
        re2c:flags:input = custom;
        re2c:define:YYCTYPE = char;
        re2c:yyfill:enable = 0;
        re2c:eof = 0;

        $               { *type = DONE; goto done; }
        body            { *type  = BODY; goto done; }
    */

done:
    scan_done(scanner, &scanner->bytes->data[idx], token_out);
    return e;
}

derr_t imf_scan(imf_scanner_t *scanner, imf_scan_mode_t mode,
        dstr_t *token_out, int *type){
    derr_t e = E_OK;
    switch(mode){
        case IMF_SCAN_HDR:
            PROP(&e, scan_hdr(scanner, token_out, type) );
            return e;
        case IMF_SCAN_UNSTRUCT:
            PROP(&e, scan_unstruct(scanner, token_out, type) );
            return e;
        case IMF_SCAN_BODY:
            PROP(&e, scan_body(scanner, token_out, type) );
            return e;
    }
    ORIG(&e, E_INTERNAL, "invalid imf_scan_mode");
}

// parser

void imfyyerror(dstr_t *imfyyloc, imf_parser_t *parser, char const *s){
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
    *parser = (imf_parser_t){0};
}

derr_t imf_parse(const dstr_t *msg, imf_t **out){
    derr_t e = E_OK;

    *out = NULL;

    imf_scanner_t scanner;
    PROP(&e, imf_scanner_init(&scanner, msg) );

    imf_parser_t parser;
    PROP_GO(&e, imf_parser_init(&parser, &scanner), cu_scanner);

    int token_type = 0;
    do {
        dstr_t token;
        PROP_GO(&e, imf_scan(&scanner, parser.scan_mode, &token, &token_type),
                cu_parser);

        parser.token = &token;
        int yyret = imfyypush_parse(parser.imfyyps, token_type, NULL, &token,
                &parser);
        switch(yyret){
            case 0:
                // YYACCEPT: parsing completed successfully
                break;
            case YYPUSH_MORE:
                // parsing incomplete, keep going
                break;
            case 1:
                // YYABORT or syntax invalid
                TRACE(&e, "invalid input on token '%x' of type %x\n",
                        FD(&token), FI(token_type));
                ORIG_GO(&e, E_PARAM, "invalid input", cu_parser);
            case 2:
                // memory exhaustion
                ORIG_GO(&e, E_NOMEM, "invalid input", cu_parser);
            default:
                // this should never happen
                TRACE(&e, "imfyypush_parse() returned %x\n", FI(yyret));
                ORIG_GO(&e, E_INTERNAL,
                        "unexpected imfyypush_parse() return value", cu_parser);
        }
        PROP_VAR_GO(&e, &parser.error, cu_parser);
    } while(token_type != DONE);

    *out = STEAL(imf_t, &parser.imf);

cu_parser:
    imf_parser_free(&parser);
cu_scanner:
    imf_scanner_free(&scanner);
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
