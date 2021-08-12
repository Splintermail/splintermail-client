%{
    #include <stdio.h>
    #include <libimap/libimap.h>
    #include <limits.h>

    #define MODE(m) p->scan_mode = SCAN_MODE_ ## m

    #define E (&p->error)

    // a YYACCEPT wrapper that resets some custom parser details
    #define ACCEPT \
        MODE(STD); \
        ie_dstr_free(STEAL(ie_dstr_t, &p->errmsg)); \
        YYACCEPT \

    static void send_cmd(imap_parser_t *p, imap_cmd_t *cmd){
        if(is_error(p->error)){
            imap_cmd_free(cmd);
            return;
        }
        p->cb.cmd(p->cb_data, cmd);
    }

    static void send_resp(imap_parser_t *p, imap_resp_t *resp){
        if(is_error(p->error)){
            imap_resp_free(resp);
            return;
        }
        p->cb.resp(p->cb_data, resp);
    }

    static void handle_parse_error(imap_parser_t *p, ie_dstr_t *tag){
        // servers pass the error message to the client.
        // clients let an error be raised
        if(!p->freeing && !p->is_client){
            imap_cmd_arg_t arg = { .error = STEAL(ie_dstr_t, &p->errmsg) };
            imap_cmd_t *cmd = imap_cmd_new(E, tag, IMAP_CMD_ERROR, arg);
            send_cmd(p, cmd);
        }else{
            ie_dstr_free(tag);
        }
    }

    // must be a macro because it has to be able to call YYERROR
    #define PARSE_NUM(text, func, out) do { \
        derr_t e = E_OK; \
        if(!is_error(p->error)){ \
            e = func(&(text)->dstr, out, 10); \
        } \
        ie_dstr_free(text); \
        if(is_error(e)){ \
            DROP_VAR(&e); \
            imapyyerror(p, "invalid number"); \
            YYERROR; \
        } \
    } while(0)

    #define NZ(num) do { \
        if(!(num)){ \
            imapyyerror(p, "invalid number"); \
            YYERROR; \
        } \
    } while(0)

    #define LITERAL_START(len, sendplus) do { \
        /* put scanner in literal mode */ \
        set_scanner_to_literal_mode(p, (len)); \
        if((sendplus) && !p->is_client){ \
            imap_cmd_arg_t arg = {0}; \
            imap_cmd_t *cmd = imap_cmd_new(E, NULL, IMAP_CMD_PLUS_REQ, arg); \
            send_cmd(p, cmd); \
        } \
    } while(0)

    // make sure we don't get two selectability flags
    #define MFLAG_SELECT(out, _mf, selectability) do { \
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
                imapyyerror(p, "multiple selectability flags"); \
                YYERROR; \
            }else{ \
                mf->selectable = selectability; \
                (out) = mf; \
            } \
        } \
    } while(0)
%}

/* use a different prefix, to not overlap with the imf parser's prefix */
%define api.prefix {imapyy}
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

/* the scanner passes its errors to the parser for better error recovery */
%token INVALID_TOKEN

/* some generic types */
%token RAW
%token NON_CRLF_CTL
%token NIL
%token DIGIT
%token NUM
%token QCHAR
%token LITERAL
%token LITERAL_END

/* commands */
%token CAPA
%token NOOP
%token LOGOUT
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
%token UNSELECT
%token IDLE
%token DONE
%token XKEYSYNC
%token XKEYADD

/* responses */
%token OK
%token NO
%token BAD
%token PREAUTH
%token BYE
/*     CAPA (listed above) */
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
/*     XKEYSYNC (listed above) */

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
/*     TEXT (listed elsewhere) */
/*     HEADER (listed elsewhere) */
%token SIZE
%token BODYSTRUCT
/*     BODY (listed above */
%token PEEK
/*     UID (listed above) */

/* BODY[] section stuff */
%token MIME
/*     TEXT (listed above) */
/*     HEADER (listed above) */
%token FIELDS
/*     NOT (listed elsewhere) */

/* FLAGS */
/*     ANSWERED (listed above) */
/*     FLAGGED (listed above) */
/*     DELETED (listed above) */
/*     SEEN (listed above) */
/*     DRAFT (listed above) */
/*     RECENT (listed above) */

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

/* XKEY extension */
%token CREATED

%type <ch> qchar
%type <ch> nqchar
// no destructor for char type

%type <dstr> tag
%type <dstr> atom
%type <dstr> atom_sc
%type <dstr> astring_mbox
%type <dstr> astring_atom
%type <dstr> atom_flag
%type <dstr> atom_fflag
%type <dstr> atom_mflag
%type <dstr> qstr
%type <dstr> qstr_body
%type <dstr> literal
%type <dstr> literal_body_0
%type <dstr> literal_body_1
%type <dstr> astring
%type <dstr> astring_1
%type <dstr> string
%type <dstr> search_charset
%type <dstr> header_list_1
%type <dstr> st_text
%type <dstr> sc_text
%type <dstr> sc_text_
%type <dstr> capas_1
%type <dstr> f_rfc822
%type <dstr> file_nstring
%type <dstr> list_char_1
%type <dstr> list_mailbox
%type <dstr> num_str
%type <dstr> fprs
%destructor { ie_dstr_free($$); } <dstr>

%type <fetch_resp_extra> f_extra
%destructor { ie_fetch_resp_extra_free($$); } <fetch_resp_extra>

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
%type <num> uid_nznum
%type <num> digit
%type <num> twodigit
%type <num> fourdigit
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


%type <imap_cmd> command
%type <imap_cmd> capa_cmd
%type <imap_cmd> noop_cmd
%type <imap_cmd> logout_cmd
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
%type <imap_cmd> unselect_cmd
%type <imap_cmd> idle_cmd
%type <imap_cmd> xkeysync_cmd
%type <imap_cmd> xkeyadd_cmd
%destructor { imap_cmd_free($$); } <imap_cmd>

%type <imap_resp> response
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
%type <imap_resp> xkeysync_resp
%type <imap_resp> xkeysync_resp_
%type <imap_resp> plus_resp

%destructor { imap_resp_free($$); } <imap_resp>

%% /********** Grammar Section **********/

