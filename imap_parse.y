%{
    #include <stdio.h>
    #include <imap_parse.h>
    #include <logger.h>

    /* a shorthand for setting the scan mode */
    #define MODE(m) parser->scan_mode = SCAN_MODE_ ## m

    /* a YYACCEPT wrapper that resets some custom parser details  */
    #define ACCEPT \
        MODE(TAG); \
        parser->hooks_up.keep_cancel(parser->hook_data); \
        parser->keep = false; \
        YYACCEPT

    /* if parser->keep is set, get ready to keep chunks of whatever-it-is */
    #define KEEP_INIT(type) if(parser->keep){ \
        derr_t error = parser->hooks_up.keep_init(parser->hook_data, \
                                                   KEEP_ ## type); \
        CATCH(E_ANY){ \
            /* store error, reset parser */ \
            parser->error = error; \
            ACCEPT; \
        } \
    }

    /* if parser->keep is set, store another chunk of whatever-it-is */
    #define KEEP if(parser->keep){ \
        derr_t error = parser->hooks_up.keep(parser->hook_data); \
        CATCH(E_ANY){ \
            /* store error, reset parser */ \
            parser->error = error; \
            ACCEPT; \
        } \
    }

    /* if parser->keep is set, fetch a reference to whatever we just kept */
    static inline imap_token_t keep_ref(imap_parser_t *parser){
        if(parser->keep){
            parser->keep = false;
            return parser->hooks_up.keep_ref(parser->hook_data);
        }else{
            return (imap_token_t){0};
        }
    }

    /* fetch a reference to whatever we just kept */
    #define KEEP_REF keep_ref(parser)
%}

/* this defines the type of yylval, which is the semantic value of a token */
%define api.value.type {imap_token_t}
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
%token NUM
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
%token CAPABILITY
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
/*     CAPABILITY (listed above) */
%token PARSE
%token PERMANENTFLAGS
%token READ_ONLY
%token READ_WRITE
%token TRYCREATE
%token UIDNEXT
%token UIDVALIDITY
%token UNSEEN
/*     ATOM (listed above) */

/* status attributes */
%token MESSAGES
/*     RECENT (listed above) */
/*     UIDNEXT (listed above) */
/*     UIDVALIDITY (listed above) */
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

%% /********** Grammar Section **********/

response: tag_or_not SP post_tag EOL { printf("response!\n"); ACCEPT; };

tag_or_not: tag     { MODE(COMMAND); parser->tagged = true; }
          | '*'     { MODE(COMMAND); parser->tagged = false; }
;

post_tag: status_type_resp
        | CAPABILITY SP atom_list_1
             { printf("capability!\n"); }
        | LIST SP post_list
             { printf("list!\n"); }
        | LSUB SP post_list
             { printf("lsub!\n"); }
        | STATUS SP mailbox '(' status_att_list ')'
             { printf("status!\n"); }
        | FLAGS SP '(' flag_list ')'
             { printf("flags!\n"); }
        | SEARCH SP num_list
             { printf("search!\n"); }
        | NUM SP post_num
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
          | UIDVALIDITY
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

keep_num: NUM   { KEEP_INIT(NUM); KEEP; $$ = KEEP_REF; };

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

status_type_resp: status_type status_extra;

status_type: status_type_ SP { MODE(STATUS_CODE_CHECK); }

status_type_: OK { printf("ok!\n"); }
            | NO { printf("no!\n"); }
            | BAD { printf("bad!\n"); }
            | PREAUTH { printf("preauth!\n"); }
            | BYE { printf("bye!\n"); }
;

status_extra: yes_st_code status_code st_txt_1
            | no_st_code st_txt_0
;

/* YES_STATUS_CODE means we got a '[' */
yes_st_code: YES_STATUS_CODE    { MODE(STATUS_CODE); };

/* NO_STATUS_CODE means we got the start of the text; keep it. */
no_st_code: NO_STATUS_CODE      { MODE(STATUS_TEXT); KEEP_INIT(TEXT); KEEP; };

status_code: status_code_       { MODE(STATUS_TEXT); };

status_code_: ALERT ']' SP                                   { printf("ALERT!\n"); }
            | st_code_capa atom_list_1 ']' SP                { printf("capa (code)!\n"); }
            | PARSE ']' SP                                   { printf("parse!\n"); }
            | st_code_permflags '(' flag_list ')' ']' SP     { printf("perm-flags!\n"); }
            | READ_ONLY ']' SP                               { printf("readonly!\n"); }
            | READ_WRITE ']' SP                              { printf("readwrite!\n"); }
            | TRYCREATE ']' SP                               { printf("trycreate!\n"); }
            | st_code_uidnext SP st_code_keep_num ']' SP     { printf("uidnext!\n"); }
            | st_code_uidvalidity SP st_code_keep_num ']' SP { printf("uid_validity!\n"); }
            | st_code_unseen SP st_code_keep_num ']' SP      { printf("unseen!\n"); }
            | st_code_atom st_txt_inner_0 ']' SP             { printf("atom!\n"); }
;

st_code_capa: CAPABILITY;
st_code_permflags: PERMANENTFLAGS;

st_code_uidnext: UIDNEXT            { MODE(NUM); };
st_code_uidvalidity: UIDVALIDITY    { MODE(NUM); };
st_code_unseen: UNSEEN              { MODE(NUM); };

st_code_atom: atom                  { printf("atoma!\n"); MODE(STATUS_TEXT); };

/*
atom2: ATOM { printf("here\n"); }
     | atom2 ATOM
;
*/

st_code_keep_num: keep_num          { MODE(STATUS_CODE); };

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
         | st_txt_1 st_txt_char     { KEEP; }
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
       | CAPABILITY
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
       | PERMANENTFLAGS
       | READ_ONLY
       | READ_WRITE
       | TRYCREATE
       | UIDNEXT
       | UIDVALIDITY
       | UNSEEN
       | MESSAGES
       | NIL
       | INBOX
;

/* due to grammar ambiguities, an atom cannot consist of a keyword, even
   though a keyword is a proper subclass of an atom.  Therefore additional
   guards will be in place to make sure only keywords we care about are passed
   into the scanner. */
atom: atom_body                 { $$ = KEEP_REF; };

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

qstring: qstring_start qstring_body '"' { $$ = KEEP_REF; };

qstring_start: '"'                      { KEEP_INIT(QSTRING); };

qstring_body: %empty
            | qstring_body QSTRING      { KEEP; }
;

/* note that LITERAL_END is passed by the application after it finishes reading
   the literal from the stream; it is never returned by the scanner */
literal: literal_start LITERAL_END  { $$ = KEEP_REF; };

literal_start: LITERAL              { KEEP_INIT(LITERAL) };

/* like with atoms, an number or literal are technically astr_atoms but that
   introduces grammatical ambiguities*/
astring: astr_atom
       | string
;

astr_atom: astr_atom_body                       { $$ = KEEP_REF; };

astr_atom_body: ASTR_ATOM                       { KEEP_INIT(ASTR_ATOM); KEEP; }
              | astr_atom_body astr_atom_like   { KEEP; }
;

astr_atom_like: ASTR_ATOM
              | NUM
              | keyword
;

tag: tag_body           { $$ = KEEP_REF; };

tag_body: TAG           { KEEP_INIT(TAG); KEEP; }
        | tag_body TAG  { KEEP; }
;

nstring: NIL
       | string
;

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
