%{
    #include <stdio.h>
    #include <imap_parse.h>
    #include <imap_expression.h>
    #include <logger.h>

    #define MODE(m) p->scan_mode = SCAN_MODE_ ## m

    #define START_QSTR p->preqstr_mode = p->scan_mode; MODE(QSTRING);
    #define END_QSTR p->scan_mode = p->preqstr_mode;

    // a YYACCEPT wrapper that resets some custom parser details
    #define ACCEPT \
        MODE(TAG); \
        p->keep = false; \
        YYACCEPT

    // must be a macro because it has to be able to call YYERROR
    #define LITERAL_START { \
        /* get the numbers from the literal, ex: {5}\r\nBYTES */ \
        /*                                       ^^^^^^^ -> LITERAL token */ \
        dstr_t sub = dstr_sub(p->token, 1, p->token->len - 3); \
        unsigned int len; \
        derr_t e = dstr_tou(&sub, &len, 10); \
        /* an error here is a proper syntax error */ \
        if(e.type != E_NONE){ \
            DROP(e); \
            /* TODO: call yyerror explicitly here, see info bison, apdx A */ \
            YYERROR; \
        } \
        /* put scanner in literal mode */ \
        set_scanner_to_literal_mode(p, len); \
    }

    #define PARSE_NUM(out){ \
        unsigned int num ; \
        derr_t e = dstr_tou(p->token, &num, 10); \
        /* an error here is a proper syntax error */ \
        if(e.type != E_NONE){ \
            DROP(e); \
            /* TODO: call yyerror explicitly here, see info bison, apdx A */ \
            YYERROR; \
        } \
        (out) = num; \
    }

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

%type <dstr> tag
%type <dstr> atom
%type <dstr> atom_
%type <dstr> qstr
%type <dstr> qstr_
%type <dstr> qstr_body_
%type <dstr> literal
%type <dstr> literal_
%type <dstr> literal_body_
%type <dstr> astring
%type <dstr> astring_
%type <dstr> string_
%type <dstr> search_charset
%type <dstr> search_astring
%type <dstr> search_atom
%destructor { ie_dstr_free($$); } <dstr>

%type <mailbox> mailbox_
%type <mailbox> mailbox
%destructor { ie_mailbox_free($$); } <mailbox>

%type <st_attr> st_attr
%type <st_attr_cmd> st_attr_clist_1
// no destructor needed

%type <time> search_date;
%type <time> date_time;
%type <time> date;
%type <time> append_time;

%type <sign> sign

%type <num> num
%type <num> digit
%type <num> twodigit
%type <num> fourdigit
//%type <num> sc_num
%type <num> date_month
%type <num> date_day
%type <num> date_day_fixed
%type <num> seq_num

%type <aflag> aflag_simple

%type <aflags> append_flags
%type <aflags> append_flags_0
%type <aflags> append_flags_1
%destructor { ie_aflags_free($$); } <aflags>

%type <boolean> uid_mode

%type <seq_spec> seq_spec

%type <seq_set> preseq
%type <seq_set> seq_set
%destructor { ie_seq_set_free($$); } <seq_set>

%type <search_key> search_key
%type <search_key> search_keys_1
%type <search_key> search_hdr
%type <search_key> search_or
%destructor { ie_search_key_free($$); } <search_key>

%% /********** Grammar Section **********/

line: command EOL { ACCEPT; }
    | response EOL { ACCEPT; }
;

//tagged: tag SP status_type_resp[s]   { ST_RESP($tag, $s); }

//       | tag SP CHECK { CHECK_CMD($tag); };
//       | tag SP CLOSE { CLOSE_CMD($tag); };
//       | tag SP EXPUNGE { EXPUNGE_CMD($tag); };

//       | fetch_cmd
//       | store_cmd
//       | copy_cmd
//       | tag SP UID
//;

command:
         tag SP STARTTLS /* TODO: respond BAD, we expect to already be in TLS */
       | tag SP AUTHENTICATE SP ign_atom /* TODO: respond BAD, we only support LOGIN */
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

       | search_cmd
;

response: untag
;

untag: '*' { MODE(COMMAND); };

/*** LOGIN command ***/

login_cmd: tag SP LOGIN SP { MODE(ASTRING); } astring[u] SP astring[p]
         { login_cmd(p, $tag, $u, $p); };

/*** SELECT command ***/

