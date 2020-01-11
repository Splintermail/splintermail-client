%{
    #include <stdio.h>
    #include <imap_parse.h>
    #include <imap_expression.h>
    #include <imap_extension.h>
    #include <imap_scan.h>
    #include <logger.h>

    #define YYTOKENTYPE // imap_scan_token_type_t

    #define MODE(m) p->scan_mode = SCAN_MODE_ ## m

    #define E (&p->error)

    // a YYACCEPT wrapper that resets some custom parser details
    #define ACCEPT \
        MODE(TAG); \
        YYACCEPT

    // must be a macro because it has to be able to call YYERROR
    #define LITERAL_START { \
        /* get the numbers from the literal, ex: {5}\r\nBYTES */ \
        /*                                       ^^^^^^^ -> LITERAL token */ \
        dstr_t sub = dstr_sub(p->token, 1, p->token->len - 3); \
        unsigned int len; \
        derr_t e = dstr_tou(&sub, &len, 10); \
        /* an error here is a proper syntax error */ \
        if(is_error(e)){ \
            DROP_VAR(&e); \
            yyerror(p, "invalid number"); \
            YYERROR; \
        } \
        /* put scanner in literal mode */ \
        set_scanner_to_literal_mode(p, len); \
    }

    #define PARSE_NUM(out){ \
        unsigned int num ; \
        derr_t e = dstr_tou(p->token, &num, 10); \
        /* an error here is a proper syntax error */ \
        if(is_error(e)){ \
            DROP_VAR(&e); \
            yyerror(p, "invalid number"); \
            YYERROR; \
        } \
        (out) = num; \
    }

    #define PARSE_NZNUM(out){ \
        PARSE_NUM(out); \
        if((out) == 0){ \
            yyerror(p, "invalid number (zero)"); \
            YYERROR; \
        } \
    }

    #define PARSE_MODSEQNUM(out){ \
        unsigned long num ; \
        derr_t e = dstr_toul(p->token, &num, 10); \
        /* an error here is a proper syntax error */ \
        if(is_error(e)){ \
            DROP_VAR(&e); \
            yyerror(p, "invalid number"); \
            YYERROR; \
        } \
        /* 63-bit number; maximum value is 2^63 - 1 */ \
        if((num) > 9223372036854775807UL){ \
            yyerror(p, "invalid number (too big)"); \
            YYERROR; \
        } \
        (out) = num; \
    }

    #define PARSE_NZMODSEQNUM(out){ \
        PARSE_MODSEQNUM(out); \
        if((out) == 0){ \
            yyerror(p, "invalid number (zero)"); \
            YYERROR; \
        } \
    }

    // make sure we don't get two selectability flags
    #define MFLAG_SELECT(out, _mf, selectability) { \
        /* _mf might be a function; just call it once */ \
        ie_mflags_t *mf = (_mf); \
        if(is_error(p->error)){ \
            /* some other error occured */ \
            ie_mflags_free(mf); \
            (out) = NULL; \
        }else{ \
            if(mf->selectable != IE_SELECTABLE_NONE){ \
                /* it's a syntax error to have multiple selectability flags */ \
                ie_mflags_free(mf); \
                (out) = NULL; \
                yyerror(p, "multiple selectability flags"); \
                YYERROR; \
            }else{ \
                mf->selectable = selectability; \
                (out) = mf; \
            } \
        } \
    }

    // the scanner only returns QCHAR with 1- or 2-char matches.
    // the 2-char matches are \\ and \"
    #define QCHAR_TO_CHAR \
        (p->token->len == 1) ? p->token->data[0] : p->token->data[1]

%}

/* this defines the type of yylval, which is the semantic value of a token */
%define api.value.type {imap_expr_t}
/* reentrant */
%define api.pure full
/* push parser */
%define api.push-pull push
/* create a user-data pointer in the api */
%parse-param { imap_parser_t *p }
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

/* commands */
%token STARTTLS
%token AUTHENTICATE
%token LOGIN
%token SELECT
%token EXAMINE
%token CREATE
%token DELETE
%token RENAME
%token SUBSCRIBE
%token UNSUBSCRIBE
%token LIST
%token LSUB
%token STATUS
%token APPEND
%token CHECK
%token CLOSE
%token EXPUNGE
%token SEARCH
%token FETCH
%token STORE
%token COPY
%token UID
%token ENABLE

/* responses */
%token OK
%token NO
%token BAD
%token PREAUTH
%token BYE
%token CAPA
/*     LIST (listed above) */
/*     LSUB (listed above) */
/*     STATUS (listed above) */
%token FLAGS
/*     SEARCH (listed above) */
%token EXISTS
%token RECENT
/*     EXPUNGE (listed above) */
/*     FETCH (listed above) */
%token ENABLED

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

/* SEARCH state */
%token CHARSET
%token ALL
%token ANSWERED
%token BCC
%token BEFORE
%token BODY
%token CC
%token DELETED
%token FLAGGED
%token FROM
%token KEYWORD
%token NEW
%token OLD
%token ON
/*     RECENT (listed above) */
%token SEEN
%token SINCE
%token SUBJECT
%token TEXT
%token TO
%token UNANSWERED
%token UNDELETED
%token UNFLAGGED
%token UNKEYWORD
/*     UNSEEN (listed above) */
%token DRAFT
%token HEADER
%token LARGER
%token NOT
%token OR
%token SENTBEFORE
%token SENTON
%token SENTSINCE
%token SMALLER
/*     UID (listed above) */
%token UNDRAFT

/* FETCH state */
/*     FLAGS (listed above */
/*     ALL (listed above */
%token FULL
%token FAST
%token ENVELOPE
%token INTDATE
%token RFC822
%token RFC822_TEXT
%token RFC822_HEADER
%token RFC822_SIZE
%token BODYSTRUCT
/*     BODY (listed above */
%token BODY_PEEK
/*     UID (listed above) */

/* BODY[] section stuff */
%token MIME
/*     TEXT (listed above) */
/*     HEADER (listed above) */
%token HDR_FLDS
%token HDR_FLDS_NOT

/* FLAGS */
/*     ANSWERED (listed above) */
/*     FLAGGED (listed above) */
/*     DELETED (listed above) */
/*     SEEN (listed above) */
/*     DRAFT (listed above) */
/*     RECENT (listed above) */
%token ASTERISK_FLAG

/* mailbox-specific flags */
%token NOINFERIORS
%token NOSELECT
%token MARKED
%token UNMARKED

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
%token SILENT

/* UIDPLUS extension */
%token APPENDUID
%token COPYUID
%token UIDNOSTICK

/* CONDSTORE extension */
%token MODSEQ
%token HIMODSEQ
%token NOMODSEQ
%token MODIFIED
%token CONDSTORE
%token CHGSINCE
%token UNCHGSINCE
%token PRIV
/* %token ALL */
%token SHARED

/* QRESYNC extension */
%token CLOSED
%token VANISHED
%token EARLIER
%token QRESYNC

%type <ch> qchar
%type <ch> nqchar
// no destructor for char type

%type <dstr> tag
%type <dstr> atom
%type <dstr> qstr
%type <dstr> qstr_body
%type <dstr> literal
%type <dstr> literal_body
%type <dstr> astring
%type <dstr> string
%type <dstr> search_charset
%type <dstr> search_astring
%type <dstr> search_atom
%type <dstr> header_list_1
%type <dstr> st_text
%type <dstr> sc_text
%type <dstr> sc_text_
%type <dstr> capas_1
%type <dstr> f_rfc822
%type <dstr> rfc822_nstring
%destructor { ie_dstr_free($$); } <dstr>

%type <mailbox> mailbox
%destructor { ie_mailbox_free($$); } <mailbox>

%type <select_params> select_param
%type <select_params> select_params_0
%type <select_params> select_params_1
%type <select_params> qresync_param
%destructor { ie_select_params_free($$); } <select_params>

%type <qresync_required> qresync_required
// no destructor needed

%type <qresync_opt> qresync_opt
%type <qresync_opt> qresync_map
%destructor {
    ie_seq_set_free($$.u);
    ie_seq_set_free($$.k);
    ie_seq_set_free($$.v);
} <qresync_opt>

%type <status_attr> s_attr_any
%type <status_attr> s_attr_32
%type <status_attr> s_attr_64
// no destructor needed

