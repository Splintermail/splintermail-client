#include <stdio.h>

#include <qwerrewq.h>
#include <qwerrewq.tab.h>
#include <common.h>
#include <logger.h>

// use RE2C to define YYMAXFILL
/*!max:re2c
*/

void yyerror(YYLTYPE *yyloc, parser_vars_t pv, char const *s){
    (void)yyloc;
    PFMT("parse error in %x at line %x, column %x:\n\n",
         FD(&pv.file->name),
         FU(yyloc->start_line),
         FU(yyloc->start_col));
    // print the line up to here
    size_t base = (size_t)(yyloc->dstr.data - pv.file->buf.data);
    size_t start = base - (yyloc->start_col - 1);
    size_t end = base + yyloc->dstr.len;
    dstr_t temp = dstr_sub(&pv.file->buf, start, end);
    PFMT("%x\n",FD(&temp));
    // print the pointer to the above line
    printf("%*s", (int)(yyloc->start_col - 1), "");
    for(size_t i = 0; i < yyloc->dstr.len; i++){ PFMT("^"); }
    PFMT("\n%x\n", FS(s));
}

int kvp_cmp(const void *p1, const void *p2){
    kvp_t *kvp1 = (kvp_t*)p1;
    kvp_t *kvp2 = (kvp_t*)p2;
    return dstr_cmp(&kvp1->key, &kvp2->key);
}

void kvp_rel(void *p){
    // TODO: release things here
    (void)p;
    LOG_ERROR("RELEASE A KVP\n");
    return;
}

char* expr_type_to_cstr(enum expr_type_t t){
    switch(t){
        case EXP_IF: return "EXP_IF";
        case EXP_SWITCH: return "EXP_SWITCH";
        case EXP_FOR: return "EXP_FOR";
        case EXP_FUNC_CALL: return "EXP_FUNC_CALL";
        case EXP_DOT: return "EXP_DOT";
        case EXP_PERCENT: return "EXP_PERCENT";
        case EXP_CARET: return "EXP_CARET";
        case EXP_PLUS: return "EXP_PLUS";
        case EXP_EQ: return "EXP_EQ";
        case EXP_MATEQ: return "EXP_MATEQ";
        case EXP_NOT: return "EXP_NOT";
        case EXP_AND: return "EXP_AND";
        case EXP_OR: return "EXP_OR";
        case EXP_EXPAND: return "EXP_EXPAND";
        case EXP_FUNC: return "EXP_FUNC";
        case EXP_DICT: return "EXP_DICT";
        case EXP_LIST: return "EXP_LIST";
        case EXP_STRING: return "EXP_STRING";
        case EXP_NUM: return "EXP_NUM";
        case EXP_TRUE: return "EXP_TRUE";
        case EXP_FALSE: return "EXP_FALSE";
        case EXP_PUKE: return "EXP_PUKE";
        case EXP_SKIP: return "EXP_SKIP";
        case EXP_NUL: return "EXP_NUL";
        case EXP_VAR: return "EXP_VAR";
        default: return "unknown type";
    }
}

