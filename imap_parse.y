%{
    #include <stdio.h>
    #include <imap_parse.h>
    #include <logger.h>
    #include <imap_expression.h>

    // a YYACCEPT wrapper that resets some custom parser details
    #define ACCEPT \
        MODE(TAG); \
        parser->keep = false; \
        YYACCEPT

    // for checking an error, but propagating it through non-standard means
    #define DOCATCH(_code) { \
        derr_t error = _code; \
        CATCH(E_ANY){ \
            /* store error and reset parser */ \
            parser->error = error; \
            ACCEPT; \
        } \
    }

    // When you have a dstr-type token but there's nothing there
    #define NUL_DSTR (dstr_t){0}

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
    // when this is called, the code/extra token is fused with the text
    // (not a macro so that c_e can be a NULL)
    static inline ie_resp_status_type_t STTOK_TEXT(ie_resp_status_type_t *c_e,
                                                   dstr_t _text){
        return (ie_resp_status_type_t){
                .code = c_e ? c_e->code : STATUS_CODE_NONE,
                .code_extra = c_e ? c_e->code_extra : 0,
                .text = _text,
            };
    }
    // this fuses all the components of the STTOK together
    #define STTOK_FUSE(st, c_e_t) \
        (ie_resp_status_type_t){ \
            .status = st.status, \
            .code = c_e_t.code, \
            .code_extra = c_e_t.code_extra, \
            .text = c_e_t.text, \
        }

    #define MODE(m) parser->scan_mode = SCAN_MODE_ ## m

    // the scanner only returns QCHAR with 1- or 2-char matches
    #define qchar_to_char \
        (parser->token->len == 1) \
            ? parser->token->data[0] \
            : parser->token->data[1] \

