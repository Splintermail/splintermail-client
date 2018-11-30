%{
    #include <stdio.h>
    #include <imap_parse.h>
    #include <logger.h>
    #include <imap_expression.h>

    #define MODE(m) parser->scan_mode = SCAN_MODE_ ## m

    #define UNQSTRING parser->scan_mode = parser->preqstr_mode;

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

    // the scanner only returns QCHAR with 1- or 2-char matches
    #define qchar_to_char \
        (parser->token->len == 1) \
            ? parser->token->data[0] \
            : parser->token->data[1]

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
        parser->hooks_up.list_end(parser->hook_data, sep, inbox, &mbx, success); \
        dstr_free(&mbx);

    #define LSUB_HOOK_START \
        DOCATCH( parser->hooks_up.lsub_start(parser->hook_data) );
    #define LSUB_HOOK_FLAG(f) \
        DOCATCH( parser->hooks_up.lsub_flag(parser->hook_data, f.type, &f.dstr) ); \
        dstr_free(&f.dstr);
    #define LSUB_HOOK_END(sep, inbox, mbx, success) \
        parser->hooks_up.lsub_end(parser->hook_data, sep, inbox, &mbx, success); \
        dstr_free(&mbx);

    #define FLAGS_HOOK_START \
        DOCATCH( parser->hooks_up.flags_start(parser->hook_data) );
    #define FLAGS_HOOK_FLAG(f) \
        DOCATCH( parser->hooks_up.flags_flag(parser->hook_data, f.type, &f.dstr) ); \
        dstr_free(&f.dstr);
    #define FLAGS_HOOK_END(success) \
        parser->hooks_up.flags_end(parser->hook_data, success);

    // the *dstr_t from status_start hook is valid until just after status_end
    #define STATUS_HOOK_START(mbx) \
        parser->status_mbx = mbx; \
        DOCATCH( parser->hooks_up.status_start(parser->hook_data, \
                                               parser->status_mbx.inbox, \
                                               &parser->status_mbx.dstr) );
    #define STATUS_HOOK(s, n) \
        DOCATCH( parser->hooks_up.status_attr(parser->hook_data, s, n) );
    #define STATUS_HOOK_END(success) \
        parser->hooks_up.status_end(parser->hook_data, success); \
        dstr_free(&parser->status_mbx.dstr);

    #define EXISTS_HOOK(num) \
        parser->hooks_up.exists(parser->hook_data, num);

    #define RECENT_HOOK(num) \
        parser->hooks_up.recent(parser->hook_data, num);

    #define EXPUNGE_HOOK(num) \
        parser->hooks_up.expunge(parser->hook_data, num);

    // fetch-related hooks
    #define FETCH_HOOK_START(num) \
        DOCATCH( parser->hooks_up.fetch_start(parser->hook_data, num) );

    #define F_FLAGS_HOOK_START \
        DOCATCH( parser->hooks_up.f_flags_start(parser->hook_data) );
    #define F_FLAGS_HOOK_FLAG(f) \
        DOCATCH( parser->hooks_up.f_flags_flag(parser->hook_data, \
                                               f.type, &f.dstr) ); \
        dstr_free(&f.dstr);
    #define F_FLAGS_HOOK_END(success) \
        parser->hooks_up.f_flags_end(parser->hook_data, success);

    #define F_RFC822_HOOK_START \
        DOCATCH( parser->hooks_up.f_rfc822_start(parser->hook_data) );
    #define F_RFC822_HOOK_LITERAL { \
        /* get the numbers from the literal, ex: {5}\r\nBYTES */ \
        dstr_t sub = dstr_sub(parser->token, 1, parser->token->len - 3); \
        size_t len; \
        dstr_toul(&sub, &len, 10);\
        DOCATCH( parser->hooks_up.f_rfc822_literal(parser->hook_data, len) ); \
    }
    #define F_RFC822_HOOK_QSTR \
        DOCATCH( parser->hooks_up.f_rfc822_qstr(parser->hook_data, \
                                                parser->token) );
    #define F_RFC822_HOOK_END(success) \
        parser->hooks_up.f_rfc822_end(parser->hook_data, success);

    #define F_UID_HOOK(num) \
        parser->hooks_up.f_uid(parser->hook_data, num);

    #define F_INTDATE_HOOK(imap_time) \
        parser->hooks_up.f_intdate(parser->hook_data, imap_time);

    #define FETCH_HOOK_END(success) \
        parser->hooks_up.fetch_end(parser->hook_data, success);

    // literal hook
    #define LITERAL_HOOK { \
        /* get the numbers from the literal, ex: {5}\r\nBYTES
                                                 ^^^^^^^ -> LITERAL token */ \
        dstr_t sub = dstr_sub(parser->token, 1, parser->token->len - 3); \
        size_t len; \
        dstr_toul(&sub, &len, 10);\
        DOCATCH( parser->hooks_up.literal(parser->hook_data, len, \
                                          parser->keep) ); \
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
%token RAW
%token NIL
%token DIGIT
%token NUM
%token QCHAR
%token LITERAL
%token LITERAL_END

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

