#ifndef QWERREWQ_H
#define QWERREWQ_H

#include <jsw_atree.h>

// before including bison-generated header files, we need to define some types

enum expr_type_t {
    EXP_IF,
    EXP_SWITCH,
    EXP_FOR,
    EXP_FUNC_CALL,
    // operators
    EXP_DOT,
    EXP_PERCENT,
    EXP_CARET,
    EXP_PLUS,
    EXP_EQ,
    EXP_MATEQ,
    EXP_NOT,
    EXP_AND,
    EXP_OR,
    // literals
    EXP_FUNC,
    EXP_DICT,
    EXP_LIST,
    EXP_STRING,
    EXP_NUM,
    EXP_TRUE,
    EXP_FALSE,
    EXP_PUKE,
    EXP_NUL,
    EXP_VAR,
};

struct expr_t;
typedef struct expr_t expr_t;

typedef struct {
    expr_t *lhs;
    expr_t *rhs;
} binop_t;

// for string literals, store the original token and the decoded version
typedef struct {
    dstr_t raw;
    dstr_t out;
} string_t;

union expr_u {
    intmax_t num;
    bool boolean;
    dstr_t dstr;
    void *ign;
    binop_t binop;
    expr_t *expr; // primarily for unary operator
    string_t string;
};

struct expr_t {
    enum expr_type_t t;
    union expr_u u;
};

// all possible types of tokens/expressions in bison parser
typedef union semtyp_t{
    expr_t *expr;
    dstr_t dstr;
} semtyp_t;

#include <qwerrewq.tab.h>

void yyerror(YYLTYPE *yyloc, void *parser, char const *s);
char *toktyp_to_str(int type);

derr_t expr_tostr(expr_t *expr, dstr_t *out);

typedef struct {
    dstr_t key;
    expr_t expr;
    jsw_anode_t node;
} kvp_t;

derr_t kvp_new(kvp_t **kvp);
void kvp_free(kvp_t **kvp);

#endif // QWERREWQ_H
