%{
    #include <stdio.h>
    #include <imap_parse.h>
    #include <logger.h>
    #include <imap_expression.h>

    /* When you have a dstr-type token but there's nothing there */
    #define NULTOK (dstr_t){0}

    // when this is called, only the status is known
    #define STTOK_STATUS(st) \
        (ie_resp_status_type_t){ \
                .status = STATUS_TYPE_ ## st, \
            }
    // when this is called, only the status code and code_extra are known
    #define ST_CODE(_code, _extra) \
        (ie_resp_status_type_t){ \
                .code = STATUS_CODE_ ## _code, \
                .code_extra = _extra, \
            }
    /* when this is called, the code/extra token is fused with the text.
       Note that code/extra might be NULL, if no code was given */
    static inline ie_resp_status_type_t STTOK_TEXT(ie_resp_status_type_t *c_e,
                                                   dstr_t _text){
        return (ie_resp_status_type_t){
                .code = c_e ? c_e->code : STATUS_CODE_NONE,
                .code_extra = c_e ? c_e->code_extra : 0,
                .text = _text,
            };
    }
    // this fuses all the components of the STTOK together
    static inline ie_resp_status_type_t STTOK_FUSE(ie_resp_status_type_t st,
                                                   ie_resp_status_type_t c_e_t){
        return (ie_resp_status_type_t){
                .status = st.status,
                .code = c_e_t.code,
                .code_extra = c_e_t.code_extra,
                .text = c_e_t.text,
            };
    }

    #define MODE(m) parser->scan_mode = SCAN_MODE_ ## m

    /* a YYACCEPT wrapper that resets some custom parser details  */
    #define ACCEPT \
        MODE(TAG); \
        keep_cancel(parser); \
        parser->keep = false; \
        YYACCEPT

    /* if parser->keep is set, get ready to keep chunks of whatever-it-is */
    #define KEEP_INIT(type) if(parser->keep){ \
        derr_t error = keep_init(parser, KEEP_ ## type); \
        CATCH(E_ANY){ \
            /* store error, reset parser */ \
            parser->error = error; \
            ACCEPT; \
        } \
    }

    /* if parser->keep is set, store another chunk of whatever-it-is */
    #define KEEP if(parser->keep){ \
        derr_t error = keep(parser); \
        CATCH(E_ANY){ \
            /* store error, reset parser */ \
            parser->error = error; \
            ACCEPT; \
        } \
    }

    // if parser->keep is set, fetch a reference to whatever we just kept
    // (not a macro because we need to set parser->keep and return a value)
    static inline dstr_t do_keep_ref(imap_parser_t *parser){
        if(parser->keep){
            parser->keep = false;
            return keep_ref(parser);
        }else{
            return NULTOK;
        }
    }

    /* fetch a reference to whatever we just kept */
    #define KEEP_REF do_keep_ref(parser)

    /* the *_HOOK macros are for calling a hook, then freeing memory */
    #define ST_HOOK(tag, st){ \
        parser->hooks_up.status_type(parser->hook_data, &tag, st.status, \
                                     st.code, st.code_extra, &st.text ); \
        dstr_free(&tag); \
        dstr_free(&st.text); \
    }
%}

/* this defines the type of yylval, which is the semantic value of a token */
%define api.value.type {imap_expr_t}
/* reentrant */
%define api.pure full
/* push parser */
%define api.push-pull push
/* create a user-data pointer in the api */
%parse-param { imap_parser_t *parser }
/* compile error on parser generator conflicts */
%expect 0

/* some generic types */
%token ATOM
%token ASTR_ATOM
%token FLAG
%token NIL
%token <num> NUM
%token QCHAR
%token QSTRING
%token LITERAL
%token LITERAL_END

/* INITIAL state */
%token TAG

/* POST_TAG state */
%token OK
%token NO
%token BAD
%token PREAUTH
%token BYE
%token CAPA
%token LIST
%token LSUB
%token STATUS
%token FLAGS
%token SEARCH