/* status attributes */
%token MESSAGES
/*     RECENT (listed above) */
/*     UIDNEXT (listed above) */
/*     UIDVLD (listed above) */
/*     UNSEEN (listed above) */

/* FETCH state */
/*     FLAGS (listed above */
%token ENVELOPE
%token INTDATE
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

/* INTERNALDATE */
%token JAN
%token FEB
%token MAR
%token APR
%token MAY
%token JUN
%token JUL
%token AUG
%token SEP
%token OCT
%token NOV
%token DEC

/* miscellaneous */
%token INBOX
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
%type <flag_type> fflag
%type <flag_type> mflag
%type <flag_type> pflag
%type <flag> keep_flag
%type <flag> keep_fflag
%type <flag> keep_mflag
%type <flag> keep_pflag
%destructor { dstr_free(& $$.dstr); } <flag>

%type <num> num
%type <num> digit
%type <num> twodigit
%type <num> fourdigit
%type <num> sc_num
%type <num> date_month
%type <num> date_day_fixed

%type <sign> sign

%type <boolean> mailbox
%type <mailbox> keep_mailbox
%destructor { dstr_free(& $$.dstr); } <mailbox>

%type <ch> nqchar
%type <ch> keep_qchar

%type <st_attr> st_attr

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

%type <preqstring> preqstring
%destructor { UNQSTRING; } <preqstring>

%type <capa> capa_start
%destructor { CAPA_HOOK_END(false); } <capa>

%type <permflag> pflag_start
%destructor { PFLAG_HOOK_END(false); } <permflag>

%type <listresp> pre_list_resp
%destructor { LIST_HOOK_END(0, false, NUL_DSTR, false); } <listresp>

%type <lsubresp> pre_lsub_resp
%destructor { LSUB_HOOK_END(0, false, NUL_DSTR, false); } <lsubresp>

%type <flagsresp> pre_flags_resp
%destructor { FLAGS_HOOK_END(false); } <flagsresp>

%type <statusresp> pre_status_resp
%destructor { STATUS_HOOK_END(false); } <statusresp>

%type <fetchresp> pre_fetch_resp
%destructor { FETCH_HOOK_END(false); } <fetchresp>

%type <f_flagsresp> pre_f_flags_resp
%destructor { F_FLAGS_HOOK_END(false); } <f_flagsresp>

%type <f_rfc822resp> pre_f_rfc822_resp
%destructor { F_RFC822_HOOK_END(false); } <f_rfc822resp>

%% /********** Grammar Section **********/

response: tagged EOL { printf("response!\n"); ACCEPT; };
        | untagged EOL { printf("response!\n"); ACCEPT; };

tagged: tag SP status_type_resp[s]   { ST_HOOK($tag, $s); };

untagged: untag SP status_type_resp[s] { ST_HOOK(NUL_DSTR, $s); }
        | untag SP CAPA SP capa_resp
        | untag SP LIST SP list_resp
        | untag SP LSUB SP lsub_resp
        | untag SP STATUS SP status_resp
        | untag SP FLAGS SP flags_resp
        | untag SP SEARCH num_list_0 /* ignored; we will never issue a SEARCH */
        | untag SP exists_resp
        | untag SP recent_resp
        | untag SP expunge_resp
        | untag SP fetch_resp
;

untag: '*' { MODE(COMMAND); };

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