// clist = "command" list, a logical OR of some flags
%type <status_attr_cmd> s_attr_clist_1
// no destructor needed

// rlist = "response" list; a logical OR of flags but with integer arguments
%type <status_attr_resp> s_attr_resp
%type <status_attr_resp> s_attr_rlist_0
%type <status_attr_resp> s_attr_rlist_1
// no destructor needed

%type <time> search_date;
%type <time> date_time;
%type <time> date;
%type <time> append_time;
%type <time> f_intdate;

%type <sign> sign
%type <sign> store_sign

%type <num> num
%type <num> nznum
%type <num> digit
%type <num> twodigit
%type <num> fourdigit
%type <num> sc_num
%type <num> date_month
%type <num> date_day
%type <num> date_day_fixed
%type <num> seq_num
%type <num> f_uid

%type <modseqnum> modseqnum
%type <modseqnum> nzmodseqnum
%type <modseqnum> f_modseq

%type <flag> flag_simple

%type <flags> flags_0
%type <flags> flags_1
%type <flags> append_flags
%type <flags> store_flags
%destructor { ie_flags_free($$); } <flags>

%type <pflag> pflag_simple

%type <pflags> pflags
%type <pflags> pflags_0
%type <pflags> pflags_1
%destructor { ie_pflags_free($$); } <pflags>

%type <fflag> fflag_simple

%type <fflags> f_fflags
%type <fflags> fflags_0
%type <fflags> fflags_1
%destructor { ie_fflags_free($$); } <fflags>

%type <selectable> mflag_select

%type <mflags> mflags
%type <mflags> mflags_0
%type <mflags> mflags_1
%destructor { ie_mflags_free($$); } <mflags>

%type <boolean> uid_mode
%type <boolean> store_silent

%type <seq_set> seq_spec
%type <seq_set> seq_set
%type <seq_set> uid_spec
%type <seq_set> uid_set
%destructor { ie_seq_set_free($$); } <seq_set>

%type <nums> nums_1
%destructor { ie_nums_free($$); } <nums>

%type <search_key> search_key
%type <search_key> search_keys_1
%type <search_key> search_hdr
%type <search_key> search_or
%type <search_key> search_modseq
%destructor { ie_search_key_free($$); } <search_key>

%type <search_modseq_ext> search_modseq_ext
%destructor { ie_search_modseq_ext_free($$); } <search_modseq_ext>

%type <entry_type> entry_type
// no destructor for entry_type

%type <sect_part> sect_part
%destructor { ie_sect_part_free($$); } <sect_part>

%type <sect_txt> sect_txt
%type <sect_txt> sect_msgtxt
%destructor { ie_sect_txt_free($$); } <sect_txt>

%type <sect> sect
%type <sect> _sect
%destructor { ie_sect_free($$); } <sect>

%type <partial> partial
%destructor { ie_partial_free($$); } <partial>

%type <fetch_simple> fetch_attr_simple
// no destructor; it's just an enum

%type <fetch_extra> fetch_attr_extra
%destructor { ie_fetch_extra_free($$); } <fetch_extra>

%type <fetch_attrs> fetch_attr
%type <fetch_attrs> fetch_attrs
%type <fetch_attrs> fetch_attrs_1
%destructor { ie_fetch_attrs_free($$); } <fetch_attrs>

%type <fetch_mods> fetch_mods_0
%type <fetch_mods> fetch_mods_1
%type <fetch_mods> fetch_mod
%destructor { ie_fetch_mods_free($$); } <fetch_mods>

%type <store_mods> store_mods_0
%type <store_mods> store_mods_1
%type <store_mods> store_mod
%destructor { ie_store_mods_free($$); } <store_mods>

%type <status> st_type
// no destructor; it's just an enum

%type <st_code> st_code
%type <st_code> st_code_
%type <st_code> sc_capa
%type <st_code> sc_pflags
%type <st_code> sc_atom
//
%type <st_code> sc_uidnostick
%type <st_code> sc_appenduid
%type <st_code> sc_copyuid
//
%type <st_code> sc_nomodseq
%type <st_code> sc_himodseq
%type <st_code> sc_modified
%destructor { ie_st_code_free($$); } <st_code>

%type <fetch_resp> msg_attrs_0
%type <fetch_resp> msg_attrs_1
%destructor { ie_fetch_resp_free($$); } <fetch_resp>


%type <imap_cmd> command_
%type <imap_cmd> starttls_cmd
%type <imap_cmd> authenticate_cmd
%type <imap_cmd> login_cmd
%type <imap_cmd> select_cmd
%type <imap_cmd> examine_cmd
%type <imap_cmd> create_cmd
%type <imap_cmd> delete_cmd
%type <imap_cmd> rename_cmd
%type <imap_cmd> subscribe_cmd
%type <imap_cmd> unsubscribe_cmd
%type <imap_cmd> list_cmd
%type <imap_cmd> lsub_cmd
%type <imap_cmd> status_cmd
%type <imap_cmd> append_cmd
%type <imap_cmd> check_cmd
%type <imap_cmd> close_cmd
%type <imap_cmd> expunge_cmd
%type <imap_cmd> search_cmd
%type <imap_cmd> fetch_cmd
%type <imap_cmd> store_cmd
%type <imap_cmd> copy_cmd
%type <imap_cmd> enable_cmd
%destructor { imap_cmd_free($$); } <imap_cmd>

%type <imap_resp> response_
%type <imap_resp> untagged_resp
%type <imap_resp> status_type_resp_tagged
%type <imap_resp> status_type_resp_untagged
%type <imap_resp> capa_resp
%type <imap_resp> list_resp
%type <imap_resp> lsub_resp
%type <imap_resp> status_resp
%type <imap_resp> flags_resp
%type <imap_resp> search_resp
%type <imap_resp> exists_resp
%type <imap_resp> recent_resp
%type <imap_resp> expunge_resp
%type <imap_resp> fetch_resp
%type <imap_resp> enabled_resp
%type <imap_resp> vanished_resp
%destructor { imap_resp_free($$); } <imap_resp>

%% /********** Grammar Section **********/

line: command EOL { ACCEPT; }
    | response EOL { ACCEPT; }
;

command: command_[c]
{
    // extract the error (if any), since are about to pass it by value
    derr_t error = p->error;
    PASSED(p->error);
    p->cb.cmd(p->cb_data, error, $c);
};

command_: starttls_cmd
        | authenticate_cmd
        | login_cmd
        | select_cmd
        | examine_cmd
        | create_cmd
        | delete_cmd
        | rename_cmd
        | subscribe_cmd
        | unsubscribe_cmd
        | list_cmd
        | lsub_cmd
        | status_cmd
        | append_cmd
        | check_cmd
        | close_cmd
        | expunge_cmd
        | search_cmd
        | fetch_cmd
        | store_cmd
        | copy_cmd
        | enable_cmd
;

response: response_[r]
{
    // extract the error (if any), since are about to pass it by value
    derr_t error = p->error;
    PASSED(p->error);
    p->cb.resp(p->cb_data, error, $r);
};

response_: status_type_resp_tagged
         | untag SP untagged_resp[u] { $$ = $u; }
;

untagged_resp: status_type_resp_untagged
             | capa_resp
             | list_resp
             | lsub_resp
             | status_resp
             | flags_resp
             | search_resp
             | exists_resp
             | recent_resp
             | expunge_resp
             | fetch_resp
             | enabled_resp
             | vanished_resp
;

untag: '*' { MODE(COMMAND); };

/*** STARTTLS command ***/

starttls_cmd: tag SP STARTTLS
    { $$ = imap_cmd_new(E, $tag, IMAP_CMD_STARTTLS, (imap_cmd_arg_t){0}); };

/*** AUTHENTICATE command ***/
authenticate_cmd: tag SP AUTHENTICATE SP atom
    { imap_cmd_arg_t arg = {.auth=$atom};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_AUTH, arg); };

/*** LOGIN command ***/

login_cmd: tag SP LOGIN SP { MODE(ASTRING); } astring[u] SP astring[p]
    { imap_cmd_arg_t arg = {.login=ie_login_cmd_new(E, $u, $p)};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_LOGIN, arg); };

/*** SELECT command ***/

