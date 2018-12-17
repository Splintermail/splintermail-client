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
            PFMT("out of memory\n"); \
            YYABORT; \
        } \
        *out = (expr_t){.t = EXP_ ## type, .u = {.member = val}}; \
    }

    #define BINOP(out, type, a, b){ \
        out = malloc(sizeof(*out)); \
        if(!out){ \
            PFMT("out of memory\n"); \
            YYABORT; \
        } \
        *out = (expr_t){.t = EXP_ ## type, .u = {.binop = {.lhs = a, .rhs = b}}}; \
    }

    #define EXP_STRING(_out, _raw){ \
        _out = malloc(sizeof(*_out)); \
        if(!_out){ \
            PFMT("out of memory\n"); \
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

%left OR
%left AND
%right '!'
%left EQ MATEQ
%left '+' '/' '\\'
%left '%' '^'
%left '.'
%left F_PAREN

%%

input: expr ';' {DSTR_VAR(out, 4096);
                 expr_tostr($expr, &out);
                 PFMT("%x\n", FD(&out)); };

expr: OOO_PAREN expr[e] ')'                     { $$ = $e; }
/**/| IF F_PAREN expr expr_pairs_1 expr ')'     { EXPR($$, PUKE, ign, NULL); }
/**/| SWITCH F_PAREN expr expr_pairs_1 expr ')' { EXPR($$, PUKE, ign, NULL); }
/**/| '[' expr FOR kvps_1 ']'                   { EXPR($$, PUKE, ign, NULL); }
/**/| expr F_PAREN exprs_1 kvps_0 ')'           { EXPR($$, PUKE, ign, NULL); }
    | expr[a] '.' expr[b]                       { BINOP($$, DOT, $a, $b); }
    | expr[a] '%' expr[b]                       { BINOP($$, PERCENT, $a, $b); }
    | expr[a] '^' expr[b]                       { BINOP($$, CARET, $a, $b); }
    | expr[a] '+' expr[b]                       { BINOP($$, PLUS, $a, $b); }
    | expr[a] EQ expr[b]                        { BINOP($$, EQ, $a, $b); }
    | expr[a] MATEQ expr[b]                     { BINOP($$, MATEQ, $a, $b); }
    | '!' expr[e]                               { EXPR($$, NOT, expr, $e); }
    | expr[a] AND expr[b]                       { BINOP($$, AND, $a, $b); }
    | expr[a] OR expr[b]                        { BINOP($$, OR, $a, $b); }
/**/| func[f]                                   { EXPR($$, PUKE, ign, NULL); }
/**/| dict[d]                                   { EXPR($$, PUKE, ign, NULL); }
/**/| list[l]                                   { EXPR($$, PUKE, ign, NULL); }
    | DQSTRING[s]                               { EXP_STRING($$, $s); }
    | SQSTRING[s]                               { EXP_STRING($$, $s); }
/**/| NUM[n]                                    { EXPR($$, PUKE, ign, NULL); }
    | TRUE                                      { EXPR($$, TRUE, ign, NULL); }
    | FALSE                                     { EXPR($$, FALSE, ign, NULL); }
    | PUKE                                      { EXPR($$, PUKE, ign, NULL); }
    | NUL                                       { EXPR($$, NUL, ign, NULL); }
    | VAR[v]                                    { EXPR($$, VAR, dstr, $v); }
;

exprs_1: expr
       | exprs_1 expr
;

/* compound expressions */
expr_pair: expr ':' expr;

expr_pairs_1: expr_pair
            | expr_pairs_1 expr_pair
;

kvp: VAR '=' expr;

kvps_0: %empty
      | kvps_1
;

kvps_1: kvp
      | kvps_1 kvp
;

/* literals */

func: '{' vars_1 kvps_0 ARROW expr '}';

dict: '<' kvps_0 '>';

list: '[' list_elems_0 ']';

list_elem: expr | expand | SKIP;

list_elems_0: %empty
            | list_elems_1
;

list_elems_1: list_elem
            | list_elems_1 list_elem
;

expand: '*' expr;

vars_1: VAR
      | vars_1 VAR
;