line: command[c] EOL {
        send_cmd(p, $c);
        ACCEPT;
  } | response[r] EOL {
        send_resp(p, $r);
        ACCEPT;
  /* various error cases */
  } | command[c] error EOL {
        handle_parse_error(p, STEAL(ie_dstr_t, &$c->tag));
        imap_cmd_free($c);
        ACCEPT;
  } | response[r] error EOL {
        handle_parse_error(p, NULL);
        imap_resp_free($r);
        ACCEPT;
  } | tag error EOL {
        handle_parse_error(p, $tag);
        ACCEPT;
  } | error EOL {
        handle_parse_error(p, NULL);
        ACCEPT;
      }
;

cmdcheck: %empty {
    if(p->is_client){
        imapyyerror(p, "syntax error");
        YYERROR;
    }
}

command: capa_cmd
       | noop_cmd
       | logout_cmd
       | starttls_cmd
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
       | unselect_cmd
       | idle_cmd
       | xkeysync_cmd
       | xkeyadd_cmd
;

respcheck: %empty {
    if(!p->is_client){
        imapyyerror(p, "syntax error");
        YYERROR;
    }
}

response: status_type_resp_tagged
        | untag SP untagged_resp[u] { $$ = $u; }
        | plus_resp
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
             | xkeysync_resp
;

untag: '*';

/*** CAPABILITY command ***/

capa_cmd: tag SP CAPA cmdcheck
    { $$ = imap_cmd_new(E, $tag, IMAP_CMD_CAPA, (imap_cmd_arg_t){0}); };

/*** NOOP command ***/

noop_cmd: tag SP NOOP cmdcheck
    { $$ = imap_cmd_new(E, $tag, IMAP_CMD_NOOP, (imap_cmd_arg_t){0}); };

/*** LOGOUT command ***/

logout_cmd: tag SP LOGOUT cmdcheck
    { $$ = imap_cmd_new(E, $tag, IMAP_CMD_LOGOUT, (imap_cmd_arg_t){0}); };

/*** STARTTLS command ***/

starttls_cmd: tag SP STARTTLS cmdcheck
    { $$ = imap_cmd_new(E, $tag, IMAP_CMD_STARTTLS, (imap_cmd_arg_t){0}); };

/*** AUTHENTICATE command ***/
authenticate_cmd: tag SP AUTHENTICATE cmdcheck SP atom
    { imap_cmd_arg_t arg = {.auth=$atom};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_AUTH, arg); };

/*** LOGIN command ***/

login_cmd: tag SP LOGIN cmdcheck SP astring[u] SP astring[p]
    { imap_cmd_arg_t arg = {.login=ie_login_cmd_new(E, $u, $p)};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_LOGIN, arg); };

/*** SELECT command ***/

select_cmd: tag SP SELECT cmdcheck SP mailbox[m] select_params_0[p]
    { imap_cmd_arg_t arg = {.select=ie_select_cmd_new(E, $m, $p)};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_SELECT, arg); };

select_params_0: %empty                        { $$ = NULL; }
               | SP '(' select_params_1[p] ')' { $$ = $p; }
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

examine_cmd: tag SP EXAMINE cmdcheck SP mailbox[m] select_params_0[p]
    { imap_cmd_arg_t arg = {.examine=ie_select_cmd_new(E, $m, $p)};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_EXAMINE, arg); };

/*** CREATE command ***/

create_cmd: tag SP CREATE cmdcheck SP mailbox[m]
    { imap_cmd_arg_t arg = {.create=$m};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_CREATE, arg); };

/*** DELETE command ***/

delete_cmd: tag SP DELETE cmdcheck SP mailbox[m]
    { imap_cmd_arg_t arg = {.delete=$m};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_DELETE, arg); };

/*** RENAME command ***/

rename_cmd: tag SP RENAME cmdcheck SP mailbox[o] SP mailbox[n]
    { imap_cmd_arg_t arg = {.rename=ie_rename_cmd_new(E, $o, $n)};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_RENAME, arg); };

/*** SUBSCRIBE command ***/

subscribe_cmd: tag SP SUBSCRIBE cmdcheck SP mailbox[m]
    { imap_cmd_arg_t arg = {.sub=$m};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_SUB, arg); };

/*** UNSUBSCRIBE command ***/

unsubscribe_cmd: tag SP UNSUBSCRIBE cmdcheck SP mailbox[m]
    { imap_cmd_arg_t arg = {.unsub=$m};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_UNSUB, arg); };

/*** LIST command ***/

list_cmd: tag SP LIST cmdcheck SP mailbox[m] SP list_mailbox[p]
    { imap_cmd_arg_t arg = {.list=ie_list_cmd_new(E, $m, $p)};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_LIST, arg); };

list_mailbox: string
            | list_char_1;

list_char: atom_char | '%' | '*';
list_char_1: list_char                 { $$ = ie_dstr_new(E, p->token, KEEP_RAW); }
           | list_char_1[l] list_char  { $$ = ie_dstr_append(E, $l, p->token, KEEP_RAW); }
;

/*** LSUB command ***/

lsub_cmd: tag SP LSUB cmdcheck SP mailbox[m] SP list_mailbox[p]
    { imap_cmd_arg_t arg = {.lsub=ie_list_cmd_new(E, $m, $p)};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_LSUB, arg); };

/*** STATUS command ***/

status_cmd: tag SP STATUS cmdcheck SP mailbox[m]
          SP '(' s_attr_clist_1[sa] ')'
    { imap_cmd_arg_t arg = {.status=ie_status_cmd_new(E, $m, $sa)};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_STATUS, arg); };

s_attr_clist_1: s_attr_any[s]                        { $$ = $s; }
              | s_attr_clist_1[old] SP s_attr_any[s] { $$ = $old | $s; }
;

/*** APPEND command ***/

append_cmd: tag SP APPEND cmdcheck SP mailbox[m]
          SP append_flags[f] append_time[t]
          literal[l]
    { imap_cmd_arg_t arg = {.append=ie_append_cmd_new(E, $m, $f, $t, $l)};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_APPEND, arg); };

append_flags: %empty                { $$ = NULL; }
            | '(' flags_0[f] ')' SP { $$ = $f; }
;

append_time: %empty             { $$ = (imap_time_t){0}; }
           | date_time[dt] SP   { $$ = $dt; }
;

/*** CHECK command ***/

check_cmd: tag[t] SP CHECK cmdcheck
    { $$ = imap_cmd_new(E, $t, IMAP_CMD_CHECK, (imap_cmd_arg_t){0}); };

/*** CLOSE command ***/

close_cmd: tag[t] SP CLOSE cmdcheck
    { $$ = imap_cmd_new(E, $t, IMAP_CMD_CLOSE, (imap_cmd_arg_t){0}); };