/* a valid status-type response with status code input could be two things:
        * OK [ALERT] asdf
             ^^^^^^^ ^^^^---> status code, followed by general text
        * OK [ALERT] asdf
             ^^^^^^^^^^^^---> no status code, followed by general text which
                              just happens to look like general text...
*/

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

/* NO_STATUS_CODE means we got the start of the text */
no_st_code: NO_STATUS_CODE      { MODE(STATUS_TEXT); KEEP_INIT; KEEP(RAW); };

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

sc_num: num          { MODE(STATUS_CODE); $$ = $1; };

/* there are several conditions under which we keep the text at the end */
sc_prekeep: %empty      { if(parser->keep_st_text){ KEEP_START; } $$ = NULL; };

st_txt_inner_0: %empty
              | SP st_txt_inner_1
;

st_txt_inner_1: st_txt_inner_char
              | st_txt_inner_1 st_txt_inner_char
;

st_txt_inner_char: ' '
                 | '['
                 | RAW
;

st_txt_0: %empty                    { $$ = NUL_DSTR; }
        | sc_prekeep st_txt_1_      { $$ = KEEP_REF($sc_prekeep); }
;

st_txt_1: sc_prekeep st_txt_1_      { $$ = KEEP_REF($sc_prekeep); };

st_txt_1_: st_txt_char              { KEEP_INIT; KEEP(RAW); }
         | st_txt_1_ st_txt_char    { KEEP(RAW); }
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

/*** LIST responses ***/
list_resp: pre_list_resp '(' list_flags ')' SP
           { MODE(NQCHAR); } nqchar
           { MODE(MAILBOX); } SP keep_mailbox[m]
           { LIST_HOOK_END($nqchar, $m.inbox, $m.dstr, true); (void)$1; };

pre_list_resp: %empty { LIST_HOOK_START; MODE(FLAG); $$ = NULL; };

list_flags: keep_mflag                  { LIST_HOOK_FLAG($keep_mflag); }
          | list_flags SP keep_mflag    { LIST_HOOK_FLAG($keep_mflag); }
;

nqchar: NIL                 { $$ = 0; }
      | '"' keep_qchar '"'  { $$ = $keep_qchar; };
;

keep_qchar: QCHAR   { $$ = qchar_to_char; }

/*** LSUB responses ***/
lsub_resp: pre_lsub_resp '(' lsub_flags ')' SP
           { MODE(NQCHAR); } nqchar
           { MODE(MAILBOX); } SP keep_mailbox[m]
           { LSUB_HOOK_END($nqchar, $m.inbox, $m.dstr, true); (void)$1; };

pre_lsub_resp: %empty { LSUB_HOOK_START; MODE(FLAG); $$ = NULL; };

lsub_flags: keep_mflag                  { LSUB_HOOK_FLAG($keep_mflag); }
          | lsub_flags SP keep_mflag    { LSUB_HOOK_FLAG($keep_mflag); }
;

/*** STATUS responses ***/
status_resp: pre_status_resp SP '(' st_attr_list_0 ')'
             { STATUS_HOOK_END(true); (void)$1; };

pre_status_resp: { MODE(MAILBOX); } keep_mailbox[m]
                 { STATUS_HOOK_START($m); MODE(ST_ATTR); $$ = NULL; };

st_attr_list_0: %empty
              | st_attr_list_1
;

st_attr_list_1: st_attr[s] SP num[n]                   { STATUS_HOOK($s, $n); }
              | st_attr_list_1 SP st_attr[s] SP num[n] { STATUS_HOOK($s, $n); }
;

st_attr: MESSAGES    { $$ = IE_ST_ATTR_MESSAGES; }
       | RECENT      { $$ = IE_ST_ATTR_RECENT; }
       | UIDNEXT     { $$ = IE_ST_ATTR_UIDNEXT; }
       | UIDVLD      { $$ = IE_ST_ATTR_UIDVLD; }
       | UNSEEN      { $$ = IE_ST_ATTR_UNSEEN; }
;

/*** FLAGS responses ***/
flags_resp: pre_flags_resp '(' flags_flags ')'
            { FLAGS_HOOK_END(true); (void)$1; };