select_cmd: tag SP SELECT SP mailbox[m] { select_cmd(p, $tag, $m); };

/*** EXAMINE command ***/

examine_cmd: tag SP EXAMINE SP mailbox[m] { examine_cmd(p, $tag, $m); };

/*** CREATE command ***/

create_cmd: tag SP CREATE SP mailbox[m] { create_cmd(p, $tag, $m); };

/*** DELETE command ***/

delete_cmd: tag SP DELETE SP mailbox[m] { delete_cmd(p, $tag, $m); };

/*** RENAME command ***/

rename_cmd: tag SP RENAME SP mailbox[o] SP mailbox[n]
          { rename_cmd(p, $tag, $o, $n); };

/*** SUBSCRIBE command ***/

subscribe_cmd: tag SP SUBSCRIBE SP mailbox[m]
             { subscribe_cmd(p, $tag, $m); };

/*** UNSUBSCRIBE command ***/

unsubscribe_cmd: tag SP UNSUBSCRIBE SP mailbox[m]
               { unsubscribe_cmd(p, $tag, $m); };

/*** LIST command ***/

list_cmd: tag SP LIST SP mailbox[m] { MODE(WILDCARD); } SP astring[pattern]
        { list_cmd(p, $tag, $m, $pattern); };

/*** LSUB command ***/

lsub_cmd: tag SP LSUB SP mailbox[m] { MODE(WILDCARD); } SP astring[pattern]
        { lsub_cmd(p, $tag, $m, $pattern); };

/*** STATUS command ***/

status_cmd: tag SP STATUS SP mailbox[m]
          { MODE(ST_ATTR); } SP '(' st_attr_clist_1[sa] ')'
          { status_cmd(p, $tag, $m, $sa); };

st_attr_clist_1: st_attr[s]                         { $$ = $s; }
               | st_attr_clist_1[old] SP st_attr[s] { $$ = $old | $s; }
;

/*** APPEND command ***/

append_cmd: tag SP APPEND SP mailbox[m]
          { MODE(FLAG); } SP append_flags[f] append_time[t]
          { MODE(ASTRING); } literal[l]
          { append_cmd(p, $tag, $m, $f, $t, $l); };

append_flags: %empty                        { $$ = NULL; }
            | '(' append_flags_0[af] ')' SP { $$ = $af; }
;

append_flags_0: %empty            { $$ = NULL; }
              | append_flags_1[l] { $$ = $l; }
;

append_flags_1: aflag_simple[s] { $$ = ie_aflags_add_simple(p, ie_aflags_new(p), $s); }
              | '\\' atom[e]    { $$ = ie_aflags_add_ext(p, ie_aflags_new(p), $e); }
              | atom [k]        { $$ = ie_aflags_add_kw(p, ie_aflags_new(p), $k); }
              | append_flags_1[af] SP aflag_simple[s] { $$ = ie_aflags_add_simple(p, $af, $s); }
              | append_flags_1[af] SP '\\' atom[e]    { $$ = ie_aflags_add_ext(p, $af, $e); }
              | append_flags_1[af] SP atom[k]         { $$ = ie_aflags_add_kw(p, $af, $k); }
;

aflag_simple: '\\' ANSWERED { $$ = IE_AFLAG_ANSWERED; }
            | '\\' FLAGGED  { $$ = IE_AFLAG_FLAGGED; }
            | '\\' DELETED  { $$ = IE_AFLAG_DELETED; }
            | '\\' SEEN     { $$ = IE_AFLAG_SEEN; }
            | '\\' DRAFT    { $$ = IE_AFLAG_DRAFT; }
;

append_time: %empty             { $$ = (imap_time_t){0}; }
           | date_time[dt] SP   { $$ = $dt; }
;

/*** UID (modifier of other commands) ***/
uid_mode: %empty { $$ = false; }
        | UID SP { $$ = true; }
;

/*** SEARCH command ***/

search_cmd: tag SP uid_mode[u] SEARCH SP
            { MODE(SEARCH); } search_charset[c]
            { MODE(SEARCH); } search_keys_1[k]
            { search_cmd(p, $tag, $u, $c, $k); };

search_charset: %empty { $$ = NULL; }
              | CHARSET SP search_astring[s] SP { $$ = $s; MODE(SEARCH); }
;