select_cmd: tag SP SELECT SP mailbox[m] select_params_0[p]
    { imap_cmd_arg_t arg = {.select=ie_select_cmd_new(E, $m, $p)};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_SELECT, arg); };

select_params_0: %empty                                                { $$ = NULL; }
               | SP { MODE(SELECT_PARAM); } '(' select_params_1[p] ')' { $$ = $p; }
;

select_params_1: select_param[p]                        { $$ = $p; }
               | select_params_1[l] SP select_param[p]  { $$ = ie_select_params_add(E, $l, $p); }
;

select_param: CONDSTORE
                { /* this is a CONDSTORE-enabling command */
                  extension_trigger_builder(E, p->exts, EXT_CONDSTORE);
                  ie_select_param_arg_t arg = {0};
                  $$ = ie_select_params_new(E, IE_SELECT_PARAM_CONDSTORE, arg); }
            | QRESYNC SP '(' qresync_param[p] ')'
                { extension_assert_on_builder(E, p->exts, EXT_QRESYNC); $$ = $p; }
;

qresync_param: qresync_required[r] qresync_opt[opt]
    { ie_select_param_arg_t arg = {.qresync={
          .uidvld = $r.uidvld,
          .last_modseq = $r.last_modseq,
          .known_uids = $opt.u,
          .seq_keys = $opt.k,
          .uid_vals = $opt.v,
      }};
      $$ = ie_select_params_new(E, IE_SELECT_PARAM_QRESYNC, arg); };

qresync_required: nznum[v] SP nzmodseqnum[m] { $$.uidvld = $v; $$.last_modseq = $m; };

qresync_opt: %empty                                  { $$.u=NULL; $$.k=NULL; $$.v=NULL; }
           | SP uid_set[u]                           { $$.u=$u;   $$.k=NULL; $$.v=NULL; }
           | SP uid_set[u] SP '(' qresync_map[m] ')' { $$.u=$u;   $$.k=$m.k; $$.v=$m.v; }
           | SP '(' qresync_map[m] ')'               { $$.u=NULL; $$.k=$m.k; $$.v=$m.v; }

qresync_map: %empty                                  { $$.u=NULL; $$.k=NULL; $$.v=NULL; }
           | uid_set[k] SP uid_set[v]                { $$.u=NULL; $$.k=$k;   $$.v=$v; }
;


/*** EXAMINE command ***/

examine_cmd: tag SP EXAMINE SP mailbox[m] { MODE(SELECT_PARAM); } select_params_0[p]
    { imap_cmd_arg_t arg = {.examine=ie_select_cmd_new(E, $m, $p)};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_EXAMINE, arg); };

/*** CREATE command ***/

create_cmd: tag SP CREATE SP mailbox[m]
    { imap_cmd_arg_t arg = {.create=$m};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_CREATE, arg); };

/*** DELETE command ***/

delete_cmd: tag SP DELETE SP mailbox[m]
    { imap_cmd_arg_t arg = {.delete=$m};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_DELETE, arg); };

/*** RENAME command ***/

rename_cmd: tag SP RENAME SP mailbox[o] SP mailbox[n]
    { imap_cmd_arg_t arg = {.rename=ie_rename_cmd_new(E, $o, $n)};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_RENAME, arg); };

/*** SUBSCRIBE command ***/

subscribe_cmd: tag SP SUBSCRIBE SP mailbox[m]
    { imap_cmd_arg_t arg = {.sub=$m};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_SUB, arg); };

/*** UNSUBSCRIBE command ***/

unsubscribe_cmd: tag SP UNSUBSCRIBE SP mailbox[m]
    { imap_cmd_arg_t arg = {.unsub=$m};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_UNSUB, arg); };

/*** LIST command ***/

list_cmd: tag SP LIST SP mailbox[m] { MODE(WILDCARD); } SP astring[p]
    { imap_cmd_arg_t arg = {.list=ie_list_cmd_new(E, $m, $p)};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_LIST, arg); };

/*** LSUB command ***/

lsub_cmd: tag SP LSUB SP mailbox[m] { MODE(WILDCARD); } SP astring[p]
    { imap_cmd_arg_t arg = {.lsub=ie_list_cmd_new(E, $m, $p)};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_LSUB, arg); };

/*** STATUS command ***/

status_cmd: tag SP STATUS SP mailbox[m]
          { MODE(STATUS_ATTR); } SP '(' s_attr_clist_1[sa] ')'
    { imap_cmd_arg_t arg = {.status=ie_status_cmd_new(E, $m, $sa)};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_STATUS, arg); };

s_attr_clist_1: s_attr_any[s]                        { $$ = $s; }
              | s_attr_clist_1[old] SP s_attr_any[s] { $$ = $old | $s; }
;

/*** APPEND command ***/

append_cmd: tag SP APPEND SP mailbox[m]
          { MODE(FLAG); } SP append_flags[f] append_time[t]
          { MODE(ASTRING); } literal[l]
    { imap_cmd_arg_t arg = {.append=ie_append_cmd_new(E, $m, $f, $t, $l)};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_APPEND, arg); };

append_flags: %empty                { $$ = NULL; }
            | '(' flags_0[f] ')' SP { $$ = $f; }
;

append_time: %empty             { $$ = (imap_time_t){0}; }
           | date_time[dt] SP   { $$ = $dt; }
;

/*** CHECK command ***/

check_cmd: tag[t] SP CHECK
    { $$ = imap_cmd_new(E, $t, IMAP_CMD_CHECK, (imap_cmd_arg_t){0}); };

/*** CLOSE command ***/

close_cmd: tag[t] SP CLOSE
    { $$ = imap_cmd_new(E, $t, IMAP_CMD_CLOSE, (imap_cmd_arg_t){0}); };

/*** EXPUNGE command ***/

expunge_cmd: tag[t] SP EXPUNGE
                { imap_cmd_arg_t arg = {0};
                  $$ = imap_cmd_new(E, $t, IMAP_CMD_EXPUNGE, arg); }
           | tag[t] SP UID SP EXPUNGE SP uid_set[s]
                { extension_assert_on_builder(E, p->exts, EXT_UIDPLUS);
                  imap_cmd_arg_t arg = {.uid_expunge=$s};
                  $$ = imap_cmd_new(E, $t, IMAP_CMD_EXPUNGE, arg); }
;

/*** UID (modifier of other commands) ***/
uid_mode: %empty { $$ = false; }
        | UID SP { $$ = true; }
;

/*** SEARCH command ***/

search_cmd: tag SP uid_mode[u] SEARCH SP
            { MODE(SEARCH); } search_charset[c]
            { MODE(SEARCH); } search_keys_1[k]
    { imap_cmd_arg_t arg = {.search=ie_search_cmd_new(E, $u, $c, $k)};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_SEARCH, arg); };

search_charset: %empty { $$ = NULL; }
              | CHARSET SP search_astring[s] SP { $$ = $s; MODE(SEARCH); }
;

search_keys_1: search_key[k]                     { $$ = $k; MODE(SEARCH); }
             | search_keys_1[a] SP search_key[b]
               { $$ = ie_search_pair(E, IE_SEARCH_AND, $a, $b); MODE(SEARCH); }
;