pre_flags_resp: %empty { FLAGS_HOOK_START; MODE(FLAG); $$ = NULL; };

flags_flags: keep_flag                  { FLAGS_HOOK_FLAG($keep_flag); }
           | flags_flags SP keep_flag   { FLAGS_HOOK_FLAG($keep_flag); }
;

/*** EXISTS responses ***/
exists_resp: num SP EXISTS { EXISTS_HOOK($num); };

/*** RECENT responses ***/
recent_resp: num SP RECENT { RECENT_HOOK($num); };

/*** EXPUNGE responses ***/
expunge_resp: num SP EXPUNGE { EXPUNGE_HOOK($num); };

/*** FETCH responses ***/

fetch_resp: pre_fetch_resp SP '(' msg_attr_list_0 ')'
            { FETCH_HOOK_END(true); (void)$1; }
;

pre_fetch_resp: num SP FETCH
                { FETCH_HOOK_START($num); $$ = NULL; MODE(MSG_ATTR); };

msg_attr_list_0: %empty
               | msg_attr_list_1
;

msg_attr_list_1: msg_attr
               | msg_attr_list_1 SP msg_attr
;

/* most of these get ignored completely, we only really need:
     - FLAGS,
     - UID,
     - INTERNALDATE,
     - the fully body text
   Anything else is going to be encrypted anyway. */
msg_attr: msg_attr_ { MODE(MSG_ATTR); };

msg_attr_: f_flags_resp
         | f_uid_resp
         | f_intdate_resp
         | f_rfc822_resp
         | ENVELOPE SP '(' envelope ')'
         | RFC822_TEXT SP nstring
         | RFC822_HEADER SP nstring
         | RFC822_SIZE SP NUM
         | BODY_STRUCTURE { LOG_ERROR("found BODYSTRUCTURE\n"); ACCEPT; }
         | BODY { LOG_ERROR("found BODYSTRUCTURE\n"); ACCEPT; }
;

/*** FETCH FLAGS ***/
f_flags_resp: pre_f_flags_resp SP '(' f_flags ')'
              { F_FLAGS_HOOK_END(true); (void)$1; };

pre_f_flags_resp: FLAGS { F_FLAGS_HOOK_START; MODE(FLAG); $$ = NULL; };

f_flags: keep_fflag                  { F_FLAGS_HOOK_FLAG($keep_fflag); }
       | flags_flags SP keep_fflag   { F_FLAGS_HOOK_FLAG($keep_fflag); }
;

/*** FETCH RFC822 ***/
f_rfc822_resp: pre_f_rfc822_resp rfc822_nstring
               { F_RFC822_HOOK_END(true); (void)$1; }

pre_f_rfc822_resp: RFC822 SP { F_RFC822_HOOK_START; $$ = NULL; MODE(NSTRING); }

rfc822_nstring: NIL
              | LITERAL { F_RFC822_HOOK_LITERAL; } LITERAL_END
              | '"' { MODE(QSTRING); } rfc822_qstr_body '"'
;

rfc822_qstr_body: RAW                   { F_RFC822_HOOK_QSTR; }
                | rfc822_qstr_body RAW  { F_RFC822_HOOK_QSTR; }
;

/*** FETCH UID ***/
f_uid_resp: UID SP { MODE(NUM); } num { F_UID_HOOK($num); };

/*** FETCH INTERNALDATE ***/
f_intdate_resp: INTDATE SP { MODE(INTDATE); }
                '"' date_day_fixed '-' date_month '-' fourdigit[y] SP
                twodigit[h] ':' twodigit[m] ':' twodigit[s] SP
                sign twodigit[zh] twodigit[zm] '"'
                { F_INTDATE_HOOK(((imap_time_t){.year   = $y,
                                                .month  = $date_month,
                                                .day    = $date_day_fixed,
                                                .hour   = $h,
                                                .min    = $m,
                                                .sec    = $s,
                                                .z_sign = $sign,
                                                .z_hour = $zh,
                                                .z_min  = $zm })); };

date_day_fixed: ' ' digit       { $$ = $digit; }
              | digit digit     { $$ = 10*$1 + $2; }
;

