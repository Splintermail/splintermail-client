#include <stdlib.h>
#include <stdio.h>

#include <server/from.h>

// scanner

derr_t from_scanner_init(from_scanner_t *scanner, const dstr_t *bytes){
    derr_t e = E_OK;

    // point to the bytes buffer
    scanner->bytes = bytes;

    // start position at beginning of buffer
    scanner->start = scanner->bytes->data;

    return e;
}

void from_scanner_free(from_scanner_t *scanner){
    // currently nothing to free, but I don't know if it'll stay like that
    *scanner = (from_scanner_t){0};
}

dstr_t from_get_scannable(from_scanner_t *scanner){
    // this is always safe because "start" is always within the bytes buffer
    size_t offset = (size_t)(scanner->start - scanner->bytes->data);
    return dstr_sub(scanner->bytes, offset, 0);
}

#include <from.tab.h>

// rfc 5322 (IMF) and rfc 5234 (ABNF) and rfc 5335 (utf8 updates to IMF)

/* notes:
    - this literally is only capable of parsing the From field.
    - utf8 is only loosely validated according to the patterns in rfc5335
    - this uses YYCTYPE=int to encode end-of-file as the return of YYPEEK
      (this is to work around the lack of the eof feature with re2c v1.1.1)
*/

static int peek(const dstr_t *text, size_t idx){
    return idx < text->len ? text->data[idx] : -1;
}


derr_t from_scan(from_scanner_t *scanner, bool one_byte_mode, dstr_t *token_out, int *type){
    derr_t e = E_OK;

    size_t idx = (size_t)(scanner->start - scanner->bytes->data);
    size_t mark;

    #define  YYPEEK()         peek(scanner->bytes, idx)
    #define  YYSKIP()         peek(scanner->bytes, ++idx)
    #define  YYBACKUP()       mark = idx;
    #define  YYRESTORE()      idx = mark;
    // #define  YYBACKUPCTX()    YYCTXMARKER = YYCURSOR
    // #define  YYRESTORECTX()   YYCURSOR = YYCTXMARKER
    // #define  YYRESTORERAG(t)  YYCURSOR = t
    // #define  YYLESSTHAN(n)    scanner->bytes->len - idx < n
    // #define  YYSTAGP(t)       t = YYCURSOR
    // #define  YYSTAGPD(t)      t = YYCURSOR - 1
    // #define  YYSTAGN(t)       t = NULL

    /*!re2c
        re2c:flags:input = custom;
        re2c:define:YYCTYPE = int;
        re2c:yyfill:enable = 0;

        nil                 = "\x00";
        ws                  = [ \x09]+;
        anytext             = [a-zA-Z0-9!#$%&'*+\-/=?^_`{|}~]+;
        obs_no_ws_ctl       = [\x01-\x08\x0B\x0C\x0E-\x1F\x7F]+;
        fold                = ([ \x09]*"\r"?"\n"[ \x09]+)+;

        utf8_2              = ([\xc2-\xdf] [\x80-\xbf])+;
        utf8_3a             = ("\xe0" [\xa0-\xbf] [\x80-\xbf])+;
        utf8_3b             = ([\xe1-\xec] [\x80-\xbf]{2})+;
        utf8_3c             = ("\xed" [\x80-\x9f] [\x80-\xbf])+;
        utf8_3d             = ([\xee-\xef] [\x80-\xbf]{2})+;
        utf8_4a             = ("\xf0" [\x90-\xbf] [\x80-\xbf]{2})+;
        utf8_4b             = ([\xf1-\xf3] [\x80-\xbf]{3})+;
        utf8_4c             = ("\xf4" [\x80-\x8f] [\x80-\xbf]{2})+;

        one_utf8_2          = [\xc2-\xdf] [\x80-\xbf];
        one_utf8_3a         = "\xe0" [\xa0-\xbf] [\x80-\xbf];
        one_utf8_3b         = [\xe1-\xec] [\x80-\xbf]{2};
        one_utf8_3c         = "\xed" [\x80-\x9f] [\x80-\xbf];
        one_utf8_3d         = [\xee-\xef] [\x80-\xbf]{2};
        one_utf8_4a         = "\xf0" [\x90-\xbf] [\x80-\xbf]{2};
        one_utf8_4b         = [\xf1-\xf3] [\x80-\xbf]{3};
        one_utf8_4c         = "\xf4" [\x80-\x8f] [\x80-\xbf]{2};
    */

    if(one_byte_mode){
    /*!re2c
        *                   { *type = yych; goto done; }
        nil                 { *type = NIL; goto done; }

        one_utf8_2          { *type = UTF8; goto done; }
        one_utf8_3a         { *type = UTF8; goto done; }
        one_utf8_3b         { *type = UTF8; goto done; }
        one_utf8_3c         { *type = UTF8; goto done; }
        one_utf8_3d         { *type = UTF8; goto done; }
        one_utf8_4a         { *type = UTF8; goto done; }
        one_utf8_4b         { *type = UTF8; goto done; }
        one_utf8_4c         { *type = UTF8; goto done; }
    */
    }else{
    /*!re2c
        *                   { *type = yych; goto done; }
        nil                 { *type = NIL; goto done; }
        ws                  { *type = WS; goto done; }
        fold                { *type = FOLD; goto done; }
        anytext             { *type = ANYTEXT; goto done; }
        obs_no_ws_ctl       { *type = OBS_NO_WS_CTRL; goto done; }

        utf8_2              { *type = UTF8; goto done; }
        utf8_3a             { *type = UTF8; goto done; }
        utf8_3b             { *type = UTF8; goto done; }
        utf8_3c             { *type = UTF8; goto done; }
        utf8_3d             { *type = UTF8; goto done; }
        utf8_4a             { *type = UTF8; goto done; }
        utf8_4b             { *type = UTF8; goto done; }
        utf8_4c             { *type = UTF8; goto done; }
    */
    }


    size_t start_offset, end_offset;
    const char *old_start;
done:
    // catch EOF condition since debian buster's re2c v1.1.1 has no eof support
    if(*type == -1) *type = DONE;

    // mark everything done until here
    old_start = scanner->start;
    scanner->start = &scanner->bytes->data[idx];

    // get the token bounds
    // this is safe; start and old_start are always within the bytes buffer
    start_offset = (size_t)(old_start - scanner->bytes->data);
    end_offset = (size_t)(scanner->start - scanner->bytes->data);
    *token_out = dstr_sub(scanner->bytes, start_offset, end_offset);
    return e;
}