search_key: ALL                         { $$ = ie_search_0(E, IE_SEARCH_ALL); }
          | ANSWERED                    { $$ = ie_search_0(E, IE_SEARCH_ANSWERED); }
          | DELETED                     { $$ = ie_search_0(E, IE_SEARCH_DELETED); }
          | FLAGGED                     { $$ = ie_search_0(E, IE_SEARCH_FLAGGED); }
          | NEW                         { $$ = ie_search_0(E, IE_SEARCH_NEW); }
          | OLD                         { $$ = ie_search_0(E, IE_SEARCH_OLD); }
          | RECENT                      { $$ = ie_search_0(E, IE_SEARCH_RECENT); }
          | SEEN                        { $$ = ie_search_0(E, IE_SEARCH_SEEN); }
          | UNANSWERED                  { $$ = ie_search_0(E, IE_SEARCH_UNANSWERED); }
          | UNDELETED                   { $$ = ie_search_0(E, IE_SEARCH_UNDELETED); }
          | UNFLAGGED                   { $$ = ie_search_0(E, IE_SEARCH_UNFLAGGED); }
          | UNSEEN                      { $$ = ie_search_0(E, IE_SEARCH_UNSEEN); }
          | DRAFT                       { $$ = ie_search_0(E, IE_SEARCH_DRAFT); }
          | UNDRAFT                     { $$ = ie_search_0(E, IE_SEARCH_UNDRAFT); }
          | BCC SP search_astring[s]    { $$ = ie_search_dstr(E, IE_SEARCH_BCC, $s); }
          | BODY SP search_astring[s]   { $$ = ie_search_dstr(E, IE_SEARCH_BODY, $s); }
          | CC SP search_astring[s]     { $$ = ie_search_dstr(E, IE_SEARCH_CC, $s); }
          | FROM SP search_astring[s]   { $$ = ie_search_dstr(E, IE_SEARCH_FROM, $s); }
          | KEYWORD SP search_atom[s]   { $$ = ie_search_dstr(E, IE_SEARCH_KEYWORD, $s); }
          | SUBJECT SP search_astring[s]{ $$ = ie_search_dstr(E, IE_SEARCH_SUBJECT, $s); }
          | TEXT SP search_astring[s]   { $$ = ie_search_dstr(E, IE_SEARCH_TEXT, $s); }
          | TO SP search_astring[s]     { $$ = ie_search_dstr(E, IE_SEARCH_TO, $s); }
          | UNKEYWORD SP search_atom[s] { $$ = ie_search_dstr(E, IE_SEARCH_UNKEYWORD, $s); }
          | search_hdr
          | BEFORE SP search_date[d]    { $$ = ie_search_date(E, IE_SEARCH_BEFORE, $d); }
          | ON SP search_date[d]        { $$ = ie_search_date(E, IE_SEARCH_ON, $d); }
          | SINCE SP search_date[d]     { $$ = ie_search_date(E, IE_SEARCH_SINCE, $d); }
          | SENTBEFORE SP search_date[d]{ $$ = ie_search_date(E, IE_SEARCH_SENTBEFORE, $d); }
          | SENTON SP search_date[d]    { $$ = ie_search_date(E, IE_SEARCH_SENTON, $d); }
          | SENTSINCE SP search_date[d] { $$ = ie_search_date(E, IE_SEARCH_SENTSINCE, $d); }
          | LARGER SP num[n]            { $$ = ie_search_num(E, IE_SEARCH_LARGER, $n); }
          | SMALLER SP num[n]           { $$ = ie_search_num(E, IE_SEARCH_SMALLER, $n); }
          | UID SP seq_set[s]           { $$ = ie_search_seq_set(E, IE_SEARCH_UID, $s); }
          | seq_set[s]                  { $$ = ie_search_seq_set(E, IE_SEARCH_SEQ_SET, $s); }
          | NOT SP search_key[k]        { $$ = ie_search_not(E, $k); }
          | '(' search_keys_1[k] ')'    { $$ = ie_search_group(E, $k); }
          | search_or
          | search_modseq
;

search_hdr: HEADER SP search_astring[h] SP search_astring[v]
          { $$ = ie_search_header(E, IE_SEARCH_HEADER, $h, $v); MODE(SEARCH); };

search_or: OR SP search_key[a] SP search_key[b]
         { $$ = ie_search_pair(E, IE_SEARCH_OR, $a, $b); };

search_modseq: MODSEQ search_modseq_ext[ext] modseqnum[s]
    { extension_trigger_builder(E, p->exts, EXT_CONDSTORE);
      $$ = ie_search_modseq(E, $ext, $s); };

search_modseq_ext: SP                             { $$ = NULL; }
                 | SP qstr[n] SP entry_type[t] SP { $$ = ie_search_modseq_ext_new(E, $n, $t); }
;

entry_type: PRIV    { $$ = IE_ENTRY_PRIV; }
          | SHARED  { $$ = IE_ENTRY_SHARED; }
          | ALL     { $$ = IE_ENTRY_ALL; }
;

search_astring: { MODE(ASTRING); } astring[k] { $$ = $k; };

search_atom: { MODE(ATOM); } atom[k] { $$ = $k; };

search_date: pre_date '"' date '"' { $$ = $date; }
           | pre_date date         { $$ = $date; }
;

date_day: digit           { $$ = $digit; }
        | digit digit     { $$ = 10*$1 + $2; }
;

pre_date: %empty { MODE(DATETIME); };

date: date_day '-' date_month '-' fourdigit
      { $$ = (imap_time_t){.year  = $fourdigit,
                           .month = $date_month,
                           .day   = $date_day}; };

/*** FETCH command ***/

fetch_cmd: tag SP uid_mode[u] FETCH SP seq_set[seq] SP
           { MODE(FETCH); } fetch_attrs[attr] fetch_mods_0[mods]
    { imap_cmd_arg_t arg = {.fetch=ie_fetch_cmd_new(E, $u, $seq, $attr, $mods)};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_FETCH, arg);
      /* VANISHED can be used with UID mode and CHANGEDSINCE both active */
      bool vanished = false;
      bool chgsince = false;
      for(ie_fetch_mods_t *mods = $mods; mods != NULL; mods = mods->next){
          if(mods->type == IE_FETCH_MOD_VANISHED){
              if($u == false){
                  TRACE_ORIG(E, E_PARAM, "VANISHED modifier applies to UID FETCH");
              }
              vanished = true;
          }else if(mods->type == IE_FETCH_MOD_CHGSINCE){
              chgsince = true;
          }
      }
      if(vanished && !chgsince){
          TRACE_ORIG(E, E_PARAM, "VANISHED modifier requires CHANGEDSINCE modifier");
      } };

fetch_attrs: ALL           { $$ = ie_fetch_attrs_new(E);
                             if($$ != NULL){
                                 $$->flags = true;
                                 $$->intdate = true;
                                 $$->envelope = true;
                             } }
           | FAST          { $$ = ie_fetch_attrs_new(E);
                             if($$ != NULL){
                                 $$->flags = true;
                                 $$->intdate = true;
                                 $$->rfc822_size = true;
                             } }
           | FULL          { $$ = ie_fetch_attrs_new(E);
                             if($$ != NULL){
                                 $$->flags = true;
                                 $$->intdate = true;
                                 $$->rfc822_size = true;
                                 $$->envelope = true;
                                 $$->body = true; } }
           | fetch_attr
           | '(' fetch_attrs_1 ')'  { $$ = $fetch_attrs_1; }
;

fetch_attr: fetch_attr_simple[s] { $$ = ie_fetch_attrs_add_simple(E, ie_fetch_attrs_new(E), $s); }
          | fetch_attr_extra[e]  { $$ = ie_fetch_attrs_add_extra(E, ie_fetch_attrs_new(E), $e); }
;

fetch_attrs_1: fetch_attr
             | fetch_attrs_1[f] SP fetch_attr_simple[s] { $$ = ie_fetch_attrs_add_simple(E, $f, $s); }
             | fetch_attrs_1[f] SP fetch_attr_extra[e]  { $$ = ie_fetch_attrs_add_extra(E, $f, $e); }
;

fetch_attr_simple: ENVELOPE       { $$ = IE_FETCH_ATTR_ENVELOPE; }
                 | FLAGS          { $$ = IE_FETCH_ATTR_FLAGS; }
                 | INTDATE        { $$ = IE_FETCH_ATTR_INTDATE; }
                 | UID            { $$ = IE_FETCH_ATTR_UID; }
                 | RFC822         { $$ = IE_FETCH_ATTR_RFC822; }
                 | RFC822_HEADER  { $$ = IE_FETCH_ATTR_RFC822_HEADER; }
                 | RFC822_SIZE    { $$ = IE_FETCH_ATTR_RFC822_SIZE; }
                 | RFC822_TEXT    { $$ = IE_FETCH_ATTR_RFC822_TEXT; }
                 | BODYSTRUCT     { $$ = IE_FETCH_ATTR_BODYSTRUCT; }
                 | BODY           { $$ = IE_FETCH_ATTR_BODY; }
                 | MODSEQ         { $$ = IE_FETCH_ATTR_MODSEQ; }
;

fetch_attr_extra: BODY '[' sect[s] ']' partial[p]      { $$ = ie_fetch_extra_new(E, false, $s, $p); }
                | BODY_PEEK '[' sect[s] ']' partial[p] { $$ = ie_fetch_extra_new(E, true, $s, $p); }
;