/* POST_NUM state */
%token EXISTS
%token RECENT
%token EXPUNGE
%token FETCH

/* status-code stuff */
%token YES_STATUS_CODE
%token NO_STATUS_CODE
%token ALERT
/*     BADCHARSET (response to search, we aren't going to search) */
/*     CAPA (listed above) */
%token PARSE
%token PERMFLAGS
%token READ_ONLY
%token READ_WRITE
%token TRYCREATE
%token UIDNEXT
%token UIDVLD
%token UNSEEN
/*     ATOM (listed above) */

/* status attributes */
%token MESSAGES
/*     RECENT (listed above) */
/*     UIDNEXT (listed above) */
/*     UIDVLD (listed above) */
/*     UNSEEN (listed above) */

/* FETCH state */
/*     FLAGS (listed above */
%token ENVELOPE
%token INTERNALDATE
%token RFC822
%token RFC822_TEXT
%token RFC822_HEADER
%token RFC822_SIZE
%token BODY_STRUCTURE
%token BODY
%token UID
%token STRUCTURE

/* miscellaneous */
%token INBOX
%token TEXT
%token EOL

/* non-terminals with semantic values */
/*
%type <dstr> keep_atom
%type <dstr> keep_qstring
%type <dstr> keep_string
%type <dstr> keep_astr_atom
*/
%type <dstr> tag
%type <dstr> st_txt_0
%type <dstr> st_txt_1
%destructor { dstr_free(& $$); } <dstr>

%type <num> sc_num

%type <status_type> status_type_resp
%type <status_type> status_type
%type <status_type> status_type_
%type <status_type> status_code
%type <status_type> status_code_
%type <status_type> status_extra


%% /********** Grammar Section **********/

response: tagged EOL { printf("response!\n"); ACCEPT; };
        | untagged EOL { printf("response!\n"); ACCEPT; };

tagged: tag SP status_type_resp[s]   { ST_HOOK($tag, $s); };

untagged: '*' SP status_type_resp[s] { ST_HOOK(NULTOK, $s); }
        | '*' SP CAPA SP atom_list_1
        | '*' SP LIST SP post_list
        | '*' SP LSUB SP post_list
        | '*' SP STATUS SP mailbox '(' status_att_list ')'
        | '*' SP FLAGS SP '(' flag_list ')'
        | '*' SP SEARCH SP num_list
        | '*' SP NUM SP post_num
;

post_list: '(' flag_list ')' nqchar mailbox
;

/* either the literal NIL or a DQUOTE QUOTED-CHAR DQUOTE */
nqchar: NIL
      | QCHAR
;

status_att_list: %empty
               | status_att_list status_att NUM
;

status_att: MESSAGES
          | RECENT
          | UIDNEXT
          | UIDVLD
          | UNSEEN
;

post_num: EXISTS { printf("exists!\n"); }
        | RECENT { printf("recent!\n"); }
        | EXPUNGE { printf("expunge!\n"); }
        | FETCH '(' msg_att_list ')' { printf("fetch!\n"); }
;

msg_att_list: %empty
            | msg_att_list msg_att
;

msg_att: FLAGS '(' flag_list ')'
       | ENVELOPE '(' envelope ')'
       | INTERNALDATE QSTRING
       | RFC822 nstring /* read this */
       | RFC822_TEXT nstring
       | RFC822_HEADER nstring
       | RFC822_SIZE NUM
       | BODY_STRUCTURE /* PUKE! '(' body ')' */
       | BODY /* PUKE! post_body */
       | UID NUM
;

/*        date    subj    from       sender     reply-to */
envelope: nstring nstring naddr_list naddr_list nstring
/*        to         cc         bcc        in-reply-to message-id */
          naddr_list naddr_list naddr_list naddr_list  nstring
;

naddr_list: NIL
          | '(' addr_list ')'
;

