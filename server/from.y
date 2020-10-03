%{
    #include <stdio.h>
    #include <server/from.h>

    #define MULTI p->one_byte_mode = false
    #define SINGLE p->one_byte_mode = true

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
%define api.prefix {fromyy}
/* this defines the type of yylval, which is the semantic value of a token */
%define api.value.type {from_expr_t}
/* track locations as dstr_t's */
%locations
/* reentrant */
%define api.pure full
/* push parser */
%define api.push-pull push
/* create a user-data pointer in the api */
%parse-param { from_parser_t *p }
/* compile error on parser generator conflicts */
%expect 0

/* not available in bison 3.3.2 (for debian buster):
%define api.location.type {dstr_t} */


/* a fake token to force the end of the parsing */
%token DONE

%token NIL
%token ANYTEXT
%token WS
%token FOLD
%token OBS_NO_WS_CTRL
%token UTF8

%type <lstr> mailbox_final
%type <lstr> mailbox_list
%type <lstr> utf8_mailbox
%type <lstr> name_addr
%type <lstr> addr_spec
%type <lstr> angle_addr
%type <lstr> local_part
%type <lstr> domain
%type <lstr> domain_literal
%type <lstr> utf8_quoted_string
%type <lstr> uqs_0
%type <lstr> uqs_1
%type <lstr> dl_0
%type <lstr> dl_1
%destructor { $$ = p->link_return(p, $$); } <lstr>

%type <text> dtext
%type <text> quoted_pair
%type <text> utf8_quoted_pair
%type <text> utf8_qcontent

%% /* Grammar Section */

// xbr = possible cfws ("break")

// from: mailbox_list[m] DONE { p->out = $m; }
// // mailbox-list, since obs-mbox-list is too hard to support
// // (that means no leading/trailing/extra commas)
// mailbox_list: utf8_mailbox
//             | mailbox_list[l] xbr ',' xbr utf8_mailbox[i]
//               { $$ = $l; p->link_return(p, $i); }
// ;

from: xbr comma_0 mailbox_final[m] DONE { p->out = $m; }

mailbox_list: utf8_mailbox
            | mailbox_list[l] xbr comma_1 utf8_mailbox[i]
              { $$ = $l; p->link_return(p, $i); }
;

mailbox_final: mailbox_list
             | mailbox_list xbr comma_1

// mailbox_list: mailbox_1 comma_0 DONE
//             | mailbox_list comma_0

comma_0: comma_1 | %empty;
comma_1: ',' xbr
       | comma_1 ',' xbr


// rfc5322: name_addr or addr_spec
// rfc5335: use utf8-addr-spec instead
utf8_mailbox: name_addr
            | addr_spec
;

name_addr: angle_addr
         | local_part[l] xbr phrase xbr angle_addr[a]  { $$ = $a; p->link_return(p, $l); }
         | local_part[l] xbr angle_addr[a]             { $$ = $a; p->link_return(p, $l); }
;

// rfc5322: [CFWS] "<" addr-spec ">" [CFWS]
// rfc5322(obs-angle-addr): has its own grammar (not supporting that here)
// rfc5335: adds [CFWS] "<" utf8-addr-spec [ alt-address ] ">" [CFWS]
angle_addr: '<' xbr addr_spec xbr maybe_alt_address '>'  { $$ = $addr_spec; };

maybe_alt_address: '<' xbr addr_spec[a] xbr '>' xbr { p->link_return(p, $a); }
                 | %empty
;

// this is a mix of utf8-addr-spec and bs-addr-spec to avoid parse conflicts
addr_spec: local_part[l] xbr '@'[at] xbr domain[d]
         { $$ = $l;
           $$ = lstr_concat(E, $$, p->link_new(E, p, @at));
           $$ = lstr_concat(E, $$, $d); };

// rfc5322: dot-atom or quoted-string (or obs-local-part)
// NOTICE: I am not allowing obs-local-part; no breaks in local part
// rfc5335: use utf8-dot-atom and utf8-quoted-string instead
local_part: utf8_quoted_string
          | utf8_dot_atom[a]  { $$ = p->link_new(E, p, @a); }
;

// rfc5322: dot-atom or domain-literal (or obs-domain)
// NOTICE: I am not allowing obs-domain; no breaks in domain
// rfc5335: modifies dot_atom to be utf8-dot-atom
// this is a mix of utf8-domain and obs-domain to avoid parse conflicts
domain: domain_literal
      | utf8_dot_atom[a]  { $$ = p->link_new(E, p, @a); }
;

// no CFWS in domain_literal, only FWS
domain_literal: '[' maybe_fws dl_0[d] ']'  { $$ = $d; };
dl_0: dl_1 maybe_fws
    | %empty          { $$ = p->link_new(E, p, DSTR_LIT("")); };
// TODO: am I supposed to not ignore FWS here?
dl_1: dtext[t]                    { $$ = p->link_new(E, p, $t); }
    | dl_1[d] maybe_fws dtext[t]  { $$ = lstr_concat(E, $d, p->link_new(E, p, $t)); }
;

// rfc5322: printable non-ws except [ ] \
// rfc5322(obs-dtext): also control chars except CR LF \x00
dtext: dtext_raw[d]    { $$ = @d; }
     | quoted_pair
;
dtext_raw: ANYTEXT|'('|')'|'<'|'>'|':'|';'|'@'|','|'.'|'"' | OBS_NO_WS_CTRL;

/*** misc ***/

// rfc5322: word is an atom or quoted-string
// rfc5335: word as a utf8-atom or utf8-quoted-string
word: utf8_atom
    | utf8_quoted_string[q]  { p->link_return(p, $q); }
;

utf8_quoted_string: '"' uqs_0[u] '"'  { $$ = $u; };

uqs_0: uqs_1
     | %empty  { $$ = p->link_new(E, p, DSTR_LIT("")); }
;
uqs_1: utf8_qcontent[q]          { $$ = p->link_new(E, p, $q); }
     | fws[f]                    { $$ = p->link_new(E, p, @f); }
     | uqs_1[u] utf8_qcontent[q] { $$ = lstr_concat(E, $u, p->link_new(E, p, $q)); }
     | uqs_1[u] fws[f]           { $$ = lstr_concat(E, $u, p->link_new(E, p, @f)); }
;

utf8_qcontent: utf8_qtext[q]  { $$ = @q; }
             | utf8_quoted_pair
;

// useful list for copy/pasting
// specials: '('|')'|'<'|'>'|'['|']'|':'|';'|'@'|'\\'|','|'.'|'"';

// rfc5322: printable us-ascii other than \ or "
// rfc5322(obs-qtext): now include control characters except: CR LF \x00
// rfc5335: now allow utf8 characters
utf8_qtext: ANYTEXT|'('|')'|'<'|'>'|'['|']'|':'|';'|'@'|','|'.'
          | OBS_NO_WS_CTRL
          | UTF8
;

// rfc5322: "\" (VCHAR / WSP)
// rfc5322(obs-qp): allow escaping anything <=\x7F
// rfc5335: allow escaping utf8
utf8_quoted_pair: '\\' { SINGLE; } utf8_escapable[e] { MULTI; $$ = @e; };
utf8_escapable: ANYTEXT|'('|')'|'<'|'>'|'['|']'|':'|';'|'@'|'\\'|','|'.'|'"'|WS
              | OBS_NO_WS_CTRL|NIL|'\n'|'\r'
              | UTF8
;
// same thing without utf8, used in dtext
quoted_pair: '\\' { SINGLE; } escapable[e] { MULTI; $$ = @e; }
escapable: ANYTEXT|'('|')'|'<'|'>'|'['|']'|':'|';'|'@'|'\\'|','|'.'|'"'|WS
         | OBS_NO_WS_CTRL|NIL|'\n'|'\r'
;

// phrase is effectively obs_phrase (shown here)
phrase: word
      | phrase xbr word
      | phrase xbr '.'
;

utf8_atom: ANYTEXT | UTF8;

// dots are allowed in the atom, but not consecutive dots or start or ending
utf8_dot_atom: utf8_atom
             | utf8_dot_atom '.' utf8_atom;

/*** COMMENTS AND FOLDING WHITE SPACE ***/

maybe_fws: fws | %empty;

// rfc5322: ([*WSP CRLF] 1*WSP)  -> only one EOL allowed
// rfc5322(obs-FWS): 1*WSP *(CRLF 1*WSP)  -> any amount of EOL allowed
// This was really fucking hard in bison, so it's actually done in re2c
fws: WS | FOLD;

comment: '(' maybe_fws ccontent_0 ')';

ccontent_0: %empty
          | ccontent_1 maybe_fws
;

ccontent_1: ccontent
          | ccontent_1 maybe_fws ccontent
;

ccontent: ctext
        | utf8_quoted_pair
        | comment
;

cfws: fws
    | comment
    | cfws fws
    | cfws comment
;

maybe_cfws: cfws | %empty;

// rf5322: printable non-ws ascii except ( ) or \
// rf5322(obs-ctext): add in control characters except \x00 CR LF
// rf5335: add in utf8 characters
ctext: ANYTEXT|'<'|'>'|'['|']'|':'|';'|'@'|','|'.'|'"'
     | OBS_NO_WS_CTRL
     | UTF8
;

// maybe break
xbr: maybe_cfws;