fetch_mods_0: %empty { $$ = NULL; }
            | SP '(' fetch_mods_1[mods] ')' { $$ = $mods; }
;

fetch_mods_1: fetch_mod
            | fetch_mods_1[l] SP fetch_mod[m]  { $$ = ie_fetch_mods_add(E, $l, $m); }
;

fetch_mod: CHGSINCE SP nzmodseqnum[s]
              { extension_trigger_builder(E, p->exts, EXT_CONDSTORE);
                ie_fetch_mod_arg_t arg = {.chgsince=$s};
                $$ = ie_fetch_mods_new(E, IE_FETCH_MOD_CHGSINCE, arg); }
         | VANISHED
              { extension_assert_on_builder(E, p->exts, EXT_QRESYNC);
                ie_fetch_mod_arg_t arg = {0};
                $$ = ie_fetch_mods_new(E, IE_FETCH_MOD_VANISHED, arg); }

sect: _sect[s] { $$ = $s; MODE(FETCH); }

_sect: %empty                         { $$ = NULL; }
     | sect_msgtxt[st]                { $$ = ie_sect_new(E, NULL, $st); }
     | sect_part[sp]                  { $$ = ie_sect_new(E, $sp, NULL); }
     | sect_part[sp] '.' sect_txt[st] { $$ = ie_sect_new(E, $sp, $st); }
;

sect_part: num                  { $$ = ie_sect_part_new(E, $num); }
         | sect_part[s] '.' num { $$ = ie_sect_part_add(E, $s, ie_sect_part_new(E, $num)); }
;

sect_txt: sect_msgtxt
        | MIME        { $$ = ie_sect_txt_new(E, IE_SECT_MIME, NULL); }
;

sect_msgtxt: HEADER { $$ = ie_sect_txt_new(E, IE_SECT_HEADER, NULL); }
           | TEXT   { $$ = ie_sect_txt_new(E, IE_SECT_TEXT, NULL); }

           | HDR_FLDS SP { MODE(ASTRING); } '(' header_list_1[h] ')'
                    { $$ = ie_sect_txt_new(E, IE_SECT_HDR_FLDS, $h); }

           | HDR_FLDS_NOT SP { MODE(ASTRING); } '(' header_list_1[h] ')'
                    { $$ = ie_sect_txt_new(E, IE_SECT_HDR_FLDS_NOT, $h); }
;

header_list_1: astring[h]                     { $$ = $h; }
             | header_list_1[l] SP astring[h] { $$ = ie_dstr_add(E, $l, $h); }
;

partial: %empty                      { $$ = NULL; }
       | '<' num[a] '.' nznum[b] '>' { $$ = ie_partial_new(E, $a, $b); }
;

/*** STORE command ***/

store_cmd: tag SP uid_mode[u] STORE SP seq_set[seq]
           { MODE(STORE); } SP store_mods_0[mods] store_sign[sign] FLAGS store_silent[silent]
           { MODE(FLAG); } store_flags[f]
    { imap_cmd_arg_t arg;
      arg.store = ie_store_cmd_new(E, $u, $seq, $mods, $sign, $silent, $f);
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_STORE, arg); };

store_mods_0: %empty                      { $$ = NULL; }
            | '(' store_mods_1[m] ')' SP  { $$ = $m; }
;

store_mods_1: store_mod
            | store_mods_1[l] SP store_mod[m]  { $$ = ie_store_mods_add(E, $l, $m); }
;

store_mod: UNCHGSINCE SP modseqnum[s]
              { extension_trigger_builder(E, p->exts, EXT_CONDSTORE);
                $$ = ie_store_mods_unchgsince(E, $s); }

store_sign: %empty  { $$ = 0; }
          | '-'     { $$ = -1; }
          | '+'     { $$ = 1; }
;

store_silent: %empty    { $$ = false; }
            | SILENT    { $$ = true; }
;

store_flags: %empty                { $$ = NULL; }
           | SP '(' flags_0[f] ')' { $$ = $f; }
           | SP flags_1[f]         { $$ = $f; }
;

/*** COPY command ***/

copy_cmd: tag SP uid_mode[u] COPY SP seq_set[seq] SP mailbox[m]
    { imap_cmd_arg_t arg = {.copy=ie_copy_cmd_new(E, $u, $seq, $m)};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_COPY, arg); };

/*** ENABLE command ***/

enable_cmd: tag SP ENABLE { MODE(ATOM); } SP capas_1[c]
    { extension_assert_on_builder(E, p->exts, EXT_ENABLE);
      imap_cmd_arg_t arg = {.enable=$c};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_ENABLE, arg); };


/*** status-type responses.  Thanks for the the shitty grammar, IMAP4rev1 ***/

/* a valid status-type response with status code input could be two things:
        * OK [ALERT] asdf
             ^^^^^^^ ^^^^---> status code, followed by general text
        * OK [ALERT] asdf
             ^^^^^^^^^^^^---> no status code, followed by general text which
                              just happens to look like a status code...
   We will resolve this by forcing any post-status text starting with a '['
   to conform to the status code style, meaning that this valid input:

        * OK [IMAP4rev1 has a shitty grammar ambiguity

   will not be accepted.  This seems like an acceptable sacrifice.
*/

status_type_resp_tagged: tag SP st_type[tp] SP
                         { MODE(STATUS_CODE_CHECK); } st_code[c] st_text[tx]
    { imap_resp_arg_t arg;
      arg.status_type = ie_st_resp_new(E, $tag, $tp, $c, $tx);
      $$ = imap_resp_new(E, IMAP_RESP_STATUS_TYPE, arg); };

status_type_resp_untagged: st_type[tp] SP
                          { MODE(STATUS_CODE_CHECK); } st_code[c] st_text[tx]
    { imap_resp_arg_t arg;
      arg.status_type = ie_st_resp_new(E, NULL, $tp, $c, $tx);
      $$ = imap_resp_new(E, IMAP_RESP_STATUS_TYPE, arg); };

st_type: OK       { $$ = IE_ST_OK; }
       | NO       { $$ = IE_ST_NO; }
       | BAD      { $$ = IE_ST_BAD; }
       | PREAUTH  { $$ = IE_ST_PREAUTH; }
       | BYE      { $$ = IE_ST_BYE; }
;

/* YES_STATUS_CODE means we got a '['
   NO_STATUS_CODE means we got the start of the text */
st_code: YES_STATUS_CODE { MODE(STATUS_CODE); }
           st_code_[c] ']' SP { MODE(STATUS_TEXT); $$ = $c; }
       | %empty { $$ = NULL; }

st_code_: ALERT      { ie_st_code_arg_t arg = {0};
                       $$ = ie_st_code_new(E, IE_ST_CODE_ALERT, arg); }
        | PARSE      { ie_st_code_arg_t arg = {0};
                       $$ = ie_st_code_new(E, IE_ST_CODE_PARSE, arg); }
        | READ_ONLY  { ie_st_code_arg_t arg = {0};
                       $$ = ie_st_code_new(E, IE_ST_CODE_READ_ONLY, arg); }
        | READ_WRITE { ie_st_code_arg_t arg = {0};
                       $$ = ie_st_code_new(E, IE_ST_CODE_READ_WRITE, arg); }
        | TRYCREATE  { ie_st_code_arg_t arg = {0};
                       $$ = ie_st_code_new(E, IE_ST_CODE_TRYCREATE, arg); }
        | UIDNEXT SP sc_num[n] { ie_st_code_arg_t arg = {.uidnext=$n};
                                 $$ = ie_st_code_new(E, IE_ST_CODE_UIDNEXT, arg); }
        | UIDVLD SP sc_num[n]  { ie_st_code_arg_t arg = {.uidvld=$n};
                                 $$ = ie_st_code_new(E, IE_ST_CODE_UIDVLD, arg); }
        | UNSEEN SP sc_num[n]  { ie_st_code_arg_t arg = {.unseen=$n};
                                 $$ = ie_st_code_new(E, IE_ST_CODE_UNSEEN, arg); }
        | sc_pflags
        | sc_capa
        | sc_atom

        | sc_uidnostick
        | sc_appenduid
        | sc_copyuid

        | sc_nomodseq
        | sc_himodseq
        | sc_modified

        /* no extension check, this must be sent by servers supporting QRESYNC,
           even if QRESYNC hasn't been enabled */
        | CLOSED { ie_st_code_arg_t arg = {0};
                   $$ = ie_st_code_new(E, IE_ST_CODE_CLOSED, arg); }
