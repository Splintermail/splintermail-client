%{
    #include <stdio.h>
    #include <libimap/libimap.h>
    #include <limits.h>

    #define E (&p->error)

    // track locations as dstr_t's of the base dstr_t
    // (based on `info bison`: "3.5.3 Default action for Locations"
    #define YYLLOC_DEFAULT(cur, rhs, n) \
    do { \
        if(n) { \
            (cur) = token_extend(YYRHSLOC(rhs, 1), YYRHSLOC(rhs, n)); \
        } else { \
            (cur) = (dstr_t){ \
                .data = YYRHSLOC(rhs, 0).data, \
                .len = 0, \
                .size = 0, \
                .fixed_size = true, \
            }; \
        } \
    } while (0)

    #define PARSE_NUM(text, func, out) do { \
        derr_t e = E_OK; \
        if(!is_error(p->error)){ \
            e = func(&(text), &(out), 10); \
        } \
        if(is_error(e)){ \
            DROP_VAR(&e); \
            smtp_resp_yyerror(&(text), p, "invalid number"); \
            YYERROR; \
        } \
    } while(0)

    static inline bool code_eq(smtp_resp_code_t a, smtp_resp_code_t b){
        return a.code == b.code
            && a.x    == b.x
            && a.y    == b.y;
    }

    #define MERGE_LINES(loc, a, b, out) do { \
        if(is_error(p->error)){ \
            /* clean up inputs */ \
            ie_dstr_free((a).text); \
            ie_dstr_free((b).text); \
            break; \
        } \
        if(!(a).text){ \
            /* nothing to merge */ \
            (out).code = b.code; \
            (out).text = (b).text; \
            break; \
        } \
        if(!code_eq((a).code, (b).code)){ \
            /* syntax error */ \
            ie_dstr_free((a).text); \
            ie_dstr_free((b).text); \
            smtp_resp_yyerror(&(loc), p, "multiline response code mismatch"); \
            YYERROR; \
            break; \
        } \
        /* do the merge */ \
        (out).code = b.code; \
        (out).text = ie_dstr_concat(E, (a).text, (b).text); \
    } while(0)
%}

/* use a different prefix, to not overlap with the imf parser's prefix */
%define api.prefix {smtp_resp_yy}
/* this defines the type of yylval, which is the semantic value of a token */
%define api.value.type {smtp_resp_expr_t}
/* track locations */
%locations
%define api.location.type {dstr_t}
/* reentrant */
%define api.pure full
%define api.push-pull push
/* create a user-data pointer in the api */
%param { smtp_resp_parser_t *p }
/* compile error on parser generator conflicts */
%expect 0

%token RAW

%type <num> num;

%type <code> code;

%type <resp> preline
%type <resp> prelines_0
%type <resp> prelines_1
%destructor { ie_dstr_free($$.text); } <resp>

%% /********** Grammar Section **********/

response: code prelines_0[pre] ' ' text eol
    {
        ie_dstr_t *text = ie_dstr_new(E, &@text, KEEP_RAW);
        smtp_resp_t resp = { .code=$code, .text=text };
        dstr_t loc = token_extend(@code, @text);
        MERGE_LINES(loc, $pre, resp, p->resp);
        YYACCEPT;
    };

/* 'code' appears at the end of the preline expression to make avoiding
   conflicts easy, althought it looks a bit weird:

           preline is this part...
           vvvvvvvvvvvvvvv
        123-some jibberish
        123 ok
        ^^^
    ... plus this part

   All the codes in a multiline response are required to be the same thing
   anyway, so it makes little difference. */
preline: '-' text eol code[c]
    {
        $$.code = $c;
        $$.text = ie_dstr_new(E, &@text, KEEP_RAW);
        $$.text = ie_dstr_append(E, $$.text, &DSTR_LIT(" "), KEEP_RAW);
    };

prelines_0: prelines_1 | %empty { $$.code = (smtp_resp_code_t){0}; $$.text=NULL; };

prelines_1: preline
          | prelines_1[a] preline[b]
            { dstr_t loc = token_extend(@a, @b);
              MERGE_LINES(loc, $a, $b, $$); }
;

code: num[c]                       { $$ = (smtp_resp_code_t){.code=$c}; }
    | num[c] '.' num[x] '.' num[y] { $$ = (smtp_resp_code_t){.code=$c, .x=$x, .y=$y}; }
;

num: RAW  { PARSE_NUM(@RAW, dstr_tou, $$); };

text_: RAW | '.' | '-' | ' ';

text: text_
    | text text_

eol: '\r'
   | '\r' '\n'
;