search_keys_1: search_key[k]                     { $$ = $k; MODE(SEARCH); }
             | search_keys_1[a] SP search_key[b]
               { $$ = ie_search_pair(p, IE_SEARCH_AND, $a, $b); MODE(SEARCH); }
;

search_key: ALL                         { $$ = ie_search_0(p, IE_SEARCH_ALL); }
          | ANSWERED                    { $$ = ie_search_0(p, IE_SEARCH_ANSWERED); }
          | DELETED                     { $$ = ie_search_0(p, IE_SEARCH_DELETED); }
          | FLAGGED                     { $$ = ie_search_0(p, IE_SEARCH_FLAGGED); }
          | NEW                         { $$ = ie_search_0(p, IE_SEARCH_NEW); }
          | OLD                         { $$ = ie_search_0(p, IE_SEARCH_OLD); }
          | RECENT                      { $$ = ie_search_0(p, IE_SEARCH_RECENT); }
          | SEEN                        { $$ = ie_search_0(p, IE_SEARCH_SEEN); }
          | UNANSWERED                  { $$ = ie_search_0(p, IE_SEARCH_UNANSWERED); }
          | UNDELETED                   { $$ = ie_search_0(p, IE_SEARCH_UNDELETED); }
          | UNFLAGGED                   { $$ = ie_search_0(p, IE_SEARCH_UNFLAGGED); }
          | UNSEEN                      { $$ = ie_search_0(p, IE_SEARCH_UNSEEN); }
          | DRAFT                       { $$ = ie_search_0(p, IE_SEARCH_DRAFT); }
          | UNDRAFT                     { $$ = ie_search_0(p, IE_SEARCH_UNDRAFT); }
          | BCC SP search_astring[s]    { $$ = ie_search_dstr(p, IE_SEARCH_BCC, $s); }
          | BODY SP search_astring[s]   { $$ = ie_search_dstr(p, IE_SEARCH_BODY, $s); }
          | CC SP search_astring[s]     { $$ = ie_search_dstr(p, IE_SEARCH_CC, $s); }
          | FROM SP search_astring[s]   { $$ = ie_search_dstr(p, IE_SEARCH_FROM, $s); }
          | KEYWORD SP search_atom[s]   { $$ = ie_search_dstr(p, IE_SEARCH_KEYWORD, $s); }
          | SUBJECT SP search_astring[s]{ $$ = ie_search_dstr(p, IE_SEARCH_SUBJECT, $s); }
          | TEXT SP search_astring[s]   { $$ = ie_search_dstr(p, IE_SEARCH_TEXT, $s); }
          | TO SP search_astring[s]     { $$ = ie_search_dstr(p, IE_SEARCH_TO, $s); }
          | UNKEYWORD SP search_atom[s] { $$ = ie_search_dstr(p, IE_SEARCH_UNKEYWORD, $s); }
          | search_hdr
          | BEFORE SP search_date[d]    { $$ = ie_search_date(p, IE_SEARCH_BEFORE, $d); }
          | ON SP search_date[d]        { $$ = ie_search_date(p, IE_SEARCH_ON, $d); }
          | SINCE SP search_date[d]     { $$ = ie_search_date(p, IE_SEARCH_SINCE, $d); }
          | SENTBEFORE SP search_date[d]{ $$ = ie_search_date(p, IE_SEARCH_SENTBEFORE, $d); }
          | SENTON SP search_date[d]    { $$ = ie_search_date(p, IE_SEARCH_SENTON, $d); }
          | SENTSINCE SP search_date[d] { $$ = ie_search_date(p, IE_SEARCH_SENTSINCE, $d); }
          | LARGER SP num[n]            { $$ = ie_search_num(p, IE_SEARCH_LARGER, $n); }
          | SMALLER SP num[n]           { $$ = ie_search_num(p, IE_SEARCH_SMALLER, $n); }
          | UID SP seq_set[s]           { $$ = ie_search_seq_set(p, IE_SEARCH_UID, $s); }
          | seq_set[s]                  { $$ = ie_search_seq_set(p, IE_SEARCH_SEQ_SET, $s); }
          | NOT SP search_key[k]        { $$ = ie_search_not(p, $k); }
          | '(' search_keys_1[k] ')'    { $$ = $k; }
          | search_or
;

search_hdr: HEADER SP search_astring[h] SP search_astring[v]
          { $$ = ie_search_header(p, IE_SEARCH_HEADER, $h, $v); MODE(SEARCH); };

