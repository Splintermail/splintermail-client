%{
    #include <stdio.h>
    #include <qwerrewq.h>
    #include <common.h>
    #include <logger.h>

    #define DOCATCH(cmd){ \
        derr_t error = cmd; \
        CATCH(E_ANY){ \
            YYABORT; \
        } \
    }

    #define EXPR(out, type, member, val){ \
        out = malloc(sizeof(*out)); \
        if(!out){ \
            FFMT(stderr, NULL, "out of memory\n"); \
            YYABORT; \
        } \
        *out = (expr_t){.t = EXP_ ## type, .u = {.member = val}}; \
    }

    #define BINOP(out, type, a, b){ \
        out = malloc(sizeof(*out)); \
        if(!out){ \
            FFMT(stderr, NULL, "out of memory\n"); \
            YYABORT; \
        } \
        *out = (expr_t){.t = EXP_ ## type, .u = {.binop = {.lhs = a, .rhs = b}}}; \
    }

    #define EXP_STRING(_out, _raw){ \
        _out = malloc(sizeof(*_out)); \
        if(!_out){ \
            FFMT(stderr, NULL, "out of memory\n"); \
            YYABORT; \
        } \
        _out->t = EXP_STRING; \
        _out->u.string.raw = _raw; \
        /* remove escapes from string */ \
        DOCATCH( dstr_new(&_out->u.string.out, _raw.len - 2) ); \
        LIST_STATIC(dstr_t, find, DSTR_LIT("\\\\"), DSTR_LIT("\\\""), \
                                  DSTR_LIT("\\'"), DSTR_LIT("\\n"), \
                                  DSTR_LIT("\\r"), DSTR_LIT("\\t")); \
        \
        LIST_STATIC(dstr_t, repl, DSTR_LIT("\\"), DSTR_LIT("\""), \
                                  DSTR_LIT("'"), DSTR_LIT("\n"), \
                                  DSTR_LIT("\\r"), DSTR_LIT("\\t")); \
        dstr_t in = dstr_sub(&_raw, 1, _raw.len - 1); \
        DOCATCH( dstr_recode(&in, &_out->u.string.out, &find, &repl, false) ); \
    }

    #define EXP_NUM(_out, _n){ \
        _out = malloc(sizeof(*_out)); \
        if(!_out){ \
            FFMT(stderr, NULL, "out of memory\n"); \
            YYABORT; \
        } \
        _out->t = EXP_NUM; \
        long long temp; \
        DOCATCH( dstr_toll(&_n, &temp, 0) ); \
        _out->u.num = temp; \
    }

    #define KVP(_out, _k, _v){ \
        _out = malloc(sizeof(*_out)); \
        if(!_out){ \
            FFMT(stderr, NULL, "out of memory\n"); \
            YYABORT; \
        } \
        _out->key = _k; \
        _out->value = _v; \
        _out->next = NULL; \
    }

    #define KVP_ADD(_out, _list, _elem){ \
        _out = _list; \
        kvp_t **end = &_out; \
        while(*end) end = &(*end)->next; \
        *end = _elem; \
    }

    #define LIST_ELEM(_out, _expr){ \
        _out = malloc(sizeof(*_out)); \
        if(!_out){ \
            FFMT(stderr, NULL, "out of memory\n"); \
            YYABORT; \
        } \
        _out->expr = _expr; \
        _out->next = NULL; \
    }

    #define LIST_ADD(_out, _list, _elem){ \
        _out = _list; \
        list_t **end = &_out; \
        while(*end) end = &(*end)->next; \
        *end = _elem; \
    }

    #define DSTR_LIST(_out, _dstr){ \
        _out = malloc(sizeof(*_out)); \
        if(!_out){ \
            FFMT(stderr, NULL, "out of memory\n"); \
            YYABORT; \
        } \
        _out->dstr = _dstr; \
        _out->next = NULL; \
    }

    #define DSTR_LIST_ADD(_out, _list, _dstr){ \
        _out = _list; \
        dstr_list_t **end = &_out; \
        while(*end) end = &(*end)->next; \
        *end = _dstr; \
    }

    #define EXPR_LIST(_out, _expr){ \
        _out = malloc(sizeof(*_out)); \
        if(!_out){ \
            FFMT(stderr, NULL, "out of memory\n"); \
            YYABORT; \
        } \
        _out->expr = _expr; \
        _out->next = NULL; \
    }

    #define EXPR_LIST_ADD(_out, _list, _expr){ \
        _out = _list; \
        expr_list_t **end = &_out; \
        while(*end) end = &(*end)->next; \
        *end = _expr; \
    }

    #define EXPR_PAIR(_out, _lhs, _rhs){ \
        _out = malloc(sizeof(*_out)); \
        if(!_out){ \
            FFMT(stderr, NULL, "out of memory\n"); \
            YYABORT; \
        } \
        _out->lhs = _lhs; \
        _out->rhs = _rhs; \
        _out->next = NULL; \
    }

    #define EXPR_PAIR_ADD(_out, _list, _ep){ \
        _out = _list; \
        expr_pair_list_t **end = &_out; \
        while(*end) end = &(*end)->next; \
        *end = _ep; \
    }
