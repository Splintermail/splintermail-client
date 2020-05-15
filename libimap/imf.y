%{
    #include <stdio.h>
    #include <libimap/libimap.h>

    #define MODE(m) p->scan_mode = IMF_SCAN_ ## m

    #define E (&p->error)
%}

/* use a different prefix, to not overlap with the imap parser's prefix */
%define api.prefix {imfyy}
/* this defines the type of yylval, which is the semantic value of a token */
%define api.value.type {imf_expr_t}
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

%type <dstr> hdrval
%type <dstr> body_1
%destructor { ie_dstr_free($$); } <dstr>

%type <hdr> hdr
%type <hdr> hdrs_0
%type <hdr> hdrs_1
%destructor { imf_hdr_free($$); } <hdr>

%type <body> body
%destructor { imf_body_free($$); } <body>

%type <token> hdrname
%type <token> unstruct
// no destructor for token

%% /* Grammar Section */

imf: hdrs_0[h] EOL { MODE(UNSTRUCT); } body[b] DONE
   { p->imf = imf_new(E, $h, $b); YYACCEPT; }

hdrs_0: EOL       { $$ = NULL; }
      | hdrs_1
;

hdrs_1: hdr                { $$ = $hdr; }
      | hdrs_1[l] hdr[h]   { $$ = imf_hdr_add(E, $l, $h); }
;

// right now we don't parse any structured headers
hdr: hdrname[name] ':' { MODE(UNSTRUCT); } hdrval[val]
    { imf_hdr_arg_u arg = { .unstruct = $val };
      $$ = imf_hdr_new(E, ie_dstr_new(E, &$name, KEEP_RAW), IMF_HDR_UNSTRUCT, arg); }
;

// since we don't operate on streams, just remember the token for later
hdrname: HDRNAME  { $$ = *p->token; };

// At the start of every new line, we either have folding white space or a new header
HDR_EOL: EOL { MODE(HDR); }

// Always trigger a mode change after seeing a block of folding whitespace
HDR_WS: WS { MODE(UNSTRUCT); };

hdrval: HDR_EOL                        { $$ = NULL; }
      | unstruct[u] HDR_EOL            { $$ = ie_dstr_new(E, &$u, KEEP_RAW); }
      | hdrval[h] HDR_WS HDR_EOL
        { $$ = ie_dstr_append(E, $h, &DSTR_LIT(" "), KEEP_RAW); }
      | hdrval[h] HDR_WS unstruct[u] HDR_EOL
        { $$ = ie_dstr_append(E,
                    ie_dstr_append(E, $h, &DSTR_LIT(" "), KEEP_RAW),
                    &$u, KEEP_RAW); }
;

// since we don't operate on streams, just remember the token for later
unstruct: UNSTRUCT { $$ = *p->token; };

// right now only unstruct bodies are supported
body: %empty     { $$ = NULL; }
    | body_1[b]
        { imf_body_arg_u arg = { .unstruct = $b };
          $$ = imf_body_new(E, IMF_BODY_UNSTRUCT, arg); }
;

body_1: EOL                 { $$ = ie_dstr_new(E, p->token, KEEP_RAW); }
      | UNSTRUCT            { $$ = ie_dstr_new(E, p->token, KEEP_RAW); }
      | body_1[b] EOL       { $$ = ie_dstr_append(E, $b, p->token, KEEP_RAW); }
      | body_1[b] UNSTRUCT  { $$ = ie_dstr_append(E, $b, p->token, KEEP_RAW); }
;