char* eval_type_to_cstr(enum eval_type_t t){
    switch(t){
        case EVAL_UNEVALUATED: return "UNEVALUATED";
        case EVAL_STRING: return "STRING";
        case EVAL_FUNC: return "FUNC";
        case EVAL_DICT: return "DICT";
        case EVAL_EXPAND: return "EXPAND";
        case EVAL_LIST: return "LIST";
        case EVAL_NUM: return "NUM";
        case EVAL_BOOL: return "BOOL";
        case EVAL_PUKE: return "PUKE";
        case EVAL_SKIP: return "SKIP";
        case EVAL_NUL: return "NUL";
        default: return "unknown type";
    }
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


LIST_FUNCTIONS(file_t)

static derr_t file_new(file_t *f, dstr_t *fname){
    derr_t error;

    // copy the filename into the struct and null-terminate
    PROP( dstr_new(&f->name, fname->len + 1) );
    PROP_GO( dstr_copy(fname, &f->name), cu_name);
    PROP_GO( dstr_null_terminate(&f->name), cu_name);

    // read the file into memory
    PROP_GO( dstr_new(&f->buf, 4096), cu_name );
    PROP_GO( dstr_read_file(f->name.data, &f->buf), cu_buf);

    // avoid memory errors by padding YYMAXFILL null bytes at the end
    PROP_GO( dstr_grow(&f->buf, f->buf.len + YYMAXFILL), cu_buf);
    for(size_t i = 0; i < YYMAXFILL; i++)
        f->buf.data[f->buf.len + i] = '\0';

    return E_OK;

cu_buf:
    dstr_free(&f->buf);
cu_name:
    dstr_free(&f->name);
    return error;
}

static void file_free(file_t *f){
    dstr_free(&f->buf);
    dstr_free(&f->name);
}

static derr_t kvps_tostr(jsw_atree_t *kvps, bool prespace, dstr_t *out){
    jsw_atrav_t trav;
    for(kvp_t *kvp = jsw_atfirst(&trav, kvps); kvp; kvp = jsw_atnext(&trav)){
        if(prespace){
            PROP( FMT(out, " ") );
        }else{
            prespace = true;
        }
        PROP( FMT(out, "%x=", FD(&kvp->key)) );
        PROP( expr_tostr(kvp->value, out) );
    }
    return E_OK;
}

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
            PROP( kvps_tostr(&expr->u.for_call.kvps, true, out) );
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
            PROP( kvps_tostr(&expr->u.func_call.kvps, true, out) );
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
            PROP( kvps_tostr(&expr->u.func.kvps, false, out) );
            PROP( FMT(out, "-> ") );
            PROP( expr_tostr(expr->u.func.out, out) );
            PROP( FMT(out, "}") );
            break;
        case EXP_DICT:
            PROP( FMT(out, "<") );
            PROP( kvps_tostr(&expr->u.dict, false, out) );
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

derr_t expr_eval(expr_t *expr, jsw_atree_t *config){
    // shortcut to expr's eval_t member
    eval_t *eval = &expr->eval;
    expr_t *temp, *lhs, *rhs;
    list_t **end;
    // check if the expression is already evaluated
    if(eval->t != EVAL_UNEVALUATED) return E_OK;
    // otherwise, evaluate based on expression type
    switch(expr->t){
        case EXP_IF:
        case EXP_SWITCH:
        case EXP_FOR:
        case EXP_FUNC_CALL:
        // binary operators
        case EXP_DOT:
            // evaluate the lhs
            lhs = expr->u.binop.lhs;
            rhs = expr->u.binop.rhs;
            PROP( expr_eval(lhs, config) );
            // option 1: lhs evaluates to EVAL_DICT, rhs is of type EXP_VAR
            if(lhs->eval.t == EVAL_DICT && rhs->t == EXP_VAR){
                // dereference rhs variable using lhs dictionary
                temp = expr_deref(&lhs->eval.u.dict, rhs->u.dstr);
                if(!temp){
                    ORIG(E_VALUE, "dictionary missing key");
                }
                // evaluate that expression
                PROP( expr_eval(temp, config) );
                // steal its eval term
                *eval = temp->eval;
            }else{
                ORIG(E_VALUE, "illegal arguments to dot operator");
            }
            break;
        case EXP_PERCENT:
        case EXP_CARET:
            // evaluate the lhs and the rhs
            lhs = expr->u.binop.lhs;
            rhs = expr->u.binop.rhs;
            PROP( expr_eval(lhs, config) );
            PROP( expr_eval(rhs, config) );
            // lhs must be string, rhs must be list
            if(lhs->eval.t == EVAL_STRING && rhs->eval.t == EVAL_LIST){
                dstr_t *out = &expr->eval.u.dstr;
                PROP( dstr_new(out, 256) );
                expr->eval.t = EVAL_STRING;
                // build output
                dstr_t *joiner = &lhs->eval.u.dstr;
                for(list_t *l = rhs->eval.u.list; l; l = l->next){
                    if(l->expr->eval.t != EVAL_STRING){
                        ORIG(E_VALUE, "illegal non-string in caret rhs");
                    }
                    PROP( dstr_append(out, &l->expr->eval.u.dstr) );
                    if(l->next) PROP( dstr_append(out, joiner) );
                }
            }else{
                ORIG(E_VALUE, "illegal arguments to caret operator");
            }
            break;
        case EXP_PLUS:
            // evaluate the lhs and the rhs
            lhs = expr->u.binop.lhs;
            rhs = expr->u.binop.rhs;
            PROP( expr_eval(lhs, config) );
            PROP( expr_eval(rhs, config) );
            // option 1: lhs and rhs both evaluate to EVAL_STRING
            if(lhs->eval.t == EVAL_STRING && rhs->eval.t == EVAL_STRING){
                expr->eval.t = EVAL_STRING;
                // allocate the string we're about to build
                PROP( dstr_new(&expr->eval.u.dstr, lhs->eval.u.dstr.len
                                                   + rhs->eval.u.dstr.len) );
                // build the string
                PROP( dstr_append(&expr->eval.u.dstr, &lhs->eval.u.dstr) );
                PROP( dstr_append(&expr->eval.u.dstr, &rhs->eval.u.dstr) );
            }else{
                ORIG(E_VALUE, "illegal arguments to plus operator");
            }
            break;
        case EXP_EQ:
        case EXP_MATEQ:
        case EXP_AND:
        case EXP_OR:
            break;
        // unary operators
        case EXP_NOT:
        case EXP_EXPAND:
            // evaluate the arg
            PROP( expr_eval(expr->u.expr, config) );
            if(expr->u.expr->eval.t != EVAL_LIST){
                ORIG(E_VALUE, "illegal argument to expand operator");
            }
            expr->eval.t = EVAL_EXPAND;
            // store the argument's linked list
            expr->eval.u.list = expr->u.expr->eval.u.list;
            break;
        // literals
        case EXP_FUNC: break;
        case EXP_DICT:
            eval->t = EVAL_DICT;
            eval->u.dict = expr->u.dict;
            break;
        case EXP_LIST:
            eval->t = EVAL_LIST;
            eval->u.list = NULL;
            // keep track of the tail of the evaluated list
            end = &eval->u.list;
            // iterate through the expression list
            for(list_t *l = expr->u.list; l; l = l->next){
                // evaluate this list element
                PROP( expr_eval(l->expr, config) );
                switch(l->expr->eval.t){
                    case EVAL_SKIP: break;
                    case EVAL_EXPAND:
                        // if element is an expand operator, connect head...
                        *end = l->expr->eval.u.list;
                        // ... and then find the tail
                        while(*end) end = &(*end)->next;
                        break;
                    default:
                        // "normal" elements just get added
                        *end = malloc(sizeof(**end));
                        if(!(*end)) ORIG(E_NOMEM, "no mem");
                        (*end)->expr = l->expr;
                        (*end)->next = NULL;
                        end = &(*end)->next;
                }
            }
            break;
        case EXP_STRING:
            eval->t = EVAL_STRING;
            eval->u.dstr = expr->u.string.out;
            // don't call dstr_free on this
            eval->u.dstr.fixed_size = true;
            break;
        case EXP_NUM:
            eval->t = EVAL_NUM;
            eval->u.num = expr->u.num;
            break;
        case EXP_TRUE:
        case EXP_FALSE:
            eval->t = EVAL_BOOL;
            eval->u.boolean = (expr->t == EXP_TRUE);
            break;
        case EXP_PUKE: ORIG(E_VALUE, "puking like you asked"); break;
        case EXP_SKIP: eval->t = EVAL_SKIP; break;
        case EXP_NUL: eval->t = EVAL_NUL; break;
        case EXP_VAR:
            // get the expression referenced by this variable
            temp = expr_deref(config, expr->u.dstr);
            if(!temp){
                ORIG(E_VALUE, "dictionary missing key");
            }
            // evaluate that expression
            PROP( expr_eval(temp, config) );
            // steal it's eval term
            *eval = temp->eval;
            break;
    }
    return E_OK;
}

expr_t *expr_deref(jsw_atree_t *dict, dstr_t key){
    kvp_t kvp_key = { .key=key };
    kvp_t *kvp = jsw_afind(dict, (void*)&kvp_key);
    return (kvp ? kvp->value : NULL);
}

derr_t eval_tostr(eval_t *eval, dstr_t *out){
    switch(eval->t){
        case EVAL_STRING:
            PROP( dstr_append(out, &eval->u.dstr) );
            break;
        case EVAL_UNEVALUATED:
        case EVAL_FUNC:
        case EVAL_DICT:
        case EVAL_EXPAND:
        case EVAL_LIST:
        case EVAL_NUM:
        case EVAL_BOOL:
        case EVAL_PUKE:
        case EVAL_SKIP:
        case EVAL_NUL:
            PROP( FMT(out, "%x", FS(eval_type_to_cstr(eval->t))) );
            break;
    }
    return E_OK;
}

typedef struct {
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

static void scanner_init(scanner_t *s, file_t *f, bool config){
    // qmode starts as true for config files
    s->qmode = config;

    // init some values
    s->lineno = 1;
    s->line_start = 0;
    s->cursor = f->buf.data;
    s->limit = f->buf.data + f->buf.len + YYMAXFILL - 1;
}

#define TOKEN(_type) { \
    *type = _type; \
    /* get the end of the token substring */ \
    size_t end = (size_t)(eot - f->buf.data); \
    loc->dstr = dstr_sub(&f->buf, start, end); \
    /* get the end-location data */ \
    loc->end_line = s->lineno; \
    loc->end_col = end - s->line_start; \
    return E_OK; \
}

#define YYFILL(n){ \
    *done = true; \
    eot = f->buf.data + f->buf.len; \
    TOKEN(PARSE_END); \
}

