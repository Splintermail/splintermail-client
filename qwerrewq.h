#ifndef QWERREWQ_H
#define QWERREWQ_H

#include <jsw_atree.h>

typedef struct {
    dstr_t name;
    dstr_t buf;
} file_t;

LIST_HEADERS(file_t)

typedef struct {
    dstr_t dstr;
    int type;
    size_t start_line;
    size_t start_col;
    size_t end_line;
    size_t end_col;
} loc_t;

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
    EXP_EXPAND,
    // literals
    EXP_FUNC,
    EXP_DICT,
    EXP_LIST,
    // more literals, but also atoms
    EXP_STRING,
    EXP_NUM,
    EXP_TRUE,
    EXP_FALSE,
    EXP_PUKE,
    EXP_SKIP,
    EXP_NUL,
    EXP_VAR,
};

char* expr_type_to_cstr(enum expr_type_t t);

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

typedef struct kvp_t {
    dstr_t key;
    expr_t *value;
    jsw_anode_t node;
    struct kvp_t *next;
} kvp_t;

typedef struct list_t {
    expr_t *expr;
    struct list_t *next;
} list_t;

typedef struct dstr_list_t {
    dstr_t dstr;
    struct dstr_list_t *next;
} dstr_list_t;

typedef struct {
    dstr_list_t *vars;
    kvp_t *kvps;
    expr_t *out;
} func_t;

typedef struct expr_list_t {
    expr_t *expr;
    struct expr_list_t *next;
} expr_list_t;

typedef struct expr_pair_list_t {
    expr_t *lhs;
    expr_t *rhs;
    struct expr_pair_list_t *next;
} expr_pair_list_t;

typedef struct {
    expr_pair_list_t *tests;
    expr_t *else_expr;
} if_t;

typedef struct {
    expr_t *value;
    expr_pair_list_t *tests;
    expr_t *default_expr;
} switch_t;

typedef struct {
    expr_t *out;
    kvp_t *kvps;
} for_t;

typedef struct {
    expr_t *func;
    expr_list_t *params;
    kvp_t *kvps;
} func_call_t;

union expr_u {
    intmax_t num;
    bool boolean;
    dstr_t dstr;
    void *ign;
    binop_t binop;
    expr_t *expr; // primarily for unary operators
    string_t string;
    func_t func;
    kvp_t *kvp;
    list_t *list;
    if_t if_call;
    switch_t switch_call;
    for_t for_call;
    func_call_t func_call;
};

struct expr_t {
    enum expr_type_t t;
    union expr_u u;
    dstr_t dstr;
    loc_t loc;
};

// all possible types of tokens/expressions in bison parser
typedef union semtyp_t{
    expr_t *expr;
    dstr_t dstr;
    func_t func;
    kvp_t *kvp;
    list_t *list;
    dstr_list_t *dstr_list;
    if_t if_call;
    switch_t switch_call;
    for_t for_call;
    func_call_t func_call;
    expr_list_t *expr_list;
    expr_pair_list_t *expr_pair_list;
} semtyp_t;

// a struct of values the parser needs access to
typedef struct {
    file_t *file;
    expr_list_t **result;
} parser_vars_t;

#define YYLTYPE loc_t
#include <qwerrewq.tab.h>

void yyerror(YYLTYPE *yyloc, parser_vars_t pv, char const *s);
char *toktyp_to_str(int type);

derr_t expr_tostr(expr_t *expr, dstr_t *out);

derr_t kvp_new(kvp_t **kvp);
void kvp_free(kvp_t **kvp);

#endif // QWERREWQ_H