search_or: OR SP search_key[a] SP search_key[b]
         { $$ = ie_search_pair(p, IE_SEARCH_OR, $a, $b); };

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

atom_: RAW                { $$ = ie_dstr_new(p, KEEP_RAW); }
     | atom_[a] atom_like_  { $$ = ie_dstr_append(p, $a, KEEP_RAW); }
;

atom_like_: RAW
          | NUM
          | keyword
;

qstr_: '"' preqstr qstr_body_[q] '"' { $$ = $q; };

preqstr: %empty { p->preqstr_mode = p->scan_mode; MODE(QSTRING); };

qstr_body_: %empty            { $$ = ie_dstr_new_empty(p); }
          | qstr_body_[q] RAW { $$ = ie_dstr_append(p, $q, KEEP_QSTRING); }
;

/* note that LITERAL_END is passed by the application after it finishes reading
   the literal from the stream; it is never returned by the scanner */
literal_: LITERAL { LITERAL_START; } literal_body_[l] { $$ = $l; };

/* the scanner produces RAW tokens until the literal lengths is met.  Even if
   the literal length is 0, at least one empty RAW token is always produced */
literal_body_: RAW                 { $$ = ie_dstr_new(p, KEEP_RAW); }
             | literal_body_[l] RAW { $$ = ie_dstr_append(p, $l, KEEP_RAW); }

string_: qstr_
       | literal_
;

astring_: atom_
        | string_
;

tag: atom[a] { $$ = $a; MODE(COMMAND); };

mailbox_: prembx astring_[a] { $$ = ie_mailbox_new_noninbox(p, $a); }
        | prembx INBOX       { $$ = ie_mailbox_new_inbox(p); }
;

prembx: %empty { MODE(MAILBOX); };

/* dummy grammar to make sure KEEP_CANCEL gets called in error handling */
prekeep: %empty { p->keep = true; };

/* the "keep" variations of the above */
atom: prekeep atom_[a] { $$ = $a; };
qstr: prekeep qstr_[q] { $$ = $q; };
literal: prekeep literal_[l] { $$ = $l; };
astring: prekeep astring_[a] { $$ = $a; };
mailbox: prekeep mailbox_[m] { $$ = $m; };

/* the "forget" variations */
ign_atom: atom_[a] { ie_dstr_free($a); };
ign_qstr: qstr_[q] { ie_dstr_free($q); };
ign_literal: literal_[l] { ie_dstr_free($l); };
ign_astring: astring_[a] { ie_dstr_free($a); };

st_attr: MESSAGES    { $$ = IE_ST_ATTR_MESSAGES; }
       | RECENT      { $$ = IE_ST_ATTR_RECENT; }
       | UIDNEXT     { $$ = IE_ST_ATTR_UIDNEXT; }
       | UIDVLD      { $$ = IE_ST_ATTR_UIDVLD; }
       | UNSEEN      { $$ = IE_ST_ATTR_UNSEEN; }
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
    | '-' { $$ = 2; }
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
            if(p->error.type == E_NONE){
                TRACE(p->error, "invalid digit: %x\n", FC(p->token->data[0]));
                TRACE_ORIG(p->error, E_INTERNAL, "invalid digit");
            }
            $$ = 0;
        }
    };

twodigit: digit digit { $$ = 10*$1 + $2; };

fourdigit: digit digit digit digit { $$ = 1000*$1 + 100*$2 + 10*$3 + $4; };

num: NUM { PARSE_NUM($$); };

// num_list_0: %empty
//           | num_list_1
// ;
//
// num_list_1: NUM
//           | num_list_1 SP NUM
// ;

seq_set: preseq[set] seq_spec[spec]      { $$ = ie_seq_set_append(p, $set, $spec); }
       | seq_set[set] ',' seq_spec[spec] { $$ = ie_seq_set_append(p, $set, $spec); }
;

preseq: %empty { MODE(SEQSET); $$ = ie_seq_set_new(p); };

seq_spec: seq_num[n]                  { $$ = ie_seq_spec_new(p, $n, $n); }
        | seq_num[n1] ':' seq_num[n2] { $$ = ie_seq_spec_new(p, $n1, $n2); }
;

seq_num: '*'    { $$ = 0; }
       | num
;


SP: ' ';