/*           addr-name addr-adl addr-mailbox addr-host */
address: '(' nstring   nstring  nstring      nstring   ')'
;

addr_list: %empty
         | addr_list address
;

/* post_body: '(' body ')'
/*          | '[' body_section ']' post_body_section
/* ;
/*
/* body_section: %empty
/*             | section_msgtext
/*             | section_part_and_msgtext
/* ;
/*
/* section_part_and_text: section_part
/*                      | section_part '.' section_msgtext
/* ;
/*
/* section_msgtext: ATOM '(' astring_list ')'
/*                | ATOM
/* ;
/*
/* section_part: NUM
/*             | section_part '.' NUM
/* ;
/*
/* post_body_section: '<' NUM '>' nstring
/*                  | nstring
/* ;
/*
/* body: body_type_1part
/*     | body_type_mpart
/* ;
/*
/* body_type_1part: body_type_basic
/*                | body_type_msg
/*                | body_type_text
/* ;
/*
/* /*               media_basic media_subtype */
/* body_type_basic: string      string        body_fields
/* ;
/*
/* /*                          body_fld_id body_fld_desc body_fld_enc body_fld_octets*/
/* body_fields: body_fld_param nstring     nstring       string       NUM
/* ;
/*
/* body_fld_param: '(' string_list ')'
/*               | NIL
/* ;
/*
/* /*             --media-message--                           body_fld_lines */
/* body_type_msg: string     string body_fields envelope body NUM
/* ;
*/


/*** status-type handling.  Thanks the the shitty grammar, IMAP4rev1 ***/

status_type_resp: status_type[s] status_extra[e] { $$ = STTOK_FUSE($s, $e); };

status_type: status_type_ SP { MODE(STATUS_CODE_CHECK); }

status_type_: OK        { $$ = STTOK_STATUS(OK); }
            | NO        { $$ = STTOK_STATUS(NO); }
            | BAD       { $$ = STTOK_STATUS(BAD); }
            | PREAUTH   { $$ = STTOK_STATUS(PREAUTH); }
            | BYE       { $$ = STTOK_STATUS(BYE); }
;

status_extra:
  yes_st_code status_code[c] st_txt_1[t]    { $$ = STTOK_TEXT(& $c, $t); }
| no_st_code st_txt_0[t]                    { $$ = STTOK_TEXT(NULL, $t); }
;

/* YES_STATUS_CODE means we got a '[' */
yes_st_code: YES_STATUS_CODE    { MODE(STATUS_CODE); };

/* NO_STATUS_CODE means we got the start of the text; keep it. */
no_st_code: NO_STATUS_CODE      { MODE(STATUS_TEXT); KEEP_INIT(TEXT); KEEP; };

status_code: status_code_       { MODE(STATUS_TEXT); $$ = $1; };

status_code_: sc_alert ']' SP           { $$ = ST_CODE(ALERT,      0); }
| sc_capa atom_list_1 ']' SP            { $$ = ST_CODE(CAPA,       0); }
| PARSE ']' SP                          { $$ = ST_CODE(PARSE,      0); }
| sc_permflags '(' flag_list ')' ']' SP { $$ = ST_CODE(PERMFLAGS,  0); }
| READ_ONLY ']' SP                      { $$ = ST_CODE(READ_ONLY,  0); }
| READ_WRITE ']' SP                     { $$ = ST_CODE(READ_WRITE, 0); }
| TRYCREATE ']' SP                      { $$ = ST_CODE(TRYCREATE,  0); }
| sc_uidnext SP sc_num[n] ']' SP        { $$ = ST_CODE(UIDNEXT,    $n); }
| sc_uidvld SP sc_num[n] ']' SP         { $$ = ST_CODE(UIDVLD,     $n); }
| sc_unseen SP sc_num[n] ']' SP         { $$ = ST_CODE(UNSEEN,     $n); }
| sc_atom st_txt_inner_0 ']' SP         { $$ = ST_CODE(ATOM,       0); }
;

