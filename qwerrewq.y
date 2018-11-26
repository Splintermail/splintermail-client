%{
    #include <stdio.h>
%}

%token PUKE
%token NUL
%token SKIP
%token TRUE
%token FALSE
%token NUM
%token STRING
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
literal: func | dict | list | STRING | NUM | bool | PUKE | NUL;

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

expand: '*' expr;

var_list_1: VAR
          | var_list_1 VAR
;