// parser


dstr_t token_extend(dstr_t start, dstr_t end){
    size_t len = (size_t)(end.data - start.data) + end.len;
    return (dstr_t){
        .data = start.data,
        .len = len,
        .size = len,
        .fixed_size = true,
    };
}

void append_raw(derr_t *e, dstr_t *out, const dstr_t raw){
    if(is_error(*e)) return;
    IF_PROP(e, dstr_append(out, &raw) ){};
}

void append_space(derr_t *e, dstr_t *out){
    if(is_error(*e)) return;
    IF_PROP(e, dstr_append(out, &DSTR_LIT(" ")) ){};
}

void fromyyerror(dstr_t *fromyyloc, from_parser_t *parser, char const *s){
    (void)parser;
    LOG_ERROR("ERROR: %x at token: %x of input: %x\n",
            FS(s), FD_DBG(fromyyloc), FD_DBG(parser->in));
}

static lstr_t *lstr_new(derr_t *e, dstr_t text){
    if(is_error(*e)) goto fail;
    lstr_t *out = malloc(sizeof(*out));
    if(!out) ORIG_GO(e, E_NOMEM, "nomem", fail);
    *out = (lstr_t){ .text=text };

    link_init(&out->link);
    return out;

fail:
    return NULL;
}

static lstr_t *lstr_free(lstr_t *lstr){
    if(!lstr) return NULL;
    link_t *link;
    while((link = link_list_pop_first(&lstr->link))){
        lstr_t *unlinked = CONTAINER_OF(link, lstr_t, link);
        // recursion will only be one layer deep
        lstr_free(unlinked);
    }
    free(lstr);
    return NULL;
}

lstr_t *lstr_concat(derr_t *e, lstr_t *a, lstr_t *b){
    if(is_error(*e)){
        lstr_free(b);
        return lstr_free(a);
    }
    LOG_DEBUG("concat: ");
    lstr_print(a);
    LOG_DEBUG(" + ");
    lstr_print(b);

    link_t *la = &a->link;
    link_t *lb = &b->link;
    link_t *la_end = la->prev;
    link_t *lb_end = lb->prev;

    la_end->next = lb;
    lb->prev = la_end;
    lb_end->next = la;
    la->prev = lb_end;

    LOG_DEBUG(" = ");
    lstr_print(a);
    LOG_DEBUG("\n");

    return a;
}

lstr_t *lstr_set_text(derr_t *e, lstr_t *lstr, dstr_t text){
    if(is_error(*e)){
        return lstr_free(lstr);
    }

    lstr->text = text;
    return lstr;
}

void lstr_print(lstr_t *lstr){
    lstr_t *head = lstr;
    do {
        LOG_DEBUG("%x", FD(&lstr->text));
        link_t *next = lstr->link.next;
        lstr = CONTAINER_OF(next, lstr_t, link);
    } while (lstr != head);
}

derr_t lstr_dump(dstr_t *out, lstr_t *lstr){
    derr_t e = E_OK;

    lstr_t *head = lstr;
    do {
        PROP(&e, FMT(out, "%x", FD(&lstr->text)) );
        link_t *next = lstr->link.next;
        lstr = CONTAINER_OF(next, lstr_t, link);
    } while (lstr != head);

    return e;
}

