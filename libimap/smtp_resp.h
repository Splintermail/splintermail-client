typedef struct {
    unsigned int code;
    unsigned int x;
    unsigned int y;
} smtp_resp_code_t;

typedef struct {
    smtp_resp_code_t code;
    ie_dstr_t *text;
} smtp_resp_t;
DEF_STEAL_STRUCT(smtp_resp_t);

void smtp_resp_free(smtp_resp_t *resp);

typedef struct {
    unsigned int num;
    ie_dstr_t *dstr;
    smtp_resp_code_t code;
    smtp_resp_t resp;
} smtp_resp_expr_t;

// actually a scanner and a parser
typedef struct {
    derr_t error;
    bool freeing;
    const dstr_t *bytes;
    size_t idx;
    smtp_resp_t resp;
} smtp_resp_parser_t;

//int smtp_resp_yylex(smtp_resp_expr_t *lvalp, dstr_t *llocp, smtp_resp_parser_t *p);
int smtp_resp_yylex(dstr_t *llocp, smtp_resp_parser_t *p);
void smtp_resp_yyerror(dstr_t *yyloc, smtp_resp_parser_t *p, char const *str);

/* given a buffer, return either:
    - more=true if the buffer does not contain an entire line
    - more=false and a response at r
    - or return E_RESPONSE on syntax error */
derr_t smtp_resp_parse(const dstr_t *buf, bool *more, smtp_resp_t *r);