static derr_t scan(scanner_t *s, file_t *f, loc_t *loc, int *type, bool *done){
    *done = false;

    size_t start;
    const char *eot; // "end of token"

restart:
    /* save start-of-token info */
    start = (size_t)(s->cursor - f->buf.data);
    loc->start_line = s->lineno;
    loc->start_col = start - s->line_start + 1;

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

        *           { eot = s->cursor; TOKEN(f->buf.data[start]); }
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
                      char pre = f->buf.data[start-1];
                      if(pre >= 'a' && pre <= 'z') TOKEN(F_PAREN);
                      if(pre >= 'A' && pre <= 'Z') TOKEN(F_PAREN);
                      if(pre >= '0' && pre <= '9') TOKEN(F_PAREN);
                      if(pre == '_') TOKEN(F_PAREN);
                      TOKEN(OOO_PAREN); }
        nl @eot     { s->lineno++;
                      s->line_start = (size_t)(eot - f->buf.data);
                      goto restart; }
    */

dqstring_mode:

    /*!re2c
        *           { ORIG(E_VALUE, "failure in scanner") }
        dqstr       { goto dqstring_mode; }
        "\"" @eot   { TOKEN(DQSTRING); }
        nl @eot     { s->lineno++;
                      s->line_start = (size_t)(eot - f->buf.data);
                      goto dqstring_mode; }
    */