/*** EXPUNGE command ***/

expunge_cmd: tag[t] SP EXPUNGE cmdcheck
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

search_cmd: tag SP uid_mode[u] SEARCH cmdcheck SP
            search_charset[c]
            search_keys_1[k]
    { imap_cmd_arg_t arg = {.search=ie_search_cmd_new(E, $u, $c, $k)};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_SEARCH, arg); };

search_charset: %empty { $$ = NULL; }
              | CHARSET SP astring[s] SP { $$ = $s; }
;

search_keys_1: search_key[k]                     { $$ = $k; }
             | search_keys_1[a] SP search_key[b]
               { $$ = ie_search_pair(E, IE_SEARCH_AND, $a, $b); }
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
          | BCC SP astring[s]           { $$ = ie_search_dstr(E, IE_SEARCH_BCC, $s); }
          | BODY SP astring[s]          { $$ = ie_search_dstr(E, IE_SEARCH_BODY, $s); }
          | CC SP astring[s]            { $$ = ie_search_dstr(E, IE_SEARCH_CC, $s); }
          | FROM SP astring[s]          { $$ = ie_search_dstr(E, IE_SEARCH_FROM, $s); }
          | KEYWORD SP atom[s]          { $$ = ie_search_dstr(E, IE_SEARCH_KEYWORD, $s); }
          | SUBJECT SP astring[s]       { $$ = ie_search_dstr(E, IE_SEARCH_SUBJECT, $s); }
          | TEXT SP astring[s]          { $$ = ie_search_dstr(E, IE_SEARCH_TEXT, $s); }
          | TO SP astring[s]            { $$ = ie_search_dstr(E, IE_SEARCH_TO, $s); }
          | UNKEYWORD SP atom[s]        { $$ = ie_search_dstr(E, IE_SEARCH_UNKEYWORD, $s); }
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

search_hdr: HEADER SP astring[h] SP astring[v]
          { $$ = ie_search_header(E, IE_SEARCH_HEADER, $h, $v); };

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

search_date: pre_date '"' date'"' post_date  { $$ = $date; }
           | pre_date date post_date         { $$ = $date; }
;

date_day: digit           { $$ = $digit; }
        | digit digit     { $$ = 10*$1 + $2; }
;

pre_date: %empty { MODE(DATETIME); };
post_date: %empty { MODE(STD); };

date: date_day '-' date_month '-' fourdigit
      { $$ = (imap_time_t){.year  = $fourdigit,
                           .month = $date_month,
                           .day   = $date_day}; };

/*** FETCH command ***/

fetch_cmd: tag SP uid_mode[u] FETCH cmdcheck SP seq_set[seq] SP
           fetch_attrs[attr] fetch_mods_0[mods]
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

fetch_attr_simple: ENVELOPE           { $$ = IE_FETCH_ATTR_ENVELOPE; }
                 | FLAGS              { $$ = IE_FETCH_ATTR_FLAGS; }
                 | INTDATE            { $$ = IE_FETCH_ATTR_INTDATE; }
                 | UID                { $$ = IE_FETCH_ATTR_UID; }
                 | RFC822             { $$ = IE_FETCH_ATTR_RFC822; }
                 | RFC822 '.' HEADER  { $$ = IE_FETCH_ATTR_RFC822_HEADER; }
                 | RFC822 '.' SIZE    { $$ = IE_FETCH_ATTR_RFC822_SIZE; }
                 | RFC822 '.' TEXT    { $$ = IE_FETCH_ATTR_RFC822_TEXT; }
                 | BODYSTRUCT         { $$ = IE_FETCH_ATTR_BODYSTRUCT; }
                 | BODY               { $$ = IE_FETCH_ATTR_BODY; }
                 | MODSEQ             { $$ = IE_FETCH_ATTR_MODSEQ; }
;

fetch_attr_extra: BODY '[' sect[s] ']' partial[p]          { $$ = ie_fetch_extra_new(E, false, $s, $p); }
                | BODY '.' PEEK '[' sect[s] ']' partial[p] { $$ = ie_fetch_extra_new(E, true, $s, $p); }
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

sect: _sect[s] { $$ = $s; }

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

           | HEADER '.' FIELDS SP '(' header_list_1[h] ')'
                    { $$ = ie_sect_txt_new(E, IE_SECT_HDR_FLDS, $h); }

           | HEADER '.' FIELDS '.' NOT SP '(' header_list_1[h] ')'
                    { $$ = ie_sect_txt_new(E, IE_SECT_HDR_FLDS_NOT, $h); }
;

header_list_1: astring[h]                     { $$ = $h; }
             | header_list_1[l] SP astring[h] { $$ = ie_dstr_add(E, $l, $h); }
;

partial: %empty                      { $$ = NULL; }
       | '<' num[a] '.' nznum[b] '>' { $$ = ie_partial_new(E, $a, $b); }
;

/*** STORE command ***/

store_cmd: tag SP uid_mode[u] STORE cmdcheck SP seq_set[seq]
           SP store_mods_0[mods] store_sign[sign] FLAGS store_silent[silent]
           store_flags[f]
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

store_silent: %empty     { $$ = false; }
            | '.' SILENT { $$ = true; }
;

store_flags: SP '(' flags_0[f] ')' { $$ = $f; }
           | SP flags_1[f]         { $$ = $f; }
;

/*** COPY command ***/

copy_cmd: tag SP uid_mode[u] COPY cmdcheck SP seq_set[seq] SP mailbox[m]
    { imap_cmd_arg_t arg = {.copy=ie_copy_cmd_new(E, $u, $seq, $m)};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_COPY, arg); };

/*** ENABLE command ***/

enable_cmd: tag SP ENABLE cmdcheck SP capas_1[c]
    { extension_assert_on_builder(E, p->exts, EXT_ENABLE);
      imap_cmd_arg_t arg = {.enable=$c};
      $$ = imap_cmd_new(E, $tag, IMAP_CMD_ENABLE, arg); };

/*** UNSELECT command ***/

unselect_cmd: tag[t] SP UNSELECT cmdcheck
    { extension_assert_on_builder(E, p->exts, EXT_UNSELECT);
      $$ = imap_cmd_new(E, $t, IMAP_CMD_UNSELECT, (imap_cmd_arg_t){0}); };

/*** IDLE command ***/