;

sc_end: %empty { MODE(STATUS_CODE); }

sc_num: { MODE(NUM); } nznum[n] sc_end { $$ = $n; };

sc_pflags: PERMFLAGS { MODE(FLAG); } SP pflags[p] sc_end
    { ie_st_code_arg_t arg = {.pflags=$p};
      $$ = ie_st_code_new(E, IE_ST_CODE_PERMFLAGS, arg); };

sc_capa: CAPA { MODE(ATOM); } SP capas_1[c] sc_end
    { ie_st_code_arg_t arg = {.capa=$c};
      $$ = ie_st_code_new(E, IE_ST_CODE_CAPA, arg); };

sc_atom: atom[a] { MODE(STATUS_TEXT); } sc_text[t]
    { ie_st_code_arg_t arg = {.atom={.name=$a, .text=$t}};
      $$ = ie_st_code_new(E, IE_ST_CODE_ATOM, arg); };


sc_uidnostick: UIDNOSTICK
    { extension_assert_on_builder(E, p->exts, EXT_UIDPLUS);
      ie_st_code_arg_t arg = {0};
      $$ = ie_st_code_new(E, IE_ST_CODE_UIDNOSTICK, arg); };

sc_appenduid: APPENDUID SP { MODE(NUM); } nznum[n] SP nznum[uid] sc_end
    { extension_assert_on_builder(E, p->exts, EXT_UIDPLUS);
      ie_st_code_arg_t arg = {.appenduid={.num=$n, .uid=$uid}};
      $$ = ie_st_code_new(E, IE_ST_CODE_APPENDUID, arg); };

sc_copyuid: COPYUID SP { MODE(NUM); } nznum[n] SP uid_set[in] SP uid_set[out] sc_end
    { extension_assert_on_builder(E, p->exts, EXT_UIDPLUS);
      ie_st_code_arg_t arg = {.copyuid={.num=$n, .uids_in=$in, .uids_out=$out}};
      $$ = ie_st_code_new(E, IE_ST_CODE_COPYUID, arg); };


sc_nomodseq: NOMODSEQ
    { extension_assert_on_builder(E, p->exts, EXT_CONDSTORE);
      ie_st_code_arg_t arg = {0};
      $$ = ie_st_code_new(E, IE_ST_CODE_NOMODSEQ, arg); }

sc_himodseq: HIMODSEQ SP { MODE(NUM); } nzmodseqnum[n] sc_end
    { extension_assert_on_builder(E, p->exts, EXT_CONDSTORE);
      ie_st_code_arg_t arg = {.himodseq=$n};
      $$ = ie_st_code_new(E, IE_ST_CODE_HIMODSEQ, arg); }

sc_modified: MODIFIED SP seq_set[s] sc_end
    { extension_assert_on_builder(E, p->exts, EXT_CONDSTORE);
      ie_st_code_arg_t arg = {.modified=$s};
      $$ = ie_st_code_new(E, IE_ST_CODE_MODIFIED, arg); };

/* If text is included after the atom code, ignore the leading space */
sc_text: %empty         { $$ = NULL; }
       | SP sc_text_[t] { $$ = $t; }
;

sc_text_: st_txt_inner_char            { $$ = ie_dstr_new(E, p->token, KEEP_RAW); }
        | sc_text_[t] st_txt_inner_char { $$ = ie_dstr_append(E, $t, p->token, KEEP_RAW); }
;

st_txt_inner_char: ' '
                 | '['
                 | RAW
;

/* starting with NO_STATUS_CODE indicates we skipped the status code entirely;
   starting with st_txt_char indicates that we started after a status code */
st_text: NO_STATUS_CODE         { MODE(STATUS_TEXT); $$ = ie_dstr_new(E, p->token, KEEP_RAW); }
       | st_txt_char            { $$ = ie_dstr_new(E, p->token, KEEP_RAW); }
       | st_text[t] st_txt_char { $$ = ie_dstr_append(E, $t, p->token, KEEP_RAW); }
;

st_txt_char: st_txt_inner_char
           | ']'
;

/*** CAPABILITY response ***/

capa_resp: CAPA { MODE(ATOM); } SP capas_1[c]
    { imap_resp_arg_t arg = {.capa=$c};
      $$ = imap_resp_new(E, IMAP_RESP_CAPA, arg); };

capas_1: atom
       | capas_1[c] SP atom[a] { $$ = ie_dstr_add(E, $c, $a); }

/*** LIST/LSUB responses ***/

list_resp: LIST SP mflags[mf] SP nqchar SP mailbox[mbx]
    { imap_resp_arg_t arg = {.list=ie_list_resp_new(E, $mf, $nqchar, $mbx)};
      $$ = imap_resp_new(E, IMAP_RESP_LIST, arg); };

lsub_resp: LSUB SP mflags[mf] SP nqchar SP mailbox[mbx]
    { imap_resp_arg_t arg = {.lsub=ie_list_resp_new(E, $mf, $nqchar, $mbx)};
      $$ = imap_resp_new(E, IMAP_RESP_LSUB, arg); };

/* nqchar can't handle spaces, so an post-nqchar MODE() call is required */
nqchar: prenqchar NIL                 { $$ = 0; MODE(MAILBOX); }
      | prenqchar '"' qchar[q] '"'  { $$ = $q; MODE(MAILBOX); }
;

prenqchar: %empty { MODE(NQCHAR); };

qchar: QCHAR { $$ = QCHAR_TO_CHAR; };

/*** STATUS response ***/

status_resp: STATUS SP mailbox[m] { MODE(STATUS_ATTR); } SP '(' s_attr_rlist_0[sa] ')'
    { imap_resp_arg_t arg = {.status=ie_status_resp_new(E, $m, $sa)};
      $$ = imap_resp_new(E, IMAP_RESP_STATUS, arg); };

s_attr_rlist_0: %empty          { $$ = (ie_status_attr_resp_t){0}; }
              | s_attr_rlist_1
;

s_attr_rlist_1: s_attr_resp
              | s_attr_rlist_1[a] SP s_attr_resp[b] { $$ = ie_status_attr_resp_add($a, $b); }
;

s_attr_resp: s_attr_32[s] SP num[n]        { $$ = ie_status_attr_resp_new_32(E, $s, $n); }
           | s_attr_64[s] SP modseqnum[n]  { $$ = ie_status_attr_resp_new_64(E, $s, $n); }
;

/*** FLAGS response ***/

flags_resp: FLAGS SP { MODE(FLAG); } '(' flags_0[f] ')'
    { imap_resp_arg_t arg = {.flags=$f};
      $$ = imap_resp_new(E, IMAP_RESP_FLAGS, arg); };

/*** SEARCH response ***/

search_resp: SEARCH
                        { imap_resp_arg_t arg = {
                              .search=ie_search_resp_new(E, NULL, false, 0),
                          };
                          $$ = imap_resp_new(E, IMAP_RESP_SEARCH, arg); }
           | SEARCH pre_search_resp SP nums_1[n]
                        { imap_resp_arg_t arg = {
                              .search=ie_search_resp_new(E, $n, false, 0),
                          };
                          $$ = imap_resp_new(E, IMAP_RESP_SEARCH, arg); }
           | SEARCH pre_search_resp SP nums_1[n] SP { MODE(MODSEQ); }
                '(' MODSEQ SP nzmodseqnum[s] ')'
                        { extension_assert_on_builder(E, p->exts, EXT_CONDSTORE);
                          imap_resp_arg_t arg = {
                              .search=ie_search_resp_new(E, $n, true, $s),
                          };
                          $$ = imap_resp_new(E, IMAP_RESP_SEARCH, arg); };

pre_search_resp: %empty { MODE(MODSEQ); };

/*** EXISTS response ***/

exists_resp: num SP EXISTS
    { imap_resp_arg_t arg = {.exists=$num};
      $$ = imap_resp_new(E, IMAP_RESP_EXISTS, arg); };

/*** RECENT response ***/

recent_resp: num SP RECENT
    { imap_resp_arg_t arg = {.recent=$num};
      $$ = imap_resp_new(E, IMAP_RESP_RECENT, arg); };

/*** EXPUNGE response ***/

