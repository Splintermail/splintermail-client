#include <stdio.h>

#include <qwerrewq.h>
#include <common.h>
#include <logger.h>

// use RE2C to define YYMAXFILL
/*!max:re2c
*/

void yyerror(YYLTYPE *yyloc, void *parser, char const *s){
    (void)yyloc;
    (void)parser;
    PFMT("ERROR at %x:%x: %x\n", FI(yyloc->first_line),
                                 FI(yyloc->first_column + 1),
                                 FS(s));
}

char *toktyp_to_str(int type){
    if(type < 256) return "";
    switch(type){
        case START: return "START";
        case END: return "END";
        case PUKE: return "PUKE";
        case NUL: return "NUL";
        case SKIP: return "SKIP";
        case TRUE: return "TRUE";
        case FALSE: return "FALSE";
        case NUM: return "NUM";
        case SQSTRING: return "SQSTRING";
        case DQSTRING: return "DQSTRING";
        case VAR: return "VAR";
        case IF: return "IF";
        case SWITCH: return "SWITCH";
        case FOR: return "FOR";
        case ARROW: return "ARROW";
        case EQ: return "EQ";
        case MATEQ: return "MATEQ";
        case AND: return "AND";
        case OR: return "OR";
        case F_PAREN: return "F_PAREN";
        case OOO_PAREN: return "OOO_PAREN";
        default: return "unknown";
    }
}

#define BINOP2STR(a,op,b);