idle_cmd:
    tag[t] SP IDLE cmdcheck EOL
    {
        extension_assert_available_builder(E, p->exts, EXT_IDLE);
        if(is_error(p->error)){
            DROP_VAR(E);
            imapyyerror(p, "IDLE not supported");
            YYERROR;
        }
        // make a copy of the tag so that error handling can depend on the tag
        ie_dstr_t *tag = ie_dstr_copy(E, $t);
        // forward the IDLE request to the server
        imap_cmd_arg_t arg = {0};
        imap_cmd_t *cmd = imap_cmd_new(E, tag, IMAP_CMD_IDLE, arg);
        send_cmd(p, cmd);
    }
    DONE
    {
        imap_cmd_arg_t arg = { .idle_done = $t };
        $$ = imap_cmd_new(E, NULL, IMAP_CMD_IDLE_DONE, arg);
    };

/*** XKEYSYNC command ***/

fprs: EOL                  { $$ = NULL; }
    | SP astring_1[a] EOL  { $$ = $a; }
;

xkeysync_cmd:
    tag[t] SP XKEYSYNC cmdcheck fprs
    {
        extension_assert_on_builder(E, p->exts, EXT_XKEY);
        if(is_error(p->error)){
            DROP_VAR(E);
            imapyyerror(p, "XKEY not supported");
            YYERROR;
        }
        // make a copy of the tag so that error handling can depend on the tag
        ie_dstr_t *tag = ie_dstr_copy(E, $t);
        // forward the XKEYSYNC request to the server
        imap_cmd_arg_t arg = { .xkeysync = $fprs };
        imap_cmd_t *cmd = imap_cmd_new(E, tag, IMAP_CMD_XKEYSYNC, arg);
        send_cmd(p, cmd);
    }
    DONE
    {
        imap_cmd_arg_t arg = { .xkeysync_done = $t };
        $$ = imap_cmd_new(E, NULL, IMAP_CMD_XKEYSYNC_DONE, arg);
    };

/*** XKEYADD command ***/

xkeyadd_cmd: tag[t] SP XKEYADD cmdcheck SP astring[key]
    { extension_assert_on_builder(E, p->exts, EXT_XKEY);
      imap_cmd_arg_t arg = {.xkeyadd=$key};
      $$ = imap_cmd_new(E, $t, IMAP_CMD_XKEYADD, arg); };

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

status_type_resp_tagged: tag SP st_type[tp] respcheck SP
                         { MODE(STATUS_CODE_CHECK); } st_code[c] st_text[tx]
    { imap_resp_arg_t arg;
      arg.status_type = ie_st_resp_new(E, $tag, $tp, $c, $tx);
      $$ = imap_resp_new(E, IMAP_RESP_STATUS_TYPE, arg); };

