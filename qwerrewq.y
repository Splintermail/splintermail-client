%{
    #include <stdio.h>
    #include <qwerrewq.h>
%}

/* this defines the type of yylval, which is the semantic value of a token */
%define api.value.type {void *}
/* reentrant */
%define api.pure full
/* push parser */
%define api.push-pull push
/* create a user-data pointer in the api */
%parse-param { void *parser }
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
%token NUM
%token SQSTRING
%token DQSTRING
%token VAR
%token IF
%token SWITCH
%token FOR
%token ARROW /* "->" */
%token EQUALS /* "==" */
%token MATCH_EQUALS /* "=~" */
%token AND /* "&&" */
%token OR /* "||" */
/* paren preceeded by a variable (no whitespace), must be function call */
%token F_PAREN
/* any other paren, must be order-of-operations */
%token OOO_PAREN

%left OR
%left AND
%right '!'
%left EQUALS MATCH_EQUALS
%left '+' '/' '\\'
%left '%' '^'
%left '.'
%left F_PAREN

%%

input: expr '\n';

expr:OOO_PAREN expr ')'
    | IF F_PAREN expr expr_pair_list_1 expr ')'
    | SWITCH F_PAREN expr expr_pair_list_1 expr ')'
    | '[' expr FOR kvp_list_1 ']'
    | expr F_PAREN expr_list_1 kvp_list_0 ')'
    | expr '.' expr
    | expr '%' expr
    | expr '^' expr
    | expr '+' expr
    | expr '/' expr
    | expr '\\' expr
    | expr EQUALS expr
    | expr MATCH_EQUALS expr
    | '!' expr
    | expr AND expr
    | expr OR expr
    | literal
    | VAR
;

expr_list_1: expr
           | expr_list_1 expr
;

/* compound expressions */
expr_pair: expr ':' expr;

expr_pair_list_1: expr_pair
                | expr_pair_list_1 expr_pair
;

kvp: VAR '=' expr;

kvp_list_0: %empty
          | kvp_list_1
;

kvp_list_1: kvp
          | kvp_list_1 kvp
;

/* literals */
literal: func | dict | list | string | NUM | bool | PUKE | NUL;

func: '{' var_list_1 kvp_list_0 ARROW expr '}';

dict: '<' kvp_list_0 '>';

bool: TRUE | FALSE;

list: '[' list_elem_list_0 ']';

list_elem: expr | expand | SKIP;

list_elem_list_0: %empty
                | list_elem_list_1
;

list_elem_list_1: list_elem
                | list_elem_list_1 list_elem
;

string: DQSTRING
      | SQSTRING
;

expand: '*' expr;

var_list_1: VAR
          | var_list_1 VAR
;