%}

/* build a union of semantic types */
%define api.value.type {semtyp_t}
/* reentrant */
%define api.pure full
/* push parser */
%define api.push-pull push
/* create a user-data pointer in the api */
%parse-param { void *parser }
/* keep track of location info */
%locations
/* compile error on parser generator conflicts */
%expect 0

%token PARSE_END

/* start and end tags, ("QWER" and "REWQ" for now) */
%token START
%token END

%token PUKE
%token NUL
%token SKIP
%token TRUE
%token FALSE
%token <dstr> NUM
%token <dstr> SQSTRING
%token <dstr> DQSTRING
%token <dstr> VAR
%token IF
%token SWITCH
%token FOR
%token ARROW /* "->" */
%token EQ /* "==" */
%token MATEQ /* "=~" */
%token AND /* "&&" */
%token OR /* "||" */
/* paren preceeded by a variable (no whitespace), must be function call */
%token F_PAREN
/* any other paren, must be order-of-operations */
%token OOO_PAREN

%type <expr> expr

%type <expr_list> exprs_1

%type <expr_pair_list> expr_pair
%type <expr_pair_list> expr_pairs_1

%type <if_call> if

%type <switch_call> switch

%type <for_call> for

%type <func_call> func_call

%type <func> func

%type <kvp> dict
%type <kvp> kvp
%type <kvp> kvps_0
%type <kvp> kvps_1

%type <list> list
%type <list> list_elem
%type <list> list_elems_0
%type <list> list_elems_1

%type <dstr_list> vars_1

%left OR
%left AND
%right '!'
%left EQ MATEQ
%left '+'
%left '%' '^'
%left F_PAREN
%left '.'

%%

input: exprs_1[l] PARSE_END
       { for(expr_list_t *expr = $l; expr; expr = expr->next){
             DSTR_VAR(out, 4096);
             expr_tostr(expr->expr, &out);
             PFMT("%x\n", FD(&out));
         }
       };