sqstring_mode:

    /*!re2c
        *           { ORIG(E_VALUE, "failure in scanner") }
        sqstr       { goto sqstring_mode; }
        "'" @eot    { TOKEN(SQSTRING); }
        nl          { s->lineno++;
                      s->line_start = (size_t)(s->cursor - f->buf.data);
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
                      s->line_start = (size_t)(eot - f->buf.data);
                      goto restart; }
    */

    return E_OK;
}

static derr_t do_qwerrewq(file_t *f, bool config, expr_list_t **result){
    derr_t error;

    // prep the scanner
    scanner_t s;
    scanner_init(&s, f, config);

    // prep the parser
    yypstate *yyps = yypstate_new();
    if(yyps == NULL){
        ORIG(E_NOMEM, "unable to allocate yypstate");
    }

    parser_vars_t pv = { .file = f, .result = result };

    // scan tokens
    bool done;
    do{
        int type;
        loc_t loc = (loc_t){0};
        PROP_GO( scan(&s, f, &loc, &type, &done), cu_parser);
        // PFMT("%x    (%x)\n", FD(&loc.dstr), FS(toktyp_to_str(type)));

        // allocate/initialize yyloc pointer
        YYLTYPE *yyloc = malloc(sizeof(*yyloc));
        if(!yyloc) ORIG_GO(E_NOMEM, "unable to allocate yyloc", cu_parser);
        *yyloc = loc;
        // allocate/initialize yylval pointer
        union expr_u *yylval = malloc(sizeof(*yylval));
        if(!yylval){
            free(yyloc);
            ORIG_GO(E_NOMEM, "unable to allocate yylval", cu_parser);
        }
        yylval->dstr = loc.dstr;
        // push token through parser
        int yyret = yypush_parse(yyps, type, (void*)yylval, yyloc, pv);
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
    return error;
}

int main(int argc, char **argv){
    derr_t error;

    logger_add_fileptr(LOG_LVL_DEBUG, stderr);

    // make sure we got a config file argument
    if(argc < 2){
        fprintf(stderr, "must specify a config file\n");
        return 1;
    }
    char *config_file = argv[1];

    // list of files and filenames
    LIST(file_t) files;
    PROP_GO( LIST_NEW(file_t, &files, 4), fail);

    // start by loading the first argument
    dstr_t conf_fname;
    DSTR_WRAP(conf_fname, config_file, strlen(config_file), true);

    file_t temp;
    PROP_GO( file_new(&temp, &conf_fname), cu_files);

    // add config file to the registry of files
    PROP_GO( LIST_APPEND(file_t, &files, temp), cu_temp);
    // no need to clean up temp, it will get cleaned with the rest of the files

    // do the parsing of config file
    expr_list_t *conf_result;
    PROP_GO( do_qwerrewq(&files.data[0], true, &conf_result), cu_files);

    // print the config file to show it was parsed correctly
    for(expr_list_t *expr = conf_result; expr; expr = expr->next){
        DSTR_VAR(out, 4096);
        expr_tostr(expr->expr, &out);
        PFMT("%x\n", FD(&out));
    }

    // make sure that the config file is the right format
    if(conf_result->next != NULL || conf_result->expr->t != EXP_DICT){
        ORIG_GO(E_VALUE, "config file must be a single dict object", cu_files);
    }

    jsw_atree_t config = conf_result->expr->u.dict;

    PFMT("---------------------------------------\n");

    // parse the regular file
    if(argc < 3){
        fprintf(stderr, "must specify an input file\n");
        return 1;
    }
    char *input_file = argv[2];

    dstr_t input_fname;
    DSTR_WRAP(input_fname, input_file, strlen(input_file), true);

    PROP_GO( file_new(&temp, &input_fname), cu_files);

    PROP_GO( LIST_APPEND(file_t, &files, temp), cu_temp);

    expr_list_t *result;
    PROP_GO( do_qwerrewq(&files.data[1], true, &result), cu_files);

    // print the config file to show it was parsed correctly
    for(expr_list_t *el = result; el; el = el->next){
        DSTR_VAR(out, 4096);
        PROP_GO( expr_eval(el->expr, &config), cu_files);
        expr_tostr(el->expr, &out);
        FMT(&out, " | '");
        eval_tostr(&el->expr->eval, &out);
        PFMT("%x'\n", FD(&out));
    }

    // if we exit normally, there's no need to do cu_temp
    goto cu_files;

cu_temp:
    file_free(&temp);

cu_files:
    for(size_t i = 0; i < files.len; i++){
        file_free(&files.data[i]);
    }
    LIST_FREE(file_t, &files);
fail:
    return error != E_OK;
}