date_month: JAN { $$ = 0; };
          | FEB { $$ = 1; };
          | MAR { $$ = 2; };
          | APR { $$ = 3; };
          | MAY { $$ = 4; };
          | JUN { $$ = 5; };
          | JUL { $$ = 6; };
          | AUG { $$ = 7; };
          | SEP { $$ = 8; };
          | OCT { $$ = 9; };
          | NOV { $$ = 10; };
          | DEC { $$ = 11; };


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

atom_body: RAW                  { KEEP_INIT; KEEP(RAW); }
         | atom_body atom_like  { KEEP(RAW); }
;

atom_like: RAW
         | NUM
         | keyword
;

string: qstring
      | literal
;

qstring: '"' preqstring qstring_body '"' { UNQSTRING; (void)$preqstring; };

preqstring: %empty { parser->preqstr_mode = parser->scan_mode;
                     MODE(QSTRING);
                     KEEP_INIT;
                     $$ = NULL; }

qstring_body: %empty
            | qstring_body atom_like    { KEEP(QSTRING); }
;

/* note that LITERAL_END is passed by the application after it finishes reading
   the literal from the stream; it is never returned by the scanner */
literal: LITERAL { LITERAL_HOOK; } LITERAL_END;

astring: atom
       | string
;

tag: prekeep atom      { $$ = KEEP_REF($prekeep); MODE(COMMAND); };

nstring: NIL
       | string
;

flag: '\\' ANSWERED      { $$ = IE_FLAG_ANSWERED; }
    | '\\' FLAGGED       { $$ = IE_FLAG_FLAGGED; }
    | '\\' DELETED       { $$ = IE_FLAG_DELETED; }
    | '\\' SEEN          { $$ = IE_FLAG_SEEN; }
    | '\\' DRAFT         { $$ = IE_FLAG_DRAFT; }
    | '\\' atom          { $$ = IE_FLAG_EXTENSION; }
    | atom               { $$ = IE_FLAG_KEYWORD; }
;

/* "fflag" for "fetch flag" */
fflag: flag
     | '\\' RECENT       { $$ = IE_FLAG_RECENT; }
;

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

sign: '+' { $$ = 1; }
    | '-' { $$ = 2; }
;

digit: DIGIT { switch(parser->token->data[0]){
                   case '0': $$ = 0; break;
                   case '1': $$ = 1; break;
                   case '2': $$ = 2; break;
                   case '3': $$ = 3; break;
                   case '4': $$ = 4; break;
                   case '5': $$ = 5; break;
                   case '6': $$ = 6; break;
                   case '7': $$ = 7; break;
                   case '8': $$ = 8; break;
                   case '9': $$ = 9; break;
                   default: ACCEPT;
               }
             };

twodigit: digit digit { $$ = 10*$1 + $2; };

fourdigit: digit digit digit digit { $$ = 1000*$1 + 100*$2 + 10*$3 + $4; };

num: NUM { dstr_tou(parser->token, & $$, 10); };

num_list_0: %empty
          | num_list_1
;

num_list_1: NUM
          | num_list_1 SP NUM
;

/* dummy grammar to make sure KEEP_CANCEL gets called in error handling */
prekeep: %empty { KEEP_START; $$ = NULL; };

/* the "keep" variations of the above (except tag, which is always kept) */
keep_atom: prekeep atom { $$ = KEEP_REF($prekeep); };
keep_flag: prekeep flag { $$ = (ie_flag_t){$flag, KEEP_REF($prekeep)}; };
keep_fflag: prekeep fflag { $$ = (ie_flag_t){$fflag, KEEP_REF($prekeep)}; };
keep_pflag: prekeep pflag { $$ = (ie_flag_t){$pflag, KEEP_REF($prekeep)}; };
keep_mflag: prekeep mflag { $$ = (ie_flag_t){$mflag, KEEP_REF($prekeep)}; };
keep_mailbox: prekeep mailbox { $$ = (ie_mailbox_t){$mailbox, KEEP_REF($prekeep)}; };
/*
keep_astr_atom: { parser->keep = true; } astr_atom { $$ = KEEP_REF; };
keep_qstring: { parser->keep = true; } qstring { $$ = KEEP_REF; };
keep_string: { parser->keep = true; } string { $$ = KEEP_REF; };
*/

SP: ' ';