// return a spare lstr_t if possible, else malloc
static lstr_t *parser_link_new(
    derr_t *e, from_parser_t *parser, dstr_t text
){
    if(is_error(*e)) return NULL;
    link_t *link = link_list_pop_first(&parser->empty_links);
    if(!link){
        return lstr_new(e, text);
    }
    lstr_t *out = CONTAINER_OF(link, lstr_t, link);
    out->text = text;
    return out;
}

// save returned lstrs_t as spares for later
static lstr_t *parser_link_return(
    from_parser_t *parser, lstr_t *lstr
){
    link_t *head = &parser->empty_links;
    link_t *old_last = head->prev;
    link_t *first_in = &lstr->link;
    link_t *last_in = first_in->prev;

    head->prev = last_in;
    last_in->next = head;
    first_in->prev = old_last;
    old_last->next = first_in;

    return NULL;
}

static derr_t from_parser_init(
    from_parser_t *parser, from_scanner_t *scanner, const dstr_t *in
){
    derr_t e = E_OK;

    *parser = (from_parser_t){
        .error = E_OK,
        .scanner = scanner,
        .link_new = parser_link_new,
        .link_return = parser_link_return,
        .in = in,
    };

    link_init(&parser->empty_links);

    // init the bison parser
    parser->fromyyps = fromyypstate_new();
    if(parser->fromyyps == NULL){
        ORIG(&e, E_NOMEM, "unable to allocate fromyypstate");
    }

    return e;
}

static void from_parser_free(from_parser_t *parser){
    fromyypstate_delete(parser->fromyyps);
    link_t *link;
    while((link = link_list_pop_first(&parser->empty_links))){
        lstr_t *lstr =
            CONTAINER_OF(link, lstr_t, link);
        lstr_free(lstr);
    }
    parser->out = lstr_free(parser->out);
    DROP_VAR(&parser->error);
    *parser = (from_parser_t){0};
}


derr_t from_parse(const dstr_t *msg, lstr_t **out){
    derr_t e = E_OK;
    *out = NULL;

    from_scanner_t scanner;
    PROP(&e, from_scanner_init(&scanner, msg) );

    from_parser_t parser;
    PROP_GO(&e, from_parser_init(&parser, &scanner, msg), cu_scanner);

    int token_type = 0;
    do {
        dstr_t token;
        PROP_GO(&e, from_scan(&scanner, parser.one_byte_mode, &token, &token_type),
                cu_parser);

        parser.token = &token;
        int yyret = fromyypush_parse(parser.fromyyps, token_type, NULL, &token,
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
                        FD_DBG(&token), FI(token_type));
                ORIG_GO(&e, E_PARAM, "invalid input", cu_parser);
            case 2:
                // memory exhaustion
                ORIG_GO(&e, E_NOMEM, "invalid input", cu_parser);
            default:
                // this should never happen
                TRACE(&e, "fromyypush_parse() returned %x\n", FI(yyret));
                ORIG_GO(&e, E_INTERNAL,
                        "unexpected fromyypush_parse() return value", cu_parser);
        }
        PROP_VAR_GO(&e, &parser.error, cu_parser);
    } while(token_type != DONE);

    *out = parser.out;
    parser.out = NULL;

    // TODO: actually return a result

cu_parser:
    from_parser_free(&parser);
cu_scanner:
    from_scanner_free(&scanner);
    return e;
}

static derr_t parse_stdin(void){
    derr_t e = E_OK;
    dstr_t in;
    dstr_new(&in, 4096);
    while(!feof(stdin)){
        PROP_GO(&e, dstr_fread(stdin, &in, 2048, NULL), cu_in);
    }
    fclose(stdin);

    dstr_t out;
    dstr_new(&out, in.len);

    lstr_t *first_mailbox;

    PROP_GO(&e, from_parse(&in, &first_mailbox), cu_out);

    PROP_GO(&e, lstr_dump(&out, first_mailbox), cu_mailbox);

    // write the first mailbox to stdout
    PROP_GO(&e, FFMT(stdout, NULL, "%x", FD(&out)), cu_mailbox);

cu_mailbox:
    lstr_free(first_mailbox);
cu_out:
    dstr_free(&out);
cu_in:
    dstr_free(&in);
    return e;
}

int main(void){
    derr_t e = E_OK;
    PROP_GO(&e, logger_add_fileptr(LOG_LVL_ERROR, stderr), fail);
    PROP_GO(&e, parse_stdin(), fail);
    return 0;

fail:
    CATCH(e, E_PARAM){
        // syntax error
        DROP_VAR(&e);
        return 1;
    }
    // any other error
    DUMP(e);
    DROP_VAR(&e);
    return 2;
}
