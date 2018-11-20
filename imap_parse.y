%{
    #include <stdio.h>
    #include <imap_scan.h>
    #include <imap_parse.h>
%}

/* this defines the type of yylval, which is the semantic value of a token */
%define api.value.type {char*}
/* reentrant */
%define api.pure full
/* push parser */
%define api.push-pull push
/* create a user-data pointer in the api */
%parse-param { ixp_t *ixp }

/* some generic types */
%token ATOM
%token FLAG
%token NIL
%token NUM
%token QCHAR
%token QSTRING
%token LITERAL

/* INITIAL state */
%token TAG
%token UNTAG

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

/* STATUS_CODE state */
%token ALERT
%token BADCHARSET
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

%token EOL

%% /********** Grammar Section **********/

input: %empty
     | input response EOL {printf("EOL\n");}
;

response: tag_or_not post_tag
;

tag_or_not: TAG
          | UNTAG { printf("untag!\n"); /*yydebug = 1;*/ }
;

post_tag: status_type status_extra /* TODO: eat all text until "]" */
        | CAPABILITY atom_list
             { printf("capability!\n"); }
        | LIST post_list
             { printf("list!\n"); }
        | LSUB post_list
             { printf("lsub!\n"); }
        | STATUS /* TODO: get "mailbox" */ '(' status_att_list ')'
             { printf("status!\n"); }
        | FLAGS '(' flag_list ')'
             { printf("flags!\n"); }
        | SEARCH num_list
             { printf("search!\n"); }
        | NUM post_num
;

status_type: OK { printf("ok!\n"); }
           | NO { printf("no!\n"); }
           | BAD { printf("bad!\n"); }
           | PREAUTH { printf("preauth!\n"); }
           | BYE { printf("bye!\n"); }
;

status_extra: '[' status_code ']'
            | %empty
;

status_code: ALERT
                { printf("ALERT!\n"); }
           | BADCHARSET '(' astring_list ')'
                { printf("BADCHARSET!\n"); }
           | CAPABILITY atom_list
                { printf("capability (code)!\n"); }
           | PARSE
                { printf("parse!\n"); }
           | PERMANENTFLAGS '(' flag_list ')'
                { printf("perm-flags!\n"); }
           | READ_ONLY
                { printf("readonly!\n"); }
           | READ_WRITE
                { printf("readwrite!\n"); }
           | TRYCREATE
                { printf("trycreate!\n"); }
           | UIDNEXT NUM
                { printf("uidnext!\n"); }
           | UIDVALIDITY NUM
                { printf("uid_validity!\n"); }
           | UNSEEN NUM
                { printf("unseen!\n"); }
           | ATOM /* TODO: eat all text until "]" */
                { printf("atom!\n"); }
;

post_list: '(' flag_list ')' nqchar /* TODO: handle mailbox or InBoX */
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
          naddr_list naddr_list naddr_list naddr_list  nstring;

naddr_list: NIL
          | '(' addr_list ')'
;

/*           addr-name addr-adl addr-mailbox addr-host */
address: '(' nstring   nstring  nstring      nstring   ')'

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


/*** start of "helper" categories: ***/

literal: OK
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
       | BADCHARSET
       | PARSE
       | PERMANENTFLAGS
       | READ_ONLY
       | READ_WRITE
       | TRYCREATE
       | UIDNEXT
       | UIDVALIDITY
       | UNSEEN
       | MESSAGES
;

atom: ATOM
    | NUM
    | TAG
    | literal
;

atom_list: %empty
         | atom_list atom
;

string: QSTRING /* TODO: application must skip characters */
      | LITERAL /* TODO: application must skip characters */
;

astring: ATOM
       | string
;

astring_list: %empty
            | astring_list astring
;

nstring: NIL
       | string
;

flag_list: %empty
         | flag_list FLAG
;

num_list: %empty
        | num_list NUM
;