/* KEEP API summary:

(purpose: only allocate memory for tokens you actually need)

// this happens in KEEP_START
keep = true
keep_init = false

try:
    // possibly called by some other hook
    {
        if(keep){
            KEEP_INIT
            keep_init = true
        }

        // possibly called be even more hooks, maybe multiple times
        {
            if(keep){
                KEEP
            }
        }
    }

    // successful handling
    KEEP_REF
    keep_init = false
    keep = false

catch:
    // this is in the %destructor
    if(keep_init):
        x = KEEP_REF
        dstr_free(x)
    keep_init = false
    keep = false

// the reason KEEP_INIT is separated from KEEP_START is that there are times
// in the grammar where you want to keep "what's next", but "what's next" might
// be an atom or a number, and you don't want to do useless allocations.  For
// example, you might want to keep a flag but that flag might be \Answered,
// which is represented by IE_FLAG_ANSWERED, and needs no memory allocation.

*/

    // indicate we are going to try to keep something
    #define KEEP_START { parser->keep = true; }

    // if parser->keep is set, get ready to keep chunks of whatever-it-is
    #define KEEP_INIT \
        if(parser->keep) DOCATCH( keep_init(parser) ); \
        parser->keep_init = true \

    // if parser->keep is set, store another chunk of whatever-it-is
    #define KEEP(type) \
        if(parser->keep) DOCATCH( keep(parser, KEEP_ ## type) );

    // fetch a reference to whatever we just kept
    #define KEEP_REF(prekeep) do_keep_ref(parser, prekeep)

    // (not a macro because we need to set variables AND return a value)
    // (prekeep argument enforces that KEEP_REF corresponds to a KEEP_START)
    static inline dstr_t do_keep_ref(imap_parser_t *parser, void *prekeep){
        (void)prekeep;
        // decide what to return
        dstr_t retval = NUL_DSTR;
        if(parser->keep_init){
            retval = keep_ref(parser);
        }
        // reset state
        parser->keep = false;
        parser->keep_init = false;
        return retval;
    }

    // called in the %destructor for prekeep
    #define KEEP_CANCEL \
        if(parser->keep_init){ \
            dstr_t freeme = keep_ref(parser); \
            dstr_free(&freeme); \
        } \
        parser->keep = false; \
        parser->keep_init = false

    // the *HOOK* macros are for calling a hooks, then freeing memory
    #define ST_HOOK(tag, st){ \
        parser->hooks_up.status_type(parser->hook_data, &tag, st.status, \
                                     st.code, st.code_extra, &st.text ); \
        dstr_free(&tag); \
        dstr_free(&st.text); \
    }

    #define CAPA_HOOK_START \
        DOCATCH( parser->hooks_up.capa_start(parser->hook_data) );
    #define CAPA_HOOK(c) \
        DOCATCH( parser->hooks_up.capa(parser->hook_data, &c) ); \
        dstr_free(&c);
    #define CAPA_HOOK_END(success) \
        parser->hooks_up.capa_end(parser->hook_data, success);

    #define PFLAG_HOOK_START \
        DOCATCH( parser->hooks_up.pflag_start(parser->hook_data) );
    #define PFLAG_HOOK(f) \
        DOCATCH( parser->hooks_up.pflag(parser->hook_data, f.type, &f.dstr) ); \
        dstr_free(&f.dstr);
    #define PFLAG_HOOK_END(success) \
        parser->hooks_up.pflag_end(parser->hook_data, success);

    #define LIST_HOOK_START \
        DOCATCH( parser->hooks_up.list_start(parser->hook_data) );
    #define LIST_HOOK_FLAG(f) \
        DOCATCH( parser->hooks_up.list_flag(parser->hook_data, f.type, &f.dstr) ); \
        dstr_free(&f.dstr);
    #define LIST_HOOK_END(sep, inbox, mbx, success) \
        parser->hooks_up.list_end(parser->hook_data, sep, inbox, mbx, success);
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

/* FLAGS */
%token ANSWERED
%token FLAGGED
%token DELETED
%token SEEN
%token DRAFT
%token NOSELECT
%token MARKED
%token UNMARKED
/*     RECENT (listed above) */
%token ASTERISK_FLAG

/* miscellaneous */
%token INBOX
%token TEXT
%token EOL

/* non-terminals with semantic values */
%type <dstr> keep_atom
/*
%type <dstr> keep_qstring
%type <dstr> keep_string
%type <dstr> keep_astr_atom
*/
%type <dstr> tag
%type <dstr> st_txt_0
%type <dstr> st_txt_1
%destructor { dstr_free(& $$); } <dstr>

%type <flag_type> flag
/* %type <flag_type> fflag */
%type <flag_type> mflag
%type <flag_type> pflag
%type <flag> keep_mflag
%type <flag> keep_pflag
%destructor { dstr_free(& $$.dstr); } <flag>

%type <num> sc_num

%type <boolean> mailbox
%type <mailbox> keep_mailbox
%destructor { dstr_free(& $$.dstr); } <mailbox>

%type <ch> nqchar

%type <status_type> status_type_resp
%type <status_type> status_type
%type <status_type> status_type_
%type <status_type> status_code
%type <status_type> status_code_
%type <status_type> status_extra

/* dummy types, to take advantage of %destructor to call hooks */

%type <prekeep> prekeep
%type <prekeep> sc_prekeep
%destructor { KEEP_CANCEL; } <prekeep>

%type <capa> capa_start
%destructor { CAPA_HOOK_END(false); } <capa>

%type <permflag> pflag_start
%destructor { PFLAG_HOOK_END(false); } <permflag>

%type <listresp> pre_list_resp
%destructor { LIST_HOOK_END(0, false, NULL, false); } <listresp>


%% /********** Grammar Section **********/

response: tagged EOL { printf("response!\n"); ACCEPT; };
        | untagged EOL { printf("response!\n"); ACCEPT; };

tagged: tag SP status_type_resp[s]   { ST_HOOK($tag, $s); };

untagged: untag SP status_type_resp[s] { ST_HOOK(NUL_DSTR, $s); }
        | untag SP CAPA SP capa_resp
        | untag SP LIST SP list_resp
        | untag SP LSUB SP list_resp
        | untag SP STATUS SP mailbox '(' status_att_list ')'
        | untag SP FLAGS SP '(' /* TODO: flag_list */ ')'
        | untag SP SEARCH SP num_list
        | untag SP NUM SP post_num
;

untag: '*' { MODE(COMMAND); };

/*** LIST responses ***/
list_resp: pre_list_resp '(' list_flags ')' SP
           { MODE(NQCHAR); } nqchar
           { MODE(MAILBOX); } SP keep_mailbox[m]
           { LIST_HOOK_END($nqchar, $m.inbox, & $m.dstr, true); (void)$1; };

pre_list_resp: %empty { LIST_HOOK_START; MODE(FLAG); $$ = NULL; };

list_flags: keep_mflag                  { LIST_HOOK_FLAG($keep_mflag); }
          | list_flags SP keep_mflag    { LIST_HOOK_FLAG($keep_mflag); }
;


/* either the literal NIL or a DQUOTE QUOTED-CHAR DQUOTE */
nqchar: NIL             { $$ = 0; }
      | '"' QCHAR '"'   { $$ = qchar_to_char; }
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

msg_att: FLAGS '(' /* TODO: flag_list */ ')'
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
no_st_code: NO_STATUS_CODE      { MODE(STATUS_TEXT); KEEP_INIT; KEEP(TEXT); };

status_code: status_code_ ']' SP    { MODE(STATUS_TEXT); $$ = $1; };

status_code_: sc_alert           { $$ = ST_CODE(ALERT,      0); }
| CAPA SP capa_resp              { $$ = ST_CODE(CAPA,       0); }
| PARSE                          { $$ = ST_CODE(PARSE,      0); }
| PERMFLAGS SP pflag_resp        { $$ = ST_CODE(PERMFLAGS,  0); }
| READ_ONLY                      { $$ = ST_CODE(READ_ONLY,  0); }
| READ_WRITE                     { $$ = ST_CODE(READ_WRITE, 0); }
| TRYCREATE                      { $$ = ST_CODE(TRYCREATE,  0); }
| sc_uidnext SP sc_num[n]        { $$ = ST_CODE(UIDNEXT,    $n); }
| sc_uidvld SP sc_num[n]         { $$ = ST_CODE(UIDVLD,     $n); }
| sc_unseen SP sc_num[n]         { $$ = ST_CODE(UNSEEN,     $n); }
| sc_atom st_txt_inner_0         { $$ = ST_CODE(ATOM,       0); }
;

sc_alert: ALERT { parser->keep_st_text = true; };

sc_uidnext: UIDNEXT            { MODE(NUM); };
sc_uidvld: UIDVLD              { MODE(NUM); };
sc_unseen: UNSEEN              { MODE(NUM); };

sc_atom: atom                  { MODE(STATUS_TEXT); };

sc_num: NUM          { MODE(STATUS_CODE); $$ = $1; };

/* there are several conditions under which we keep the text at the end */
sc_prekeep: %empty      { if(parser->keep_st_text){ KEEP_START; } $$ = NULL; };

st_txt_inner_0: %empty
              | st_txt_inner_0 st_txt_inner_char
;

st_txt_inner_char: ' '
                 | '['
                 | TEXT
;

st_txt_0: %empty                    { $$ = NUL_DSTR; }
        | sc_prekeep st_txt_1_      { $$ = KEEP_REF($sc_prekeep); }
;

st_txt_1: sc_prekeep st_txt_1_      { $$ = KEEP_REF($sc_prekeep); };

st_txt_1_: st_txt_char              { KEEP_INIT; KEEP(TEXT); }
         | st_txt_1_ st_txt_char     { KEEP(TEXT); }
;

st_txt_char: st_txt_inner_char
           | ']'
;

/*** CAPABILITY handling ***/
/* note the (void)$1 is because if there's an error in capa_list, we need to
   be able to trigger CAPA_HOOK_END via the %destructor, therefore capa_start
   has a semantic value we need to explicitly ignore to avoid warnings */
capa_resp: capa_start capa_list { CAPA_HOOK_END(true); (void)$1; };

capa_start: %empty { CAPA_HOOK_START; MODE(ATOM); $$ = NULL; };

capa_list: keep_atom               { CAPA_HOOK($keep_atom); }
         | capa_list SP keep_atom  { CAPA_HOOK($keep_atom); }
;

/*** PERMANENTFLAG handling ***/
/* %destructor is used to guarantee HOOK_END gets called, as with CAPABILITY */
pflag_resp: pflag_start '(' pflag_list_0 ')' { PFLAG_HOOK_END(true); (void)$1; };

pflag_start: %empty { PFLAG_HOOK_START; MODE(FLAG); $$ = NULL; };

pflag_list_0: %empty
             | pflag_list_1
;
pflag_list_1: keep_pflag                     { PFLAG_HOOK($keep_pflag); }
             | pflag_list_1 SP keep_pflag    { PFLAG_HOOK($keep_pflag); }
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
   into the scanner at any given moment. */
atom: atom_body

atom_body: ATOM                 { KEEP_INIT; KEEP(ATOM); }
         | atom_body atom_like  { KEEP(ATOM); }
;

atom_like: ATOM
         | NUM
         | keyword
;

string: qstring
      | literal
;

qstring: '"' { KEEP_INIT; } qstring_body '"';

qstring_body: %empty
            | qstring_body QSTRING      { KEEP(QSTRING); }
;

/* note that LITERAL_END is passed by the application after it finishes reading
   the literal from the stream; it is never returned by the scanner */
literal: literal_start LITERAL_END;

literal_start: LITERAL              { /* TODO: handle literals */ };

/* like with atoms, an number or keword are technically astr_atoms but that
   introduces grammatical ambiguities */
astring: astr_atom
       | string
;

astr_atom: ASTR_ATOM                  { KEEP_INIT; KEEP(ASTR_ATOM); }
         | astr_atom astr_atom_like   { KEEP(ASTR_ATOM); }
;

astr_atom_like: ASTR_ATOM
              | NUM
              | keyword
;

tag: prekeep tag_body      { $$ = KEEP_REF($prekeep); MODE(COMMAND); };

tag_body: TAG           { KEEP_INIT; KEEP(TAG); }
        | tag_body TAG  { KEEP(TAG); }
;

nstring: NIL
       | string
;

/*!re2c
    *               { return E_PARAM; }
    atom_spec       { *type = yych; goto done; }
    literal         { *type = LITERAL; goto done; }
    eol             { *type = EOL; goto done; }

    'answered'      { *type = ANSWERED; goto done; }
    'flagged'       { *type = FLAGGED; goto done; }
    'deleted'       { *type = DELETED; goto done; }
    'seen'          { *type = SEEN; goto done; }
    'draft'         { *type = DRAFT; goto done; }
    'recent'        { *type = RECENT; goto done; }
    "\\\*"          { *type = ASTERISK_FLAG; goto done; }

    atom            { *type = ATOM; goto done; }
*/

flag: '\\' ANSWERED      { $$ = IE_FLAG_ANSWERED; }
    | '\\' FLAGGED       { $$ = IE_FLAG_FLAGGED; }
    | '\\' DELETED       { $$ = IE_FLAG_DELETED; }
    | '\\' SEEN          { $$ = IE_FLAG_SEEN; }
    | '\\' DRAFT         { $$ = IE_FLAG_DRAFT; }
    | '\\' atom          { $$ = IE_FLAG_EXTENSION; }
    | atom               { $$ = IE_FLAG_KEYWORD; }
;

/* "fflag" for "fetch flag"
fflag: flag
     | '\\' RECENT       { $$ = IE_FLAG_RECENT; }
; */

/* 'mflag" for "mailbox flag" */
mflag: flag
     | NOSELECT         { $$ = IE_FLAG_NOSELECT; }
     | MARKED           { $$ = IE_FLAG_MARKED; }
     | UNMARKED         { $$ = IE_FLAG_UNMARKED; }
;

/* "pflag" for "permanent flag" */
pflag: flag
     | ASTERISK_FLAG    { $$ = IE_FLAG_ASTERISK; }
;

mailbox: astring        { $$ = false; /* not an INBOX */ }
       | INBOX          { $$ = true;  /* is an INBOX */ }

;

/* dummy grammar to make sure KEEP_CANCEL gets called in error handling */
prekeep: %empty { KEEP_START; $$ = NULL; };

/* the "keep" variations of the above (except tag, which is always kept) */
keep_atom: prekeep atom { $$ = KEEP_REF($prekeep); };
keep_pflag: prekeep pflag { $$ = (ie_flag_t){$pflag, KEEP_REF($prekeep)}; };
keep_mflag: prekeep mflag { $$ = (ie_flag_t){$mflag, KEEP_REF($prekeep)}; };
keep_mailbox: prekeep mailbox { $$ = (ie_mailbox_t){$mailbox, KEEP_REF($prekeep)}; };
/*
keep_astr_atom: { parser->keep = true; } astr_atom { $$ = KEEP_REF; };
keep_qstring: { parser->keep = true; } qstring { $$ = KEEP_REF; };
keep_string: { parser->keep = true; } string { $$ = KEEP_REF; };
*/

/* lists */

num_list: %empty
        | num_list NUM
;

SP: ' ';