expunge_resp: num SP EXPUNGE
    { imap_resp_arg_t arg = {.expunge=$num};
      $$ = imap_resp_new(E, IMAP_RESP_EXPUNGE, arg); };

/*** FETCH response ***/

fetch_resp: num[n] SP FETCH { MODE(FETCH); } SP '(' msg_attrs_0[ma] ')'
    { imap_resp_arg_t arg = {.fetch=ie_fetch_resp_num(E, $ma, $n)};
      $$ = imap_resp_new(E, IMAP_RESP_FETCH, arg); };

msg_attrs_0: %empty { $$ = NULL; }
           | msg_attrs_1
;

/* "fetch mode" */
fm: %empty { MODE(FETCH); };

/* most of these get ignored completely, we only really need:
     - FLAGS
     - UID
     - INTERNALDATE
     - the fully body text
     - MODSEQ
   Anything else is going to be encrypted anyway. */
msg_attrs_1: f_fflags[f] fm  { $$ = ie_fetch_resp_flags(E, ie_fetch_resp_new(E), $f); }
           | f_uid[u] fm     { $$ = ie_fetch_resp_uid(E, ie_fetch_resp_new(E), $u); }
           | f_intdate[d] fm { $$ = ie_fetch_resp_intdate(E, ie_fetch_resp_new(E), $d); }
           | f_rfc822[r] fm  { $$ = ie_fetch_resp_content(E, ie_fetch_resp_new(E), $r); }
           | f_modseq[s] fm  { $$ = ie_fetch_resp_modseq(E, ie_fetch_resp_new(E), $s); }
           | ign_msg_attr fm { $$ = ie_fetch_resp_new(E); }

           | msg_attrs_1[m] SP f_fflags[f] fm  { $$ = ie_fetch_resp_flags(E, $m, $f); }
           | msg_attrs_1[m] SP f_uid[u] fm     { $$ = ie_fetch_resp_uid(E, $m, $u); }
           | msg_attrs_1[m] SP f_intdate[d] fm { $$ = ie_fetch_resp_intdate(E, $m, $d); }
           | msg_attrs_1[m] SP f_rfc822[r] fm  { $$ = ie_fetch_resp_content(E, $m, $r); }
           | msg_attrs_1[m] SP f_modseq[s] fm  { $$ = ie_fetch_resp_modseq(E, $m, $s); }
           | msg_attrs_1[m] SP ign_msg_attr fm   { $$ = $m; }
;

/* we can parse these without throwing errors, but we ignore them */
ign_msg_attr: ENVELOPE SP '(' { MODE(NSTRING); } envelope ')'
            | RFC822_TEXT SP { MODE(NSTRING); } ign_nstring
            | RFC822_HEADER SP { MODE(NSTRING); } ign_nstring
            | RFC822_SIZE SP NUM
            /* don't even parse anything that starts with BODY */
;


f_fflags: FLAGS SP { MODE(FLAG); } '(' fflags_0[f] ')' { $$ = $f; };

f_rfc822: RFC822 SP { MODE(NSTRING); } rfc822_nstring[r] { $$ = $r; };

f_modseq: MODSEQ SP { MODE(MODSEQ); } '(' nzmodseqnum[s] ')'
    { extension_assert_on_builder(E, p->exts, EXT_CONDSTORE);
      $$ = $s; };

/* this will some day write to a file instead of memory, otherwise it would
   just be "nstring" type */
rfc822_nstring: NIL { $$ = ie_dstr_new_empty(E); }
              | literal
              | qstr
;

f_uid: UID SP { MODE(NUM); } num[n] { $$ = $n; };

f_intdate: INTDATE SP date_time[d] { $$ = $d; };

/*        date           subj           from          sender        reply-to */
envelope: ign_nstring SP ign_nstring SP naddr_list SP naddr_list SP ign_nstring SP
/*        to            cc            bcc           in-reply-to    message-id */
          naddr_list SP naddr_list SP naddr_list SP ign_nstring SP ign_nstring
;

naddr_list: NIL
          | '(' addr_list ')'
;

addr_list: address
         | addr_list /* no SP! */ address
;

/*           addr-name      addr-adl       addr-mailbox   addr-host */
address: '(' ign_nstring SP ign_nstring SP ign_nstring SP ign_nstring   ')'
;

ign_nstring: NIL
           | ign_string
;

/*** ENABLED response ***/

enabled_resp: ENABLED { MODE(ATOM); } SP capas_1[c]
    { extension_assert_on_builder(E, p->exts, EXT_ENABLE);
      imap_resp_arg_t arg = {.enabled=$c};
      $$ = imap_resp_new(E, IMAP_RESP_ENABLED, arg); };

/*** VANISHED response ***/

vanished_resp: VANISHED SP seq_set[s]
                { extension_assert_on_builder(E, p->exts, EXT_QRESYNC);
                  imap_resp_arg_t arg = {.vanished={.earlier=false, .uids=$s}};
                  $$ = imap_resp_new(E, IMAP_RESP_VANISHED, arg); }
             | VANISHED SP '(' EARLIER ')' SP seq_set[s]
                { extension_assert_on_builder(E, p->exts, EXT_QRESYNC);
                  imap_resp_arg_t arg = {.vanished={.earlier=true, .uids=$s}};
                  $$ = imap_resp_new(E, IMAP_RESP_VANISHED, arg); }
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

atom_like: RAW
         | NUM
         | keyword
;

atom: RAW                { $$ = ie_dstr_new(E, p->token, KEEP_RAW); }
    | atom[a] atom_like  { $$ = ie_dstr_append(E, $a, p->token, KEEP_RAW); }
;

qstr: '"' preqstr qstr_body[q] '"' { p->scan_mode = p->preqstr_mode; $$ = $q; };

preqstr: %empty { p->preqstr_mode = p->scan_mode; MODE(QSTRING); };

qstr_body: %empty           { $$ = ie_dstr_new_empty(E); }
         | qstr_body[q] RAW { $$ = ie_dstr_append(E, $q, p->token, KEEP_QSTRING); }
;

ign_qstr: '"' preqstr ign_qstr_body '"' { p->scan_mode = p->preqstr_mode; };

ign_qstr_body: %empty
             | ign_qstr_body RAW
;


/* note that LITERAL_END is passed by the application after it finishes reading
   the literal from the stream; it is never returned by the scanner */
literal: LITERAL { LITERAL_START; } literal_body[l] { $$ = $l; };

/* the scanner produces RAW tokens until the literal lengths is met.  Even if
   the literal length is 0, at least one empty RAW token is always produced */
literal_body: RAW                 { $$ = ie_dstr_new(E, p->token, KEEP_RAW); }
            | literal_body[l] RAW { $$ = ie_dstr_append(E, $l, p->token, KEEP_RAW); }

ign_literal: LITERAL { LITERAL_START; } ign_literal_body;

ign_literal_body: RAW
                | ign_literal_body RAW

string: qstr
      | literal
;

ign_string: ign_qstr
          | ign_literal
;

astring: atom
       | string
;

tag: atom[a] { $$ = $a; MODE(COMMAND); };

mailbox: prembx astring[a] { $$ = ie_mailbox_new_noninbox(E, $a); }
       | prembx INBOX       { $$ = ie_mailbox_new_inbox(E); }
;

prembx: %empty { MODE(MAILBOX); };

s_attr_32: MESSAGES    { $$ = IE_STATUS_ATTR_MESSAGES; }
         | RECENT      { $$ = IE_STATUS_ATTR_RECENT; }
         | UIDNEXT     { $$ = IE_STATUS_ATTR_UIDNEXT; }
         | UIDVLD      { $$ = IE_STATUS_ATTR_UIDVLD; }
         | UNSEEN      { $$ = IE_STATUS_ATTR_UNSEEN; }
;

s_attr_64: HIMODSEQ
    { extension_assert_on_builder(E, p->exts, EXT_CONDSTORE);
      $$ = IE_STATUS_ATTR_HIMODSEQ; };

s_attr_any: s_attr_32
          | s_attr_64
;

date_time: pre_date_time '"' date_day_fixed '-' date_month '-' fourdigit[y] SP
           twodigit[h] ':' twodigit[m] ':' twodigit[s] SP
           sign twodigit[zh] twodigit[zm] '"'
           { $$ = (imap_time_t){.year   = $y,
                                .month  = $date_month,
                                .day    = $date_day_fixed,
                                .hour   = $h,
                                .min    = $m,
                                .sec    = $s,
                                .z_sign = $sign,
                                .z_hour = $zh,
                                .z_min  = $zm }; };

