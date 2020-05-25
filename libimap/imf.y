%{
    #include <stdio.h>
    #include <libimap/libimap.h>

    #define MODE(m) p->scan_mode = IMF_SCAN_ ## m

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

%}

/* use a different prefix, to not overlap with the imap parser's prefix */
%define api.prefix {imfyy}
/* this defines the type of yylval, which is the semantic value of a token */
%define api.value.type {imf_expr_t}
/* track locations as dstr_t's */
%locations
%define api.location.type {dstr_t}
/* reentrant */
%define api.pure full
/* push parser */
%define api.push-pull push
/* create a user-data pointer in the api */
%parse-param { imf_parser_t *p }
/* compile error on parser generator conflicts */
%expect 0

/* the scanner passes its errors to the parser for better error recovery */
%token INVALID_TOKEN
/* a fake token to force the end of the parsing */
%token DONE

%token EOL
%token WS
%token HDRNAME
%token UNSTRUCT
%token BODY

%type <hdr> hdrs
%type <hdr> hdrs_0
%type <hdr> hdrs_1
%type <hdr> hdr
%destructor { imf_hdr_free($$); } <hdr>

%type <body> body
%destructor { imf_body_free($$); } <body>

%% /* Grammar Section */

imf: hdrs[h] body[b] DONE
   { dstr_t bytes = token_extend(@h, @b);
     p->imf = imf_new(E, bytes, @h, $h, $b); YYACCEPT; }

hdrs: hdrs_0[h] EOL { $$ = $h; MODE(BODY); };

hdrs_0: EOL       { $$ = NULL; }
      | hdrs_1
;

hdrs_1: hdr                { $$ = $hdr; }
      | hdrs_1[l] hdr[h]   { $$ = imf_hdr_add(E, $l, $h); }
;

// right now we don't parse any structured headers
hdr: HDRNAME[name] ':' { MODE(UNSTRUCT); } hdrval[val]
    { dstr_t bytes = token_extend(@name, @val);
      imf_hdr_arg_u arg = { .unstruct = @val };
      $$ = imf_hdr_new(E, bytes, @name, IMF_HDR_UNSTRUCT, arg); }
;

// At the start of every new line, we either have folding white space or a new header
hdr_eol: EOL { MODE(HDR); }

// Always trigger a mode change after seeing a block of folding whitespace
HDR_WS: WS { MODE(UNSTRUCT); };

hdrval: hdr_eol
      | UNSTRUCT[u] hdr_eol
      | hdrval[h] HDR_WS hdr_eol
      | hdrval[h] HDR_WS UNSTRUCT[u] hdr_eol
;

// right now only unstruct bodies are supported
body: %empty     { $$ = NULL; }
    | BODY[b]
        { imf_body_arg_u arg = {};
          $$ = imf_body_new(E, @b, IMF_BODY_UNSTRUCT, arg); }
;