derr_t expr_tostr(expr_t *expr, dstr_t *out){
    char *op = NULL;
    switch(expr->t){
        case EXP_IF:
            PROP( FMT(out, "if(") );
            for(expr_pair_list_t *p = expr->u.if_call.tests; p; p = p->next){
                PROP( expr_tostr(p->lhs, out) );
                PROP( FMT(out, ":") );
                PROP( expr_tostr(p->rhs, out) );
                PROP( FMT(out, " ") );
            }
            PROP( expr_tostr(expr->u.if_call.else_expr, out) );
            PROP( FMT(out, ")") );
            break;
        case EXP_SWITCH:
            PROP( FMT(out, "switch(") );
            PROP( expr_tostr(expr->u.switch_call.value, out) );
            PROP( FMT(out, "){") );
            for(expr_pair_list_t *p = expr->u.switch_call.tests; p; p = p->next){
                PROP( expr_tostr(p->lhs, out) );
                PROP( FMT(out, ":") );
                PROP( expr_tostr(p->rhs, out) );
                PROP( FMT(out, " ") );
            }
            PROP( expr_tostr(expr->u.switch_call.default_expr, out) );
            PROP( FMT(out, "}") );
            break;
        case EXP_FOR:
            PROP( FMT(out, "[") );
            PROP( expr_tostr(expr->u.for_call.out, out) );
            PROP( FMT(out, " FOR") );
            for(kvp_t *kvp = expr->u.for_call.kvps; kvp; kvp = kvp->next){
                PROP( FMT(out, " %x=", FD(&kvp->key)) );
                PROP( expr_tostr(kvp->value, out) );
            }
            PROP( FMT(out, "]") );
            break;
        case EXP_FUNC_CALL:
            PROP( FMT(out, "(") );
            PROP( expr_tostr(expr->u.func_call.func, out) );
            PROP( FMT(out, " call ") );
            for(expr_list_t *p = expr->u.func_call.params; p; p = p->next){
                if(p != expr->u.func_call.params) PROP( FMT(out, " ") );
                PROP( expr_tostr(p->expr, out) );
            }
            for(kvp_t *kvp = expr->u.func_call.kvps; kvp; kvp = kvp->next){
                PROP( FMT(out, " %x=", FD(&kvp->key)) );
                PROP( expr_tostr(kvp->value, out) );
            }
            PROP( FMT(out, ")") );
            break;
        // binary operators
        case EXP_DOT:       if(!op) op = ".";
        case EXP_PERCENT:   if(!op) op = "%";
        case EXP_CARET:     if(!op) op = "^";
        case EXP_PLUS:      if(!op) op = "+";
        case EXP_EQ:        if(!op) op = "==";
        case EXP_MATEQ:     if(!op) op = "=~";
        case EXP_AND:       if(!op) op = "&&";
        case EXP_OR:        if(!op) op = "||";
            PROP( FMT(out, "(") );
            PROP( expr_tostr(expr->u.binop.lhs, out) );
            PROP( FMT(out, " %x ", FS(op)) );
            PROP( expr_tostr(expr->u.binop.rhs, out) );
            PROP( FMT(out, ")") );
            break;
        // unary operators
        case EXP_NOT: if(!op) op = "!";
        case EXP_EXPAND: if(!op) op = "*";
            PROP( FMT(out, "*(") );
            PROP( expr_tostr(expr->u.expr, out) );
            PROP( FMT(out, ")") );
            break;
        case EXP_FUNC:
            PROP( FMT(out, "{") );
            for(dstr_list_t *dl = expr->u.func.vars; dl; dl = dl->next){
                PROP( FMT(out, "%x ", FD(&dl->dstr)) );
            }
            for(kvp_t *kvp = expr->u.func.kvps; kvp; kvp = kvp->next){
                PROP( FMT(out, "%x=", FD(&kvp->key)) );
                PROP( expr_tostr(kvp->value, out) );
                PROP( FMT(out, " ") );
            }
            PROP( FMT(out, "-> ") );
            PROP( expr_tostr(expr->u.func.out, out) );
            PROP( FMT(out, "}") );
            break;
        case EXP_DICT:
            PROP( FMT(out, "<") );
            for(kvp_t *kvp = expr->u.kvp; kvp; kvp = kvp->next){
                PROP( FMT(out, "%x=", FD(&kvp->key)) );
                PROP( expr_tostr(kvp->value, out) );
                if(kvp->next) PROP( FMT(out, " ") );
            }
            PROP( FMT(out, ">") );
            break;
        case EXP_LIST:
            PROP( FMT(out, "[") );
            for(list_t *elem = expr->u.list; elem; elem = elem->next){
                PROP( expr_tostr(elem->expr, out) );
                if(elem->next) PROP( FMT(out, " ") );
            }
            PROP( FMT(out, "]") );
            break;
        case EXP_STRING: PROP( FMT(out, "%x", FD(&expr->u.string.raw)) ); break;
        case EXP_NUM: PROP( FMT(out, "%x", FI(expr->u.num)) ); break;
        case EXP_TRUE: PROP( FMT(out, "TRUE") ); break;
        case EXP_FALSE: PROP( FMT(out, "FALSE") ); break;
        case EXP_PUKE: PROP( FMT(out, "PUKE") ); break;
        case EXP_SKIP: PROP( FMT(out, "SKIP") ); break;
        case EXP_NUL: PROP( FMT(out, "NULL") ); break;
        case EXP_VAR: PROP( FMT(out, "VAR(%x)", FD(&expr->u.dstr)) ); break;
    }
    return E_OK;
}

typedef struct {
    dstr_t fname;
    dstr_t buf;
    // betwen QWER-REWQ tags, or not?
    bool qmode;
    // location tracking information
    size_t lineno;
    size_t line_start;
    // re2c scanner state
    const char* cursor;
    const char* marker;
    const char* limit;
    char yych;
} scanner_t;

static derr_t scanner_new(scanner_t *s, const dstr_t *fname, bool config){
    derr_t error;
    // copy the filename into the struct and null-terminate
    PROP( dstr_new(&s->fname, fname->len + 1) );
    PROP_GO( dstr_copy(fname, &s->fname), cu_fname);
    PROP_GO( dstr_null_terminate(&s->fname), cu_fname);

    // read the file into memory
    PROP_GO( dstr_new(&s->buf, 4096), cu_fname );
    PROP_GO( dstr_read_file(s->fname.data, &s->buf), cu_buf);

    // avoid memory errors by padding YYMAXFILL null bytes at the end
    PROP_GO( dstr_grow(&s->buf, s->buf.len + YYMAXFILL), cu_buf);
    for(size_t i = 0; i < YYMAXFILL; i++)
        s->buf.data[s->buf.len + i] = '\0';

    // qmode starts as true for config files
    s->qmode = config;

    // init some values
    s->lineno = 1;
    s->cursor = s->buf.data;
    s->limit = s->buf.data + s->buf.len + YYMAXFILL - 1;

    return E_OK;

cu_buf:
    dstr_free(&s->buf);
cu_fname:
    dstr_free(&s->fname);
    return error;
}