pre_date_time: %empty { MODE(DATETIME); };

date_day_fixed: ' ' digit       { $$ = $digit; }
              | digit digit     { $$ = 10*$1 + $2; }
;

date_month: JAN { $$ = 0; }
          | FEB { $$ = 1; }
          | MAR { $$ = 2; }
          | APR { $$ = 3; }
          | MAY { $$ = 4; }
          | JUN { $$ = 5; }
          | JUL { $$ = 6; }
          | AUG { $$ = 7; }
          | SEP { $$ = 8; }
          | OCT { $$ = 9; }
          | NOV { $$ = 10; }
          | DEC { $$ = 11; }
;

sign: '+' { $$ = 1; }
    | '-' { $$ = -1; }
;

digit: DIGIT {
     switch(p->token->data[0]){
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
        default:
            // scanner should guarantee this never happens
            if(is_error(p->error)){
                TRACE(&p->error, "invalid digit: %x\n", FC(p->token->data[0]));
                TRACE_ORIG(&p->error, E_INTERNAL, "invalid digit");
            }
            $$ = 0;
        }
    };

twodigit: digit digit { $$ = 10*$1 + $2; };

fourdigit: digit digit digit digit { $$ = 1000*$1 + 100*$2 + 10*$3 + $4; };

num: NUM { PARSE_NUM($$); };
nznum: NUM { PARSE_NZNUM($$); };
modseqnum: NUM { PARSE_MODSEQNUM($$); };
nzmodseqnum: NUM { PARSE_NZMODSEQNUM($$); };

preseq: %empty { MODE(SEQSET); };

seq_spec: seq_num[n]                  { $$ = ie_seq_set_new(E, $n, $n); }
        | seq_num[n1] ':' seq_num[n2] { $$ = ie_seq_set_new(E, $n1, $n2); }
;

seq_num: '*'    { $$ = 0; }
       | nznum
;

nums_1: num              { $$ = ie_nums_new(E, $num); }
      | nums_1[l] SP num { $$ = ie_nums_append(E, $l, ie_nums_new(E, $num)); }
;

seq_set: preseq seq_spec[set]            { $$ = $set; }
       | seq_set[set] ',' seq_spec[spec] { $$ = ie_seq_set_append(E, $set, $spec); }
;

/* uid_set is like seq_set except without '*' values */
uid_spec: nznum[n]                { $$ = ie_seq_set_new(E, $n, $n); }
        | nznum[n1] ':' nznum[n2] { $$ = ie_seq_set_new(E, $n1, $n2); }
;

uid_set: preseq uid_spec[set]            { $$ = $set; }
       | uid_set[set] ',' uid_spec[spec] { $$ = ie_seq_set_append(E, $set, $spec); }


/* flags, used by APPEND commands, STORE commands, and FLAGS responses */

flags_0: %empty     { $$ = NULL; }
       | flags_1
;

flags_1: flag_simple[s]  { $$ = ie_flags_add_simple(E, ie_flags_new(E), $s); }
       | '\\' atom[e]    { $$ = ie_flags_add_ext(E, ie_flags_new(E), $e); }
       | atom [k]        { $$ = ie_flags_add_kw(E, ie_flags_new(E), $k); }
       | flags_1[f] SP flag_simple[s] { $$ = ie_flags_add_simple(E, $f, $s); }
       | flags_1[f] SP '\\' atom[e]   { $$ = ie_flags_add_ext(E, $f, $e); }
       | flags_1[f] SP atom[k]        { $$ = ie_flags_add_kw(E, $f, $k); }
;

flag_simple: '\\' ANSWERED { $$ = IE_FLAG_ANSWERED; }
           | '\\' FLAGGED  { $$ = IE_FLAG_FLAGGED; }
           | '\\' DELETED  { $$ = IE_FLAG_DELETED; }
           | '\\' SEEN     { $$ = IE_FLAG_SEEN; }
           | '\\' DRAFT    { $$ = IE_FLAG_DRAFT; }
;

/* pflags, only used by PERMANENTFLAGS code of status-type response */

pflags: '(' pflags_0[pf] ')' { $$ = $pf; };

pflags_0: %empty     { $$ = NULL; }
       | pflags_1
;

pflags_1: pflag_simple[s]  { $$ = ie_pflags_add_simple(E, ie_pflags_new(E), $s); }
        | '\\' atom[e]     { $$ = ie_pflags_add_ext(E, ie_pflags_new(E), $e); }
        | atom [k]         { $$ = ie_pflags_add_kw(E, ie_pflags_new(E), $k); }
        | pflags_1[pf] SP pflag_simple[s] { $$ = ie_pflags_add_simple(E, $pf, $s); }
        | pflags_1[pf] SP '\\' atom[e]    { $$ = ie_pflags_add_ext(E, $pf, $e); }
        | pflags_1[pf] SP atom[k]         { $$ = ie_pflags_add_kw(E, $pf, $k); }
;

pflag_simple: '\\' ANSWERED { $$ = IE_PFLAG_ANSWERED; }
            | '\\' FLAGGED  { $$ = IE_PFLAG_FLAGGED; }
            | '\\' DELETED  { $$ = IE_PFLAG_DELETED; }
            | '\\' SEEN     { $$ = IE_PFLAG_SEEN; }
            | '\\' DRAFT    { $$ = IE_PFLAG_DRAFT; }
            | ASTERISK_FLAG { $$ = IE_PFLAG_ASTERISK; }
;

/* only one of these in a list */
mflag_select: '\\' NOSELECT    { $$ = IE_SELECTABLE_NOSELECT; }
            | '\\' MARKED      { $$ = IE_SELECTABLE_MARKED; }
            | '\\' UNMARKED    { $$ = IE_SELECTABLE_UNMARKED; }
;

/* fflags, only used by FETCH responses */

fflags_0: %empty     { $$ = NULL; }
        | fflags_1
;

fflags_1: fflag_simple[s] { $$ = ie_fflags_add_simple(E, ie_fflags_new(E), $s); }
        | '\\' atom[e]    { $$ = ie_fflags_add_ext(E, ie_fflags_new(E), $e); }
        | atom [k]        { $$ = ie_fflags_add_kw(E, ie_fflags_new(E), $k); }
        | fflags_1[f] SP fflag_simple[s] { $$ = ie_fflags_add_simple(E, $f, $s); }
        | fflags_1[f] SP '\\' atom[e]    { $$ = ie_fflags_add_ext(E, $f, $e); }
        | fflags_1[f] SP atom[k]         { $$ = ie_fflags_add_kw(E, $f, $k); }
;

fflag_simple: '\\' ANSWERED { $$ = IE_FFLAG_ANSWERED; }
            | '\\' FLAGGED  { $$ = IE_FFLAG_FLAGGED; }
            | '\\' DELETED  { $$ = IE_FFLAG_DELETED; }
            | '\\' SEEN     { $$ = IE_FFLAG_SEEN; }
            | '\\' DRAFT    { $$ = IE_FFLAG_DRAFT; }
            | '\\' RECENT   { $$ = IE_FFLAG_RECENT; }
;

/* mflags, only used by LIST and LSUB responses */

mflags: { MODE(MFLAG); } '(' mflags_0[mf] ')' { $$ = $mf; };

mflags_0: %empty     { $$ = NULL; }
        | mflags_1
;

mflags_1: '\\' NOINFERIORS { $$ = ie_mflags_add_noinf(E, ie_mflags_new(E)); }
        | mflag_select[s]  { MFLAG_SELECT($$, ie_mflags_new(E), $s); }
        | '\\' atom[e]     { $$ = ie_mflags_add_ext(E, ie_mflags_new(E), $e); }
        | mflags_1[mf] SP '\\' NOINFERIORS { $$ = ie_mflags_add_noinf(E, $mf); }
        | mflags_1[mf] SP mflag_select[s]  { MFLAG_SELECT($$, $mf, $s); }
        | mflags_1[mf] SP '\\' atom[e]     { $$ = ie_mflags_add_ext(E, $mf, $e); }
;

SP: ' ';