status_type_resp_untagged: st_type[tp] respcheck SP
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
st_code: YES_STATUS_CODE { MODE(STD); } st_code_[c] ']' SP { $$ = $c; }
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
        | UIDNEXT SP nznum[n] { ie_st_code_arg_t arg = {.uidnext=$n};
                                $$ = ie_st_code_new(E, IE_ST_CODE_UIDNEXT, arg); }
        | UIDVLD SP nznum[n]  { ie_st_code_arg_t arg = {.uidvld=$n};
                                $$ = ie_st_code_new(E, IE_ST_CODE_UIDVLD, arg); }
        | UNSEEN SP nznum[n]  { ie_st_code_arg_t arg = {.unseen=$n};
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

sc_pflags: PERMFLAGS SP pflags[p]
    { ie_st_code_arg_t arg = {.pflags=$p};
      $$ = ie_st_code_new(E, IE_ST_CODE_PERMFLAGS, arg); };

sc_capa: CAPA SP capas_1[c]
    { ie_st_code_arg_t arg = {.capa=$c};
      $$ = ie_st_code_new(E, IE_ST_CODE_CAPA, arg); };

sc_atom: atom_sc[a] sc_text[t]
    { ie_st_code_arg_t arg = {.atom={.name=$a, .text=$t}};
      $$ = ie_st_code_new(E, IE_ST_CODE_ATOM, arg); };

sc_uidnostick: UIDNOSTICK
    { extension_assert_on_builder(E, p->exts, EXT_UIDPLUS);
      ie_st_code_arg_t arg = {0};
      $$ = ie_st_code_new(E, IE_ST_CODE_UIDNOSTICK, arg); };

sc_appenduid: APPENDUID SP nznum[n] SP nznum[uid]
    { extension_assert_on_builder(E, p->exts, EXT_UIDPLUS);
      ie_st_code_arg_t arg = {.appenduid={.uidvld=$n, .uid=$uid}};
      $$ = ie_st_code_new(E, IE_ST_CODE_APPENDUID, arg); };

sc_copyuid: COPYUID SP nznum[n] SP uid_set[in] SP uid_set[out]
    { extension_assert_on_builder(E, p->exts, EXT_UIDPLUS);
      ie_st_code_arg_t arg = {.copyuid={.uidvld=$n, .uids_in=$in, .uids_out=$out}};
      $$ = ie_st_code_new(E, IE_ST_CODE_COPYUID, arg); };


sc_nomodseq: NOMODSEQ
    { extension_assert_on_builder(E, p->exts, EXT_CONDSTORE);
      ie_st_code_arg_t arg = {0};
      $$ = ie_st_code_new(E, IE_ST_CODE_NOMODSEQ, arg); }

sc_himodseq: HIMODSEQ SP nzmodseqnum[n]
    { extension_assert_on_builder(E, p->exts, EXT_CONDSTORE);
      ie_st_code_arg_t arg = {.himodseq=$n};
      $$ = ie_st_code_new(E, IE_ST_CODE_HIMODSEQ, arg); }

sc_modified: MODIFIED SP seq_set[s]
    { extension_assert_on_builder(E, p->exts, EXT_CONDSTORE);
      ie_st_code_arg_t arg = {.modified=$s};
      $$ = ie_st_code_new(E, IE_ST_CODE_MODIFIED, arg); };

/* If text is included after the atom code, ignore the leading space */
sc_text: %empty         { $$ = NULL; }
       | SP sc_text_[t] { $$ = $t; }
;

sc_text_: st_txt_inner_char             { $$ = ie_dstr_new(E, p->token, KEEP_RAW); }
        | sc_text_[t] st_txt_inner_char { $$ = ie_dstr_append(E, $t, p->token, KEEP_RAW); }
;

st_txt_inner_char: atom_char | '(' | ')' | '{' | '%' | '*' | SP | NON_CRLF_CTL | '"' | '\\';

/* starting with NO_STATUS_CODE indicates we skipped the status code entirely;
   starting with st_txt_char indicates that we started after a status code */
st_text: NO_STATUS_CODE         { MODE(STD); $$ = ie_dstr_new(E, p->token, KEEP_RAW); }
       | st_txt_char            { $$ = ie_dstr_new(E, p->token, KEEP_RAW); }
       | st_text[t] st_txt_char { $$ = ie_dstr_append(E, $t, p->token, KEEP_RAW); }
;

st_txt_char: st_txt_inner_char | ']';

/*** CAPABILITY response ***/

capa_resp: CAPA respcheck SP capas_1[c]
    { imap_resp_arg_t arg = {.capa=$c};
      $$ = imap_resp_new(E, IMAP_RESP_CAPA, arg); };

capas_1: atom
       | capas_1[c] SP atom[a] { $$ = ie_dstr_add(E, $c, $a); }

/*** LIST/LSUB responses ***/

list_resp: LIST respcheck SP mflags[mf] SP nqchar SP mailbox[mbx]
    { imap_resp_arg_t arg = {.list=ie_list_resp_new(E, $mf, $nqchar, $mbx)};
      $$ = imap_resp_new(E, IMAP_RESP_LIST, arg); };

lsub_resp: LSUB respcheck SP mflags[mf] SP nqchar SP mailbox[mbx]
    { imap_resp_arg_t arg = {.lsub=ie_list_resp_new(E, $mf, $nqchar, $mbx)};
      $$ = imap_resp_new(E, IMAP_RESP_LSUB, arg); };

/* nqchar can't handle spaces, so an post-nqchar MODE() call is required */
nqchar: prenqchar NIL                 { $$ = 0; MODE(STD); }
      | prenqchar '"' qchar[q] '"'  { $$ = $q; MODE(STD); }
;

prenqchar: %empty { MODE(NQCHAR); };

qchar: QCHAR {
    // the scanner only returns QCHAR with 1- or 2-char matches.
    // the 2-char matches are \\ and \"
    $$ = (p->token->len == 1) ? p->token->data[0] : p->token->data[1];
};

/*** STATUS response ***/

status_resp: STATUS respcheck SP mailbox[m] SP '(' s_attr_rlist_0[sa] ')'
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

flags_resp: FLAGS respcheck SP '(' flags_0[f] ')'
    { imap_resp_arg_t arg = {.flags=$f};
      $$ = imap_resp_new(E, IMAP_RESP_FLAGS, arg); };

/*** SEARCH response ***/

search_resp: SEARCH respcheck
                        { imap_resp_arg_t arg = {
                              .search=ie_search_resp_new(E, NULL, false, 0),
                          };
                          $$ = imap_resp_new(E, IMAP_RESP_SEARCH, arg); }
           | SEARCH respcheck SP nums_1[n]
                        { imap_resp_arg_t arg = {
                              .search=ie_search_resp_new(E, $n, false, 0),
                          };
                          $$ = imap_resp_new(E, IMAP_RESP_SEARCH, arg); }
           | SEARCH respcheck SP nums_1[n] SP
                '(' MODSEQ SP nzmodseqnum[s] ')'
                        { extension_assert_on_builder(E, p->exts, EXT_CONDSTORE);
                          imap_resp_arg_t arg = {
                              .search=ie_search_resp_new(E, $n, true, $s),
                          };
                          $$ = imap_resp_new(E, IMAP_RESP_SEARCH, arg); };

/*** EXISTS response ***/

exists_resp: num SP EXISTS respcheck
    { imap_resp_arg_t arg = {.exists=$num};
      $$ = imap_resp_new(E, IMAP_RESP_EXISTS, arg); };

/*** RECENT response ***/

recent_resp: num SP RECENT respcheck
    { imap_resp_arg_t arg = {.recent=$num};
      $$ = imap_resp_new(E, IMAP_RESP_RECENT, arg); };

/*** EXPUNGE response ***/

expunge_resp: num SP EXPUNGE respcheck
    { imap_resp_arg_t arg = {.expunge=$num};
      $$ = imap_resp_new(E, IMAP_RESP_EXPUNGE, arg); };

/*** FETCH response ***/

fetch_resp: num[n] SP FETCH respcheck SP '(' msg_attrs_0[ma] ')'
    { imap_resp_arg_t arg = {.fetch=ie_fetch_resp_num(E, $ma, $n)};
      $$ = imap_resp_new(E, IMAP_RESP_FETCH, arg); };

msg_attrs_0: %empty { $$ = NULL; }
           | msg_attrs_1
;

/* most of these get ignored completely, we only really need:
     - FLAGS
     - UID
     - INTERNALDATE
     - the fully body text
     - MODSEQ
   Anything else is going to be encrypted anyway. */
msg_attrs_1: f_fflags[f]  { $$ = ie_fetch_resp_flags(E, ie_fetch_resp_new(E), $f); }
           | f_uid[u]     { $$ = ie_fetch_resp_uid(E, ie_fetch_resp_new(E), $u); }
           | f_intdate[d] { $$ = ie_fetch_resp_intdate(E, ie_fetch_resp_new(E), $d); }
           | f_rfc822[r]  { $$ = ie_fetch_resp_rfc822(E, ie_fetch_resp_new(E), $r); }
           | f_modseq[s]  { $$ = ie_fetch_resp_modseq(E, ie_fetch_resp_new(E), $s); }
           | f_extra[e]   { $$ = ie_fetch_resp_add_extra(E, ie_fetch_resp_new(E), $e); }
           | ign_msg_attr { $$ = ie_fetch_resp_new(E); }

           | msg_attrs_1[m] SP f_fflags[f]  { $$ = ie_fetch_resp_flags(E, $m, $f); }
           | msg_attrs_1[m] SP f_uid[u]     { $$ = ie_fetch_resp_uid(E, $m, $u); }
           | msg_attrs_1[m] SP f_intdate[d] { $$ = ie_fetch_resp_intdate(E, $m, $d); }
           | msg_attrs_1[m] SP f_rfc822[r]  { $$ = ie_fetch_resp_rfc822(E, $m, $r); }
           | msg_attrs_1[m] SP f_modseq[s]  { $$ = ie_fetch_resp_modseq(E, $m, $s); }
           | msg_attrs_1[m] SP f_extra[e]   { $$ = ie_fetch_resp_add_extra(E, $m, $e); }
           | msg_attrs_1[m] SP ign_msg_attr { $$ = $m; }
;

/* we can parse these without throwing errors, but we ignore them */
ign_msg_attr: ENVELOPE SP '(' envelope ')'
            | RFC822 '.' TEXT SP ign_nstring
            | RFC822 '.' HEADER SP ign_nstring
            | RFC822 '.' SIZE SP NUM
            /* don't even parse anything that starts with BODY */
;


f_fflags: FLAGS SP '(' fflags_0[f] ')' { $$ = $f; };

f_rfc822: RFC822 SP file_nstring[r] { $$ = $r; };

/* of all the BODY[*] things, just support BODY[] */
f_extra: BODY '[' ']' SP file_nstring[r]
    { $$ = ie_fetch_resp_extra_new(E, NULL, NULL, $r); };

f_modseq: MODSEQ SP '(' nzmodseqnum[s] ')'
    { extension_assert_on_builder(E, p->exts, EXT_CONDSTORE);
      $$ = $s; };

/* this will some day write to a file instead of memory, otherwise it would
   just be "nstring" type */
file_nstring: NIL { $$ = ie_dstr_new_empty(E); }
              | string
;

f_uid: UID SP num[n] { $$ = $n; };

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

enabled_resp: ENABLED respcheck SP capas_1[c]
    { extension_assert_on_builder(E, p->exts, EXT_ENABLE);
      imap_resp_arg_t arg = {.enabled=$c};
      $$ = imap_resp_new(E, IMAP_RESP_ENABLED, arg); };

/*** VANISHED response ***/

vanished_resp: VANISHED respcheck SP uid_set[s]
                { extension_assert_on_builder(E, p->exts, EXT_QRESYNC);
                  imap_resp_arg_t arg = {
                      .vanished=ie_vanished_resp_new(E, false, $s)
                  };
                  $$ = imap_resp_new(E, IMAP_RESP_VANISHED, arg); }
             | VANISHED respcheck SP '(' EARLIER ')' SP uid_set[s]
                { extension_assert_on_builder(E, p->exts, EXT_QRESYNC);
                  imap_resp_arg_t arg = {
                      .vanished=ie_vanished_resp_new(E, true, $s)
                  };
                  $$ = imap_resp_new(E, IMAP_RESP_VANISHED, arg); }
;

/*** XKEYSYNC response ***/

xkeysync_resp: XKEYSYNC respcheck SP xkeysync_resp_[r] { $$ = $r; };

xkeysync_resp_: CREATED SP astring[c]
                 { extension_assert_on_builder(E, p->exts, EXT_XKEY);
                   ie_xkeysync_resp_t *xkeysync = ie_xkeysync_resp_new(E, $c, NULL);
                   imap_resp_arg_t arg = { .xkeysync = xkeysync };
                   $$ = imap_resp_new(E, IMAP_RESP_XKEYSYNC, arg); }
              | DELETED SP astring[d]
                 { extension_assert_on_builder(E, p->exts, EXT_XKEY);
                   ie_xkeysync_resp_t *xkeysync = ie_xkeysync_resp_new(E, NULL, $d);
                   imap_resp_arg_t arg = { .xkeysync = xkeysync };
                   $$ = imap_resp_new(E, IMAP_RESP_XKEYSYNC, arg); }
              | OK
                 { extension_assert_on_builder(E, p->exts, EXT_XKEY);
                   imap_resp_arg_t arg = {0};
                   $$ = imap_resp_new(E, IMAP_RESP_XKEYSYNC, arg); }
;

/*** PLUS response ***/

plus_resp: '+' respcheck SP st_code[c] st_text[tx]
    { ie_plus_resp_t *plus = ie_plus_resp_new(E, $c, $tx);
      imap_resp_arg_t arg = { .plus = plus };
      $$ = imap_resp_new(E, IMAP_RESP_PLUS, arg); };


/*** start of "helper" categories: ***/
sc_keyword: ALERT
          | APPENDUID
          | CAPA
          | CLOSED
          | COPYUID
          | HIMODSEQ
          | MODIFIED
          | NOMODSEQ
          | PARSE
          | PERMFLAGS
          | READ_ONLY
          | READ_WRITE
          | TRYCREATE
          | UIDNEXT
          | UIDNOSTICK
          | UIDVLD
          | UNSEEN
;

flag_keyword: ANSWERED
            | DELETED
            | DRAFT
            | FLAGGED
            | RECENT
            | SEEN
;

mflag_keyword: MARKED
             | NOINFERIORS
             | NOSELECT
             | UNMARKED
;

mailbox_keyword: INBOX;

misc_keyword: ALL
            | APPEND
            | APR
            | AUG
            | AUTHENTICATE
            | BAD
            | BCC
            | BEFORE
            | BODYSTRUCT
            | BODY
            | BYE
            | CC
            | CHGSINCE
            | CHARSET
            | CHECK
            | CLOSE
            | CONDSTORE
            | COPY
            | CREATE
            | CREATED
            | DEC
            | DELETE
            | DONE
            | EARLIER
            | ENABLED
            | ENABLE
            | ENVELOPE
            | EXAMINE
            | EXISTS
            | EXPUNGE
            | FAST
            | FEB
            | FETCH
            | FIELDS
            | FLAGS
            | FROM
            | FULL
            | HEADER
            | IDLE
            | INTDATE
            | JAN
            | JUL
            | JUN
            | KEYWORD
            | LARGER
            | LIST
            | LOGIN
            | LOGOUT
            | LSUB
            | MAR
            | MAY
            | MESSAGES
            | MIME
            | MODSEQ
            | NEW
            | NIL
            | NOOP
            | NOT
            | NO
            | NOV
            | OCT
            | OK
            | OLD
            | ON
            | OR
            | PEEK
            | PREAUTH
            | PRIV
            | QRESYNC
            | RENAME
            | RFC822
            | SEARCH
            | SELECT
            | SENTBEFORE
            | SENTON
            | SENTSINCE
            | SEP
            | SHARED
            | SILENT
            | SINCE
            | SIZE
            | SMALLER
            | STARTTLS
            | STATUS
            | STORE
            | SUBJECT
            | SUBSCRIBE
            | TEXT
            | TO
            | UID
            | UNANSWERED
            | UNCHGSINCE
            | UNDELETED
            | UNDRAFT
            | UNFLAGGED
            | UNKEYWORD
            | UNSELECT
            | UNSUBSCRIBE
            | VANISHED
            | XKEYADD
            | XKEYSYNC
;

keyword: sc_keyword | flag_keyword | mflag_keyword | mailbox_keyword | misc_keyword;

raw: RAW | NUM | keyword;

// punctuation that is sometimes meaningful but is allowed in atoms anyway
tag_punc:  ':' | '.' | '}' | '[' | ',' | '<' | '>' | '-';
atom_punc: tag_punc | '+';
atom_char: raw | atom_punc;

atom: atom_char          { $$ = ie_dstr_new(E, p->token, KEEP_RAW); }
    | atom[a] atom_char  { $$ = ie_dstr_append(E, $a, p->token, KEEP_RAW); }
;

atom_sc_char: RAW | NUM | atom_punc | flag_keyword | mflag_keyword | mailbox_keyword | misc_keyword;
atom_sc: atom_sc_char             { $$ = ie_dstr_new(E, p->token, KEEP_RAW); }
       | atom_sc[a] atom_sc_char  { $$ = ie_dstr_append(E, $a, p->token, KEEP_RAW); }
;

atom_flag_char: RAW | NUM | atom_punc | RECENT | sc_keyword | mflag_keyword | mailbox_keyword | misc_keyword;
atom_flag: atom_flag_char               { $$ = ie_dstr_new(E, p->token, KEEP_RAW); }
         | atom_flag[a] atom_flag_char  { $$ = ie_dstr_append(E, $a, p->token, KEEP_RAW); }
;

atom_fflag_char: RAW | NUM | atom_punc | sc_keyword | mflag_keyword | mailbox_keyword | misc_keyword;
atom_fflag: atom_fflag_char                { $$ = ie_dstr_new(E, p->token, KEEP_RAW); }
          | atom_fflag[a] atom_fflag_char  { $$ = ie_dstr_append(E, $a, p->token, KEEP_RAW); }
;

atom_mflag_char: RAW | NUM | atom_punc | sc_keyword | flag_keyword | mailbox_keyword | misc_keyword;
atom_mflag: atom_mflag_char                { $$ = ie_dstr_new(E, p->token, KEEP_RAW); }
          | atom_mflag[a] atom_mflag_char  { $$ = ie_dstr_append(E, $a, p->token, KEEP_RAW); }
;


// characters allowed in astring but not atom
resp_specials: ']';
astring_atom_char: atom_char
                 | resp_specials
;
astring_atom: astring_atom_char                  { $$ = ie_dstr_new(E, p->token, KEEP_RAW); }
            | astring_atom[a] astring_atom_char  { $$ = ie_dstr_append(E, $a, p->token, KEEP_RAW); }
;

// tag is ASTRING-CHAR except '+'
tag_char: raw | tag_punc | resp_specials;
tag: tag_char         { $$ = ie_dstr_new(E, p->token, KEEP_RAW); }
   | tag[t] tag_char  { $$ = ie_dstr_append(E, $t, p->token, KEEP_RAW); }
;

astring_mbx_char: RAW | NUM | atom_punc | resp_specials | sc_keyword | flag_keyword | mflag_keyword | misc_keyword;
astring_mbox: astring_mbx_char                  { $$ = ie_dstr_new(E, p->token, KEEP_RAW); }
            | astring_mbox[a] astring_mbx_char  { $$ = ie_dstr_append(E, $a, p->token, KEEP_RAW); }
;


qstr: '"' preqstr qstr_body[q] postqstr '"' { $$ = $q; };
preqstr: %empty { MODE(QSTRING); };
postqstr: %empty { MODE(STD); };

qstr_body: %empty           { $$ = ie_dstr_new_empty(E); }
         | qstr_body[q] raw { $$ = ie_dstr_append(E, $q, p->token, KEEP_QSTRING); }
;

ign_qstr: '"' preqstr ign_qstr_body postqstr '"';

ign_qstr_body: %empty
             | ign_qstr_body raw
;


literal_start: '{' num[n] '}' EOL { LITERAL_START($n, true); };

//  literal: LITERAL { LITERAL_START; } literal_body_0[l]  LITERAL_END { $$ = $l; };
literal: literal_start literal_body_0[l]  LITERAL_END { $$ = $l; };

literal_body_0: %empty          { $$ = ie_dstr_new_empty(E); }
              | literal_body_1
;

literal_body_1: raw                   { $$ = ie_dstr_new(E, p->token, KEEP_RAW); }
              | literal_body_1[l] raw { $$ = ie_dstr_append(E, $l, p->token, KEEP_RAW); }
;

// ign_literal: LITERAL { LITERAL_START; } ign_literal_body;
ign_literal: literal_start ign_literal_body;

ign_literal_body: raw
                | ign_literal_body raw
;

string: qstr
      | literal
;

ign_string: ign_qstr
          | ign_literal
;

astring: astring_atom
       | string
;

astring_1: astring
         | astring_1[l] SP astring[s]  { $$ = ie_dstr_add(E, $l, $s); }
;


mailbox: string[m]       { $$ = ie_mailbox_new_noninbox(E, $m); }
       | astring_mbox[m] { $$ = ie_mailbox_new_noninbox(E, $m); }
       | INBOX           { $$ = ie_mailbox_new_inbox(E); }
;

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
           sign twodigit[zh] twodigit[zm] post_date_time '"'
           { $$ = (imap_time_t){.year   = $y,
                                .month  = $date_month,
                                .day    = $date_day_fixed,
                                .hour   = $h,
                                .min    = $m,
                                .sec    = $s,
                                .z_hour = $sign * $zh,
                                .z_min  = $zm }; };

pre_date_time: %empty { MODE(DATETIME); };
post_date_time: %empty { MODE(STD); };

date_day_fixed: ' ' digit       { $$ = $digit; }
              | digit digit     { $$ = 10*$1 + $2; }
;

date_month: JAN { $$ = 1; }
          | FEB { $$ = 2; }
          | MAR { $$ = 3; }
          | APR { $$ = 4; }
          | MAY { $$ = 5; }
          | JUN { $$ = 6; }
          | JUL { $$ = 7; }
          | AUG { $$ = 8; }
          | SEP { $$ = 9; }
          | OCT { $$ = 10; }
          | NOV { $$ = 11; }
          | DEC { $$ = 12; }
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

num_str: NUM             { $$ = ie_dstr_new(E, p->token, KEEP_RAW); }
       | num_str[n] NUM  { $$ = ie_dstr_append(E, $n, p->token, KEEP_RAW); }
;

num: num_str[n] { PARSE_NUM($n, dstr_tou, &$$); };
nznum: num_str[n] { PARSE_NUM($n, dstr_tou, &$$); NZ($$); };
modseqnum: num_str[n] { PARSE_NUM($n, dstr_tou64, &$$); };
nzmodseqnum: num_str[n] { PARSE_NUM($n, dstr_tou64, &$$); NZ($$); };

seq_spec: seq_num[n]                  { $$ = ie_seq_set_new(E, $n, $n); }
        | seq_num[n1] ':' seq_num[n2] { $$ = ie_seq_set_new(E, $n1, $n2); }
;

seq_num: nznum | '*' { $$ = 0; };

nums_1: num              { $$ = ie_nums_new(E, $num); }
      | nums_1[l] SP num { $$ = ie_nums_append(E, $l, ie_nums_new(E, $num)); }
;

seq_set: seq_spec[set]                   { $$ = $set; }
       | seq_set[set] ',' seq_spec[spec] { $$ = ie_seq_set_append(E, $set, $spec); }
;

/* workaround a bug in dovecot v2.3.13-14 where UID EXPUNGE 1 returns
   VANISHED 0:1 instead of VANISHED 1: just change the 0 to a 1 */
uid_nznum: num_str[n] { PARSE_NUM($n, dstr_tou, &$$); if($$ == 0) $$ = 1; };

/* uid_set is like seq_set except without '*' values */
uid_spec: uid_nznum[n]                    { $$ = ie_seq_set_new(E, $n, $n); }
        | uid_nznum[n1] ':' uid_nznum[n2] { $$ = ie_seq_set_new(E, $n1, $n2); }
;

uid_set: uid_spec[set]                   { $$ = $set; }
       | uid_set[set] ',' uid_spec[spec] { $$ = ie_seq_set_append(E, $set, $spec); }


/* flags, used by APPEND commands, STORE commands, and FLAGS responses */

flags_0: %empty     { $$ = NULL; }
       | flags_1
;

flags_1: flag_simple[s]     { $$ = ie_flags_add_simple(E, ie_flags_new(E), $s); }
       | '\\' atom_flag[e]  { $$ = ie_flags_add_ext(E, ie_flags_new(E), $e); }
       | atom [k]           { $$ = ie_flags_add_kw(E, ie_flags_new(E), $k); }
       | flags_1[f] SP flag_simple[s]    { $$ = ie_flags_add_simple(E, $f, $s); }
       | flags_1[f] SP '\\' atom_flag[e] { $$ = ie_flags_add_ext(E, $f, $e); }
       | flags_1[f] SP atom[k]           { $$ = ie_flags_add_kw(E, $f, $k); }
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

pflags_1: pflag_simple[s]   { $$ = ie_pflags_add_simple(E, ie_pflags_new(E), $s); }
        | '\\' atom_flag[e] { $$ = ie_pflags_add_ext(E, ie_pflags_new(E), $e); }
        | atom [k]          { $$ = ie_pflags_add_kw(E, ie_pflags_new(E), $k); }
        | pflags_1[pf] SP pflag_simple[s]   { $$ = ie_pflags_add_simple(E, $pf, $s); }
        | pflags_1[pf] SP '\\' atom_flag[e] { $$ = ie_pflags_add_ext(E, $pf, $e); }
        | pflags_1[pf] SP atom[k]           { $$ = ie_pflags_add_kw(E, $pf, $k); }
;

pflag_simple: '\\' ANSWERED { $$ = IE_PFLAG_ANSWERED; }
            | '\\' FLAGGED  { $$ = IE_PFLAG_FLAGGED; }
            | '\\' DELETED  { $$ = IE_PFLAG_DELETED; }
            | '\\' SEEN     { $$ = IE_PFLAG_SEEN; }
            | '\\' DRAFT    { $$ = IE_PFLAG_DRAFT; }
            | '\\' '*'      { $$ = IE_PFLAG_ASTERISK; }
;

/* only one of these in a list */
mflag_select: '\\' NOSELECT    { $$ = IE_SELECTABLE_NOSELECT; }
            | '\\' MARKED      { $$ = IE_SELECTABLE_MARKED; }
            | '\\' UNMARKED    { $$ = IE_SELECTABLE_UNMARKED; }
;

/* fflags, only used by FETCH responses */

fflags_0: %empty     { $$ = ie_fflags_new(E); }
        | fflags_1
;

fflags_1: fflag_simple[s]    { $$ = ie_fflags_add_simple(E, ie_fflags_new(E), $s); }
        | '\\' atom_fflag[e] { $$ = ie_fflags_add_ext(E, ie_fflags_new(E), $e); }
        | atom [k]           { $$ = ie_fflags_add_kw(E, ie_fflags_new(E), $k); }
        | fflags_1[f] SP fflag_simple[s]    { $$ = ie_fflags_add_simple(E, $f, $s); }
        | fflags_1[f] SP '\\' atom_fflag[e] { $$ = ie_fflags_add_ext(E, $f, $e); }
        | fflags_1[f] SP atom[k]            { $$ = ie_fflags_add_kw(E, $f, $k); }
;

fflag_simple: '\\' ANSWERED { $$ = IE_FFLAG_ANSWERED; }
            | '\\' FLAGGED  { $$ = IE_FFLAG_FLAGGED; }
            | '\\' DELETED  { $$ = IE_FFLAG_DELETED; }
            | '\\' SEEN     { $$ = IE_FFLAG_SEEN; }
            | '\\' DRAFT    { $$ = IE_FFLAG_DRAFT; }
            | '\\' RECENT   { $$ = IE_FFLAG_RECENT; }
;

/* mflags, only used by LIST and LSUB responses */

mflags: '(' mflags_0[mf] ')' { $$ = $mf; };

mflags_0: %empty     { $$ = NULL; }
        | mflags_1
;

mflags_1: '\\' NOINFERIORS   { $$ = ie_mflags_add_noinf(E, ie_mflags_new(E)); }
        | mflag_select[s]    { MFLAG_SELECT($$, ie_mflags_new(E), $s); }
        | '\\' atom_mflag[e] { $$ = ie_mflags_add_ext(E, ie_mflags_new(E), $e); }
        | mflags_1[mf] SP '\\' NOINFERIORS   { $$ = ie_mflags_add_noinf(E, $mf); }
        | mflags_1[mf] SP mflag_select[s]    { MFLAG_SELECT($$, $mf, $s); }
        | mflags_1[mf] SP '\\' atom_mflag[e] { $$ = ie_mflags_add_ext(E, $mf, $e); }
;

SP: ' ';