static void scanner_free(scanner_t *s){
    dstr_free(&s->fname);
    dstr_free(&s->buf);
}

typedef struct {
    dstr_t dstr;
    int type;
    size_t start_line;
    size_t start_col;
    size_t end_line;
    size_t end_col;
} token_t;

#define TOKEN(_type) { \
    token->type = _type; \
    /* get the end of the token substring */ \
    size_t end = (size_t)(eot - s->buf.data); \
    token->dstr = dstr_sub(&s->buf, start, end); \
    /* get the end-location data */ \
    token->end_line = s->lineno; \
    token->end_col = end - s->line_start; \
    return E_OK; \
}

#define YYFILL(n){ \
    *done = true; \
    eot = s->buf.data + s->buf.len; TOKEN(PARSE_END); \
    token->type = PARSE_END; \
    return E_OK; \
}

static derr_t scan(scanner_t *s, token_t *token, bool *done){
    *done = false;

    size_t start;
    const char *eot; // "end of token"

restart:
    /* save start-of-token info */
    start = (size_t)(s->cursor - s->buf.data);
    token->start_line = s->lineno;
    token->start_col = start - s->line_start;

    if(!s->qmode) goto skip_mode;

    /*!stags:re2c format = 'const char *@@;'; */
    /*!re2c
        re2c:define:YYCTYPE = char;
        re2c:define:YYCURSOR = 's->cursor';
        re2c:define:YYMARKER = 's->marker';
        re2c:define:YYLIMIT = 's->limit';

        ignore = [^Q\n\x00]+;
        ignoreQ = "Q";
        nullbyte = "\x00";
        start = "QWER";
        end = "REWQ";

        ws = [ \r\t]+;
        nl = [\n];
        puke = "puke";
        null = "null";
        skip = "skip";
        true = "true";
        false = "false";
        dqstr = ([^"\\\n]|"\\\"")+;
        sqstr = ([^'\\\n]|"\\'")+;
        var = [a-zA-Z_][a-zA-Z_0-9]*;
        num = [0-9]+;
        comment = "#"[^\n]*;
        paren = "(";

        *           { eot = s->cursor; TOKEN(s->buf.data[start]); }
        end @eot    { s->qmode = false; TOKEN(END); }
        puke @eot   { TOKEN(PUKE); }
        null @eot   { TOKEN(NUL); }
        skip @eot   { TOKEN(SKIP); }
        true @eot   { TOKEN(TRUE); }
        false @eot  { TOKEN(FALSE); }
        num @eot    { TOKEN(NUM); }
        "\""        { goto dqstring_mode; }
        "'"         { goto sqstring_mode; }
        comment     { goto restart; }
        ws          { goto restart; }
        "==" @eot   { TOKEN(EQ); }
        "=~" @eot   { TOKEN(MATEQ); }
        "&&" @eot   { TOKEN(AND); }
        "||" @eot   { TOKEN(OR); }
        "->" @eot   { TOKEN(ARROW); }
        "if" @eot   { TOKEN(IF); }
        "switch" @eot{TOKEN(SWITCH); }
        "for" @eot  { TOKEN(FOR); }
        var @eot    { TOKEN(VAR); }
        paren       { eot = s->cursor;
                      if(start == 0) TOKEN(OOO_PAREN);
                      char pre = s->buf.data[start-1];
                      if(pre >= 'a' && pre <= 'z') TOKEN(F_PAREN);
                      if(pre >= 'A' && pre <= 'Z') TOKEN(F_PAREN);
                      if(pre >= '0' && pre <= '9') TOKEN(F_PAREN);
                      if(pre == '_') TOKEN(F_PAREN);
                      TOKEN(OOO_PAREN); }
        nl @eot     { s->lineno++;
                      s->line_start = (size_t)(eot - s->buf.data);
                      goto restart; }
    */

dqstring_mode:

    /*!re2c
        *           { ORIG(E_VALUE, "failure in scanner") }
        dqstr       { goto dqstring_mode; }
        "\"" @eot   { TOKEN(DQSTRING); }
        nl @eot     { s->lineno++;
                      s->line_start = (size_t)(eot - s->buf.data);
                      goto dqstring_mode; }
    */

sqstring_mode:

    /*!re2c
        *           { ORIG(E_VALUE, "failure in scanner") }
        sqstr       { goto sqstring_mode; }
        "'" @eot    { TOKEN(SQSTRING); }
        nl          { s->lineno++;
                      s->line_start = (size_t)(s->cursor - s->buf.data);
                      goto sqstring_mode; }
    */

skip_mode:
    /*!re2c
        *           { ORIG(E_VALUE, "failure in scanner") }
        start @eot  { s->qmode = true; TOKEN(START); }
        ignore      { goto restart; }
        ignoreQ     { goto restart; }
        nullbyte    { goto restart; }
        nl @eot     { s->lineno++;
                      s->line_start = (size_t)(eot - s->buf.data);
                      goto restart; }
    */

    return E_OK;
}

