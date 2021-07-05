%{
    #include <stdio.h>
    #include <libimap/libimap.h>

    #define MODE(m) p->scan_mode = IMF_SCAN_ ## m

    #define E (&p->error)

    // track locations as dstr_off_t's of the base dstr_t
    // (based on `info bison`: "3.5.3 Default action for Locations"
    #define YYLLOC_DEFAULT(cur, rhs, n) \
    do { \
        if(n) { \
            (cur) = token_extend2(YYRHSLOC(rhs, 1), YYRHSLOC(rhs, n)); \
        } else { \
            (cur) = (dstr_off_t){ \
                .buf = YYRHSLOC(rhs, 0).buf, \
                .start = 0, \
                .len = 0, \
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
%define api.location.type {dstr_off_t}
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

// returned as an intermediate result
%type <hdrs> hdrs

%type <hdr> hdrs_0
%type <hdr> hdrs_1
%type <hdr> hdr
%destructor { imf_hdr_free($$); } <hdr>

%type <body> body
%destructor { imf_body_free($$); } <body>

%% /* Grammar Section */

imf: hdrs_ body[b] DONE {
   dstr_off_t bytes = token_extend2(p->hdrs->bytes, @b);
   p->imf = imf_new(E, bytes, STEAL(imf_hdrs_t, &p->hdrs), $b); YYACCEPT;
};

// an intermediate result we sometimes return to the caller
hdrs_: hdrs[h] { p->hdrs = $h; };

hdrs: hdrs_0[h] EOL[sep] {
   dstr_off_t bytes = token_extend2(@h, @sep);
    $$ = imf_hdrs_new(E, bytes, @sep, $h); MODE(BODY);
};

hdrs_0: EOL       { $$ = NULL; }
      | hdrs_1
;

hdrs_1: hdr                { $$ = $hdr; }
      | hdrs_1[l] hdr[h]   { $$ = imf_hdr_add(E, $l, $h); }
;

// right now we don't parse any structured headers
hdr: HDRNAME[name] ':' { MODE(UNSTRUCT); } hdrval[val]
    { dstr_off_t bytes = token_extend2(@name, @val);
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
        { imf_body_arg_u arg = {0};
          $$ = imf_body_new(E, @b, IMF_BODY_UNSTRUCT, arg); }
;