expr: OOO_PAREN expr[e] ')'     { $$ = $e; }
    | if[f]                     { EXPR($$, IF, if_call, $f); }
    | switch[s]                 { EXPR($$, SWITCH, switch_call, $s); }
    | for[f]                    { EXPR($$, FOR, for_call, $f); }
    | func_call[f]              { EXPR($$, FUNC_CALL, func_call, $f); }
    | expr[a] '.' expr[b]       { BINOP($$, DOT, $a, $b); }
    | expr[a] '%' expr[b]       { BINOP($$, PERCENT, $a, $b); }
    | expr[a] '^' expr[b]       { BINOP($$, CARET, $a, $b); }
    | expr[a] '+' expr[b]       { BINOP($$, PLUS, $a, $b); }
    | expr[a] EQ expr[b]        { BINOP($$, EQ, $a, $b); }
    | expr[a] MATEQ expr[b]     { BINOP($$, MATEQ, $a, $b); }
    | '!' expr[e]               { EXPR($$, NOT, expr, $e); }
    | expr[a] AND expr[b]       { BINOP($$, AND, $a, $b); }
    | expr[a] OR expr[b]        { BINOP($$, OR, $a, $b); }
    | func[f]                   { EXPR($$, FUNC, func, $f); }
    | dict[d]                   { EXPR($$, DICT, kvp, $d); }
    | list[l]                   { EXPR($$, LIST, list, $l); }
    | DQSTRING[s]               { EXP_STRING($$, $s); }
    | SQSTRING[s]               { EXP_STRING($$, $s); }
    | NUM[n]                    { EXP_NUM($$, $n); }
    | TRUE                      { EXPR($$, TRUE, ign, NULL); }
    | FALSE                     { EXPR($$, FALSE, ign, NULL); }
    | PUKE                      { EXPR($$, PUKE, ign, NULL); }
    | NUL                       { EXPR($$, NUL, ign, NULL); }
    | VAR[v]                    { EXPR($$, VAR, dstr, $v); }
;

if: IF F_PAREN expr_pairs_1[t] expr[e] ')'
    { $$ = (if_t){.tests = $t, .else_expr = $e}; };

switch: SWITCH F_PAREN expr[v] expr_pairs_1[t] expr[d] ')'
        { $$ = (switch_t){.value = $v, .tests = $t, .default_expr = $d}; };

for: '[' expr[o] FOR kvps_1[k] ']'
     { $$ = (for_t){.out = $o, .kvps = $k}; };

func_call: expr[f] F_PAREN exprs_1[p] kvps_0[k] ')'
           { $$ = (func_call_t){.func = $f, .params = $p, .kvps = $k}; };

exprs_1: expr[e]                { EXPR_LIST($$, $e); }
       | exprs_1[l] expr[e]     { expr_list_t *temp;
                                  EXPR_LIST(temp, $e);
                                  EXPR_LIST_ADD($$, $l, temp); }
;

expr_pair: expr[l] ':' expr[r]  { EXPR_PAIR($$, $l, $r); };

expr_pairs_1: expr_pair
            | expr_pairs_1[l] expr_pair[p] { EXPR_PAIR_ADD($$, $l, $p); }
;

kvp: VAR[k] '=' expr[v]         { KVP($$, $k, $v); };

kvps_0: %empty                  { $$ = NULL; }
      | kvps_1
;

kvps_1: kvp
      | kvps_1[l] kvp[k]        { KVP_ADD($$, $l, $k); }
;

func: '{' vars_1[v] kvps_0[k] ARROW expr[o] '}'
      { $$ = (func_t){.vars=$v, .kvps=$k, .out=$o}; };

dict: '<' kvps_0[l] '>'         { $$ = $l; };

list: '[' list_elems_0[l] ']'   { $$ = $l; };

list_elem: expr[e]              { LIST_ELEM($$, $e); }
         | '*' expr[e]          { expr_t *expr;
                                  EXPR(expr, EXPAND, expr, $e);
                                  LIST_ELEM($$, expr); }
         | SKIP                 { expr_t *expr;
                                  EXPR(expr, SKIP, ign, NULL);
                                  LIST_ELEM($$, expr); }
;

list_elems_0: %empty        { $$ = NULL; }
            | list_elems_1
;

list_elems_1: list_elem
            | list_elems_1[l] list_elem[e]  { LIST_ADD($$, $l, $e); }
;

vars_1: VAR[v]              { DSTR_LIST($$, $v); }
      | vars_1[l] VAR[v]    { dstr_list_t *temp;
                              DSTR_LIST(temp, $v);
                              DSTR_LIST_ADD($$, $l, temp); }
;