static derr_t do_qwerrewq(char *filename){
    derr_t error;

    // wrap filename in a dstr_t
    dstr_t fname;
    DSTR_WRAP(fname, filename, strlen(filename), true);

    // prep the scanner
    scanner_t s;
    PROP( scanner_new(&s, &fname, true) );

    // prep the parser
    yypstate *yyps = yypstate_new();
    if(yyps == NULL){
        ORIG_GO(E_NOMEM, "unable to allocate yypstate", cu_scanner);
    }

    // scan tokens
    bool done;
    do{
        token_t token = (token_t){0};
        PROP_GO( scan(&s, &token, &done), cu_parser);
        // PFMT("%x    (%x)\n", FD(&token.dstr), FS(toktyp_to_str(token.type)));

        // allocate/initialize yyloc pointer
        YYLTYPE *yyloc = malloc(sizeof(*yyloc));
        if(!yyloc) ORIG_GO(E_NOMEM, "unable to allocate yyloc", cu_parser);
        *yyloc = (YYLTYPE){.first_line = (int)token.start_line,
                           .first_column = (int)token.start_col,
                           .last_line = (int)token.end_line,
                           .last_column = (int)token.end_col};
        // allocate/initialize yylval pointer
        union expr_u *yylval = malloc(sizeof(*yylval));
        if(!yylval){
            free(yyloc);
            ORIG_GO(E_NOMEM, "unable to allocate yylval", cu_parser);
        }
        yylval->dstr = token.dstr;
        // push token through parser
        int yyret = yypush_parse(yyps, token.type, (void*)yylval, yyloc, NULL);
        switch(yyret){
            case 0:             // parsing completed successful; parser is reset
                break;
            case YYPUSH_MORE:   // parsing incomplete, but valid; parser not reset
                continue;
            case 1:             // invalid; parser is reset
                ORIG_GO(E_PARAM, "invalid input", cu_parser);
            case 2:             // memory exhaustion; parser is reset
                ORIG_GO(E_NOMEM, "memory exhaustion during yypush_parse", cu_parser);
            default:            // this should never happen
                LOG_ERROR("yypush_parse() returned %x\n", FI(yyret));
                ORIG_GO(E_INTERNAL, "unexpected yypush_parse() return value", cu_parser);
        }
    }while(!done);

cu_parser:
    yypstate_delete(yyps);
cu_scanner:
    scanner_free(&s);
    return error;
}

int main(int argc, char **argv){
    // make sure we got a file argument
    if(argc < 2){
        fprintf(stderr, "must specify a file\n");
        return 1;
    }

    derr_t error = do_qwerrewq(argv[1]);
    return error != E_OK;
}