sc_alert: ALERT { parser->keep = true; }
sc_capa: CAPA;
sc_permflags: PERMFLAGS;

sc_uidnext: UIDNEXT            { MODE(NUM); };
sc_uidvld: UIDVLD              { MODE(NUM); };
sc_unseen: UNSEEN              { MODE(NUM); };

sc_atom: atom                  { MODE(STATUS_TEXT); };

sc_num: NUM          { MODE(STATUS_CODE); $$ = $1; };

st_txt_inner_0: %empty
              | st_txt_inner_0 st_txt_inner_char
;

st_txt_inner_char: ' '
                 | '['
                 | TEXT
;

st_txt_0: %empty                    { KEEP_INIT(TEXT); $$ = KEEP_REF; }
        | st_txt_1_                 { $$ = KEEP_REF; }
;

st_txt_1: st_txt_1_                 { $$ = KEEP_REF; };

st_txt_1_: st_txt_char              { KEEP_INIT(TEXT); KEEP; }
         | st_txt_1_ st_txt_char     { KEEP; }
;

st_txt_char: st_txt_inner_char
           | ']'
;


/*** start of "helper" categories: ***/

keyword: OK
       | NO
       | BAD
       | PREAUTH
       | BYE
       | CAPA
       | LIST
       | LSUB
       | STATUS
       | FLAGS
       | SEARCH
       | EXISTS
       | RECENT
       | EXPUNGE
       | FETCH
       | ALERT
       | PARSE
       | PERMFLAGS
       | READ_ONLY
       | READ_WRITE
       | TRYCREATE
       | UIDNEXT
       | UIDVLD
       | UNSEEN
       | MESSAGES
       | NIL
       | INBOX
;

/* due to grammar ambiguities, an atom cannot consist of a keyword, even
   though a keyword is a proper subclass of an atom.  Therefore additional
   guards will be in place to make sure only keywords we care about are passed
   into the scanner. */
atom: atom_body

atom_body: ATOM                 { KEEP_INIT(ATOM); KEEP; }
         | atom_body atom_like  { KEEP; }
;

atom_like: ATOM
         | NUM
         | keyword
;

string: qstring
      | literal
;

qstring: '"' { KEEP_INIT(QSTRING); } qstring_body '"';

qstring_body: %empty
            | qstring_body QSTRING      { KEEP; }
;

/* note that LITERAL_END is passed by the application after it finishes reading
   the literal from the stream; it is never returned by the scanner */
literal: literal_start LITERAL_END;

literal_start: LITERAL              { KEEP_INIT(LITERAL) };

/* like with atoms, an number or literal are technically astr_atoms but that
   introduces grammatical ambiguities*/
astring: astr_atom
       | string
;

astr_atom: ASTR_ATOM                  { KEEP_INIT(ASTR_ATOM); KEEP; }
         | astr_atom astr_atom_like   { KEEP; }
;

astr_atom_like: ASTR_ATOM
              | NUM
              | keyword
;

tag: tag_body      { $$ = KEEP_REF; MODE(COMMAND); };

tag_body: TAG           { parser->keep = true; KEEP_INIT(TAG); KEEP; }
        | tag_body TAG  { KEEP; }
;

nstring: NIL
       | string
;

/* the "keep" variations of the above (except tag, which is always kept) */
/*
keep_atom: { parser->keep = true; } atom { $$ = KEEP_REF; };
keep_astr_atom: { parser->keep = true; } astr_atom { $$ = KEEP_REF; };
keep_qstring: { parser->keep = true; } qstring { $$ = KEEP_REF; };
keep_string: { parser->keep = true; } string { $$ = KEEP_REF; };
*/

/* lists */

atom_list_1: atom
           | atom_list_1 SP atom
;

mailbox: astring
       | INBOX
;

flag_list: %empty
         | flag_list FLAG
;

num_list: %empty
        | num_list NUM
;

SP: ' ';
