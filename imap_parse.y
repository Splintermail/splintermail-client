%{
    #include <stdio.h>
    #include <imap_parse.h>
    #include <logger.h>
    #include <imap_expression.h>

    #define MODE(m) parser->scan_mode = SCAN_MODE_ ## m

    #define START_QSTR parser->preqstr_mode = parser->scan_mode; MODE(QSTRING);
    #define END_QSTR parser->scan_mode = parser->preqstr_mode;

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

    /* for building lists of status attributes.  Since status attributes do
       not allow for extensions, it is easy to build the list of attributes in
       the parser and only have single STATUS_CMD a STATUS_RESP hooks */
    static inline ie_st_attr_resp_t add_resp_st_attr(ie_st_attr_resp_t *old,
                                                     ie_st_attr_t attr,
                                                     unsigned int num){
        ie_st_attr_resp_t out = old ? *old : (ie_st_attr_resp_t){0};
        // set the attribute
        switch(attr){
            case IE_ST_ATTR_MESSAGES:   out.messages = num; break;
            case IE_ST_ATTR_RECENT:     out.recent = num;   break;
            case IE_ST_ATTR_UIDNEXT:    out.uidnext = num;  break;
            case IE_ST_ATTR_UIDVLD:     out.uidvld = num;   break;
            case IE_ST_ATTR_UNSEEN:     out.unseen = num;   break;
        }
        // mark that attribute as "set"
        out.attrs = out.attrs | attr;
        return out;
    }

    // the *CMD* and *RESP* macros are for calling a hooks, then freeing memory
    #define LOGIN_CMD(tag, u, p) \
        parser->hooks_dn.login(parser->hook_data, tag, u, p);

    #define SELECT_CMD(tag, mbx) \
        parser->hooks_dn.select(parser->hook_data, tag, mbx.inbox, mbx.dstr);

    #define EXAMINE_CMD(tag, mbx) \
        parser->hooks_dn.examine(parser->hook_data, tag, mbx.inbox, mbx.dstr);

    #define CREATE_CMD(tag, mbx) \
        parser->hooks_dn.create(parser->hook_data, tag, mbx.inbox, mbx.dstr);

    #define DELETE_CMD(tag, mbx) \
        parser->hooks_dn.delete(parser->hook_data, tag, mbx.inbox, mbx.dstr);

    #define RENAME_CMD(tag, old, new) \
        parser->hooks_dn.rename(parser->hook_data, tag, old.inbox, old.dstr, \
                                new.inbox, new.dstr);

    #define SUBSCRIBE_CMD(tag, mbx) \
        parser->hooks_dn.subscribe(parser->hook_data, tag, mbx.inbox, mbx.dstr);

    #define UNSUBSCRIBE_CMD(tag, mbx) \
        parser->hooks_dn.unsubscribe(parser->hook_data, tag, mbx.inbox, mbx.dstr);

    #define LIST_CMD(tag, mbx, pat) \
        parser->hooks_dn.list(parser->hook_data, tag, mbx.inbox, mbx.dstr, pat);

    #define LSUB_CMD(tag, mbx, pat) \
        parser->hooks_dn.lsub(parser->hook_data, tag, mbx.inbox, mbx.dstr, pat);

    #define STATUS_CMD(tag, mbx, sa) \
        parser->hooks_dn.status(parser->hook_data, tag,\
            mbx.inbox, mbx.dstr, \
            sa & IE_ST_ATTR_MESSAGES, \
            sa & IE_ST_ATTR_RECENT, \
            sa & IE_ST_ATTR_UIDNEXT, \
            sa & IE_ST_ATTR_UIDVLD, \
            sa & IE_ST_ATTR_UNSEEN);

    #define CHECK_CMD(tag) \
        parser->hooks_dn.check(parser->hook_data, tag);

    #define CLOSE_CMD(tag) \
        parser->hooks_dn.close(parser->hook_data, tag);

    #define EXPUNGE_CMD(tag) \
        parser->hooks_dn.expunge(parser->hook_data, tag);

    #define APPEND_CMD(tag, mbx, flags, date_time){ \
        /* get the numbers from the literal, ex: {5}\r\nBYTES */ \
        dstr_t sub = dstr_sub(parser->token, 1, parser->token->len - 3); \
        size_t len; \
        dstr_toul(&sub, &len, 10);\
        parser->hooks_dn.append(parser->hook_data, tag, mbx.inbox, \
                                mbx.dstr, flags, date_time, len); \
    }

    #define SEARCH_CMD(tag, uid, charset, search_key) \
        parser->hooks_dn.search(parser->hook_data, tag, uid, charset, \
                                search_key);

    #define FETCH_CMD(tag, uid, seq_set, fetch_attr) \
        parser->hooks_dn.fetch(parser->hook_data, tag, uid, seq_set, \
                               fetch_attr);

    #define STORE_CMD(tag, uid, seq, sign, silent, flgs) \
        parser->hooks_dn.store(parser->hook_data, tag, uid, seq, sign, \
                               silent, flgs);

    #define COPY_CMD(tag, uid, set, mbx) \
        parser->hooks_dn.copy(parser->hook_data, tag, uid, set, mbx.inbox, \
                              mbx.dstr);

    // Responses:

    #define ST_RESP(tag, st) \
        parser->hooks_up.status_type(parser->hook_data, tag, st.status, \
                                     st.code, st.code_extra, st.text );

    #define CAPA_RESP_START \
        DOCATCH( parser->hooks_up.capa_start(parser->hook_data) );
    #define CAPA_RESP(c) \
        DOCATCH( parser->hooks_up.capa(parser->hook_data, c) );
    #define CAPA_RESP_END(success) \
        parser->hooks_up.capa_end(parser->hook_data, success);

    #define PFLAG_RESP(f) \
        parser->hooks_up.pflag(parser->hook_data, f);

    #define LIST_RESP(flg, sep, mbx) \
        parser->hooks_up.list(parser->hook_data, flg, sep, mbx.inbox, mbx.dstr);

    #define LSUB_RESP(flg, sep, mbx) \
        parser->hooks_up.lsub(parser->hook_data, flg, sep, mbx.inbox, mbx.dstr);

    #define FLAGS_RESP(flg) \
        parser->hooks_up.flags(parser->hook_data, flg);

    #define STATUS_RESP(mbx, sa) \
        parser->hooks_up.status(parser->hook_data, \
            mbx.inbox, mbx.dstr, \
            sa.attrs & IE_ST_ATTR_MESSAGES, sa.messages, \
            sa.attrs & IE_ST_ATTR_RECENT, sa.recent, \
            sa.attrs & IE_ST_ATTR_UIDNEXT, sa.uidnext, \
            sa.attrs & IE_ST_ATTR_UIDVLD, sa.uidvld, \
            sa.attrs & IE_ST_ATTR_UNSEEN, sa.unseen);

    #define EXISTS_RESP(num) \
        parser->hooks_up.exists(parser->hook_data, num);

    #define RECENT_RESP(num) \
        parser->hooks_up.recent(parser->hook_data, num);

    #define EXPUNGE_RESP(num) \
        parser->hooks_up.expunge(parser->hook_data, num);

    // fetch-related hooks
    #define FETCH_RESP_START(num) \
        DOCATCH( parser->hooks_up.fetch_start(parser->hook_data, num) );

    #define F_FLAGS_RESP(flags) \
        DOCATCH( parser->hooks_up.f_flags(parser->hook_data, flags) );

    #define F_RFC822_RESP_START \
        DOCATCH( parser->hooks_up.f_rfc822_start(parser->hook_data) );
    #define F_RFC822_RESP_LITERAL { \
        /* get the numbers from the literal, ex: {5}\r\nBYTES */ \
        dstr_t sub = dstr_sub(parser->token, 1, parser->token->len - 3); \
        size_t len; \
        dstr_toul(&sub, &len, 10);\
        DOCATCH( parser->hooks_up.f_rfc822_literal(parser->hook_data, len) ); \
    }
    #define F_RFC822_RESP_QSTR \
        DOCATCH( parser->hooks_up.f_rfc822_qstr(parser->hook_data, \
                                                parser->token) );
    #define F_RFC822_RESP_END(success) \
        parser->hooks_up.f_rfc822_end(parser->hook_data, success);

    #define F_UID_RESP(num) \
        parser->hooks_up.f_uid(parser->hook_data, num);

    #define F_INTDATE_RESP(imap_time) \
        parser->hooks_up.f_intdate(parser->hook_data, imap_time);

    #define FETCH_RESP_END(success) \
        parser->hooks_up.fetch_end(parser->hook_data, success);

    // literal hook
    #define LITERAL_RESP { \
        /* get the numbers from the literal, ex: {5}\r\nBYTES
                                                 ^^^^^^^ -> LITERAL token */ \
        dstr_t sub = dstr_sub(parser->token, 1, parser->token->len - 3); \
        size_t len; \
        dstr_toul(&sub, &len, 10);\
        DOCATCH( parser->hooks_up.literal(parser->hook_data, len, \
                                          parser->keep) ); \
    }

    // allocators
    #define SECT_PART(out, list, num) \
        ie_section_part_t *temp; \
        temp = malloc(sizeof(*temp)); \
        if(!temp){ \
            parser->error = E_NOMEM; \
            ACCEPT; \
        } \
        /* append this struct to the end of the existing linked list */ \
        out = list; \
        ie_section_part_t **end = &out; \
        while(*end) end = &(*end)->next; \
        *end = temp; \
        temp->n = num; \
        temp->next = NULL

    #define SEARCH(out, typ_) \
        out = malloc(sizeof(*out)); \
        if(!out){ \
            parser->error = E_NOMEM; \
            ACCEPT; \
        } \
        out->type = IE_SEARCH_ ## typ_; \
        out->next = NULL

    #define SEARCH_1(out, typ_, t1, v1) \
        SEARCH(out, typ_); \
        out->param.t1 = v1;

    #define SEARCH_2(out, typ_, t1, v1, t2, v2) \
        SEARCH_1(out, typ_, t1, v1); \
        out->param.t2 = v2;

    #define HEADER(out, list, header) \
        dstr_link_t *temp; \
        temp = malloc(sizeof(*temp)); \
        if(!temp){ \
            parser->error = E_NOMEM; \
            ACCEPT; \
        } \
        /* append this struct to the end of the existing linked list */ \
        out = list; \
        dstr_link_t **end = &out; \
        while(*end) end = &(*end)->next; \
        *end = temp; \
        temp->dstr = header; \
        temp->next = NULL

    #define SECT(out, sp, st) \
        out = malloc(sizeof(*out)); \
        if(!out){ \
            parser->error = E_NOMEM; \
            ACCEPT; \
        } \
        *out = (ie_fetch_extra_t){ .sect_part = sp, .sect_txt = st }

    #define SEQ_SPEC(out, _n1, _n2) \
        out = malloc(sizeof(*out)); \
        if(!out){ \
            parser->error = E_NOMEM; \
            ACCEPT; \
        } \
        out->n1 = _n1; \
        out->n2 = _n2; \
        out->next = NULL

    // fusers

    #define ADD_FLAG(out, list, flag) \
        out = list; \
        dstr_link_t *temp, **end; \
        switch(flag.type){ \
            /* in standard cases, mark the flag as "set" */ \
            case IE_FLAG_ANSWERED: out.answered = true; break; \
            case IE_FLAG_FLAGGED: out.flagged = true; break; \
            case IE_FLAG_DELETED: out.deleted = true; break; \
            case IE_FLAG_SEEN: out.seen = true; break; \
            case IE_FLAG_DRAFT: out.draft = true; break; \
            case IE_FLAG_RECENT: out.recent = true; break; \
            case IE_FLAG_NOSELECT: out.noselect = true; break; \
            case IE_FLAG_MARKED: out.marked = true; break; \
            case IE_FLAG_UNMARKED: out.unmarked = true; break; \
            case IE_FLAG_ASTERISK: out.asterisk = true; break; \
            /* in the other cases, allocate a dstr_link and append to list */ \
            case IE_FLAG_KEYWORD: \
            case IE_FLAG_EXTENSION: \
                temp = malloc(sizeof(*temp)); \
                if(!temp){ \
                    parser->error = E_NOMEM; \
                    ACCEPT; \
                } \
                temp->dstr = flag.dstr; \
                temp->next = NULL; \
                if(flag.type == IE_FLAG_KEYWORD) \
                    end = &out.keywords; \
                else \
                    end = &out.extensions; \
                while(*end) end = &(*end)->next; \
                *end = temp; \
        }

    #define FUSE_SEARCH(out, list, key) \
        out = list; \
        ie_search_key_t **end = &out; \
        while(*end) end = &(*end)->next; \
        *end = key

    #define FUSE_FETCH_ATTR(out, old, new) \
        /* logical OR of all boolean flags */ \
        out.envelope      = old.envelope      || new.envelope; \
        out.flags         = old.flags         || new.flags; \
        out.intdate       = old.intdate       || new.intdate; \
        out.uid           = old.uid           || new.uid; \
        out.rfc822        = old.rfc822        || new.rfc822; \
        out.rfc822_header = old.rfc822_header || new.rfc822_header; \
        out.rfc822_size   = old.rfc822_size   || new.rfc822_size; \
        out.rfc822_text   = old.rfc822_text   || new.rfc822_text; \
        out.body          = old.body          || new.body; \
        out.bodystruct    = old.bodystruct    || new.bodystruct; \
        /* combine the linked lists of extras */ \
        out.extra = old.extra; \
        ie_fetch_extra_t **end = &out.extra; \
        while(*end){ end = &((*end)->next); } \
        *end = new.extra

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
%token SILENT

/* non-terminals with semantic values */
%type <dstr> keep_atom
%type <dstr> keep_astring
%type <dstr> tag
%type <dstr> st_txt_0
%type <dstr> st_txt_1
%type <dstr> search_charset
%type <dstr> search_atom
%type <dstr> search_astring
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

%type <flag_list> append_flags
%type <flag_list> append_flags_0
%type <flag_list> append_flags_1
%type <flag_list> store_flags
%type <flag_list> store_flags_0
%type <flag_list> store_flags_1
%type <flag_list> pflags_0
%type <flag_list> pflags_1
%type <flag_list> mflags_0
%type <flag_list> mflags_1
%type <flag_list> flags_0
%type <flag_list> flags_1
%type <flag_list> f_flags_0
%type <flag_list> f_flags_1
%destructor { ie_flag_list_free(& $$); } <flag_list>

%type <num> num
%type <num> digit
%type <num> twodigit
%type <num> fourdigit
%type <num> sc_num
%type <num> date_month
%type <num> date_day
%type <num> date_day_fixed
%type <num> seq_num

%type <sign> sign
%type <sign> store_sign

%type <boolean> mailbox
%type <mailbox> keep_mailbox
%destructor { dstr_free(& $$.dstr); } <mailbox>

%type <ch> nqchar
%type <ch> keep_qchar

%type <time> date_time;
%type <time> append_time;
%type <time> search_date;
%type <time> date;

%type <st_attr> st_attr
%type <st_attr_cmd> st_attr_clist_1
%type <st_attr_resp> st_attr_rlist_0
%type <st_attr_resp> st_attr_rlist_1

%type <seq_set> seq_set
%type <seq_set> seq_spec
%destructor { ie_seq_set_free($$); } <seq_set>

%type <boolean> store_silent
%type <boolean> uid_mode

%type <search_key> search_key
%type <search_key> search_keys_1
%type <search_key> search_hdr
%type <search_key> search_or
%destructor { ie_search_key_free($$); } <search_key>

%type <partial> partial

%type <sect_part> sect_part
%destructor { ie_section_part_free($$); } <sect_part>

%type <sect_txt> sect_txt
%type <sect_txt> sect_msgtxt
%type <sect_txt> hdr_flds
%type <sect_txt> hdr_flds_not
%destructor { dstr_link_free($$.headers); } <sect_txt>

%type <dstr_link> header_list_1
%destructor { dstr_link_free($$); } <dstr_link>

%type <fetch_extra> sect
%destructor { ie_fetch_extra_free($$); } <fetch_extra>

%type <fetch_attr> fetch_attr
%type <fetch_attr> fetch_attrs
%type <fetch_attr> fetch_attrs_1
%destructor { ie_fetch_attr_free(&$$); } <fetch_attr>

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
%destructor { END_QSTR; } <preqstring>

%type <capa> capa_start
%destructor { CAPA_RESP_END(false); } <capa>

%type <fetchresp> pre_fetch_resp
%destructor { FETCH_RESP_END(false); } <fetchresp>

%type <f_rfc822resp> pre_f_rfc822_resp
%destructor { F_RFC822_RESP_END(false); } <f_rfc822resp>

%% /********** Grammar Section **********/

response: tagged EOL { ACCEPT; };
        | untagged EOL { ACCEPT; };

tagged: tag SP status_type_resp[s]   { ST_RESP($tag, $s); };
      | tag SP STARTTLS /* respond BAD, we expect to already be in TLS */
      | tag SP AUTHENTICATE SP atom /* respond BAD, we only support LOGIN */
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
      | tag SP CHECK { CHECK_CMD($tag); };
      | tag SP CLOSE { CLOSE_CMD($tag); };
      | tag SP EXPUNGE { EXPUNGE_CMD($tag); };
      | search_cmd
      | fetch_cmd
      | store_cmd
      | copy_cmd
      | tag SP UID
;


untagged: untag SP status_type_resp[s] { ST_RESP(NUL_DSTR, $s); }
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


/*** LOGIN command ***/

login_cmd: tag SP LOGIN SP { MODE(ASTRING); } keep_astring[u] SP keep_astring[p]
           { LOGIN_CMD($tag, $u, $p); };

/*** SELECT command ***/

select_cmd: tag SP SELECT SP keep_mailbox[m] { SELECT_CMD($tag, $m); };

/*** EXAMINE command ***/

examine_cmd: tag SP EXAMINE SP keep_mailbox[m] { EXAMINE_CMD($tag, $m); };

/*** CREATE command ***/

create_cmd: tag SP CREATE SP keep_mailbox[m] { CREATE_CMD($tag, $m); };

/*** DELETE command ***/

delete_cmd: tag SP DELETE SP keep_mailbox[m] { DELETE_CMD($tag, $m); };

/*** RENAME command ***/

rename_cmd: tag SP RENAME SP keep_mailbox[o] SP keep_mailbox[n]
            { RENAME_CMD($tag, $o, $n); };

/*** SUBSCRIBE command ***/

subscribe_cmd: tag SP SUBSCRIBE SP keep_mailbox[m]
               { SUBSCRIBE_CMD($tag, $m); };

/*** UNSUBSCRIBE command ***/

unsubscribe_cmd: tag SP UNSUBSCRIBE SP keep_mailbox[m]
                 { UNSUBSCRIBE_CMD($tag, $m); };

/*** LIST command ***/

list_cmd: tag SP LIST SP keep_mailbox[m]
          { MODE(WILDCARD); } SP keep_astring[pattern]
          { LIST_CMD($tag, $m, $pattern); };

/*** LSUB command ***/

lsub_cmd: tag SP LSUB SP keep_mailbox[m]
          { MODE(WILDCARD); } SP keep_astring[pattern]
          { LSUB_CMD($tag, $m, $pattern); };

/*** STATUS command ***/

status_cmd: tag SP STATUS SP keep_mailbox[m]
            { MODE(ST_ATTR); } SP '(' st_attr_clist_1[sa] ')'
            { STATUS_CMD($tag, $m, $sa); };

st_attr_clist_1: st_attr[s]                         { $$ = $s; }
               | st_attr_clist_1[old] SP st_attr[s] { $$ = $old | $s; }
;

/*** APPEND command ***/

/* APPEND_CMD also marks a literal, hence the LITERAL_END after append_cmd_.
   Otherwise, destructors would be called incorrectly. */
append_cmd: append_cmd_ LITERAL_END

append_cmd_: tag SP APPEND SP keep_mailbox[m]
             { MODE(FLAG); } SP append_flags[f] append_time[t]
             { MODE(ASTRING); } LITERAL
             { APPEND_CMD($tag, $m, $f, $t); };

append_flags: %empty                            { $$ = (ie_flag_list_t){0}; }
            | '(' append_flags_0[af] ')' SP     { $$ = $af; }
;

append_flags_0: %empty          { $$ = (ie_flag_list_t){0}; }
              | append_flags_1
;

append_flags_1: keep_flag[f]        { ADD_FLAG($$, (ie_flag_list_t){0}, $f); }
              | append_flags_1[l] SP keep_flag[f]   { ADD_FLAG($$, $l, $f); }
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
            { SEARCH_CMD($tag, $u, $c, $k); };

search_charset: %empty { $$ = (dstr_t){0}; }
              | CHARSET SP search_astring[s] SP { $$ = $s; MODE(SEARCH); }
;

search_keys_1: search_key[k]                     { $$ = $k; MODE(SEARCH); }
             | search_keys_1[l] SP search_key[k] { FUSE_SEARCH($$, $l, $k);
                                                   MODE(SEARCH); };
;

search_key: ALL                         { SEARCH($$, ALL); }
          | ANSWERED                    { SEARCH($$, ANSWERED); }
          | DELETED                     { SEARCH($$, DELETED); }
          | FLAGGED                     { SEARCH($$, FLAGGED); }
          | NEW                         { SEARCH($$, NEW); }
          | OLD                         { SEARCH($$, OLD); }
          | RECENT                      { SEARCH($$, RECENT); }
          | SEEN                        { SEARCH($$, SEEN); }
          | UNANSWERED                  { SEARCH($$, UNANSWERED); }
          | UNDELETED                   { SEARCH($$, UNDELETED); }
          | UNFLAGGED                   { SEARCH($$, UNFLAGGED); }
          | UNSEEN                      { SEARCH($$, UNSEEN); }
          | DRAFT                       { SEARCH($$, DRAFT); }
          | UNDRAFT                     { SEARCH($$, UNDRAFT); }
          | BCC SP search_astring[s]    { SEARCH_1($$, BCC, dstr, $s); }
          | BODY SP search_astring[s]   { SEARCH_1($$, BODY, dstr, $s); }
          | CC SP search_astring[s]     { SEARCH_1($$, CC, dstr, $s); }
          | FROM SP search_astring[s]   { SEARCH_1($$, FROM, dstr, $s); }
          | KEYWORD SP search_atom[s]   { SEARCH_1($$, KEYWORD, dstr, $s); }
          | SUBJECT SP search_astring[s]{ SEARCH_1($$, SUBJECT, dstr, $s); }
          | TEXT SP search_astring[s]   { SEARCH_1($$, TEXT, dstr, $s); }
          | TO SP search_astring[s]     { SEARCH_1($$, TO, dstr, $s); }
          | UNKEYWORD SP search_atom[s] { SEARCH_1($$, UNKEYWORD, dstr, $s); }
          | search_hdr
          | BEFORE SP search_date[d]    { SEARCH_1($$, BEFORE, date, $d); }
          | ON SP search_date[d]        { SEARCH_1($$, ON, date, $d); }
          | SINCE SP search_date[d]     { SEARCH_1($$, SINCE, date, $d); }
          | SENTBEFORE SP search_date[d]{ SEARCH_1($$, SENTBEFORE, date, $d); }
          | SENTON SP search_date[d]    { SEARCH_1($$, SENTON, date, $d); }
          | SENTSINCE SP search_date[d] { SEARCH_1($$, SENTSINCE, date, $d); }
          | LARGER SP num[n]            { SEARCH_1($$, LARGER, num, $n); }
          | SMALLER SP num[n]           { SEARCH_1($$, SMALLER, num, $n); }
          | UID SP seq_set[s]           { SEARCH_1($$, UID, seq_set, $s); }
          | seq_set[s]                  { SEARCH_1($$, SEQ_SET, seq_set, $s); }
          | NOT SP search_key[k]        { SEARCH_1($$, NOT, search_key, $k); }
          | '(' search_keys_1[k] ')'    { SEARCH_1($$, GROUP, search_key, $k); }
          | search_or
;

search_hdr: HEADER SP { MODE(ASTRING); } keep_astring[s1] SP keep_astring[s2]
            { SEARCH_2($$, HEADER, header.name, $s1, header.value, $s2);
              MODE(SEARCH); };

search_or: OR SP search_key[k1] SP search_key[k2]
           { SEARCH_2($$, OR, search_or.a, $k1, search_or.b, $k2); };

search_astring: { MODE(ASTRING); } keep_astring[k] { $$ = $k; };

search_atom: { MODE(ATOM); } keep_atom[k] { $$ = $k; };

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
           { MODE(FETCH); } fetch_attrs[attr]
           { FETCH_CMD($tag, $u, $seq, $attr); };

fetch_attrs: ALL           { $$ = (ie_fetch_attr_t){ .flags = true,
                                                     .intdate = true,
                                                     .rfc822_size = true,
                                                     .envelope = true }; }
           | FAST          { $$ = (ie_fetch_attr_t){ .flags = true,
                                                     .intdate = true,
                                                     .rfc822_size = true }; }
           | FULL          { $$ = (ie_fetch_attr_t){ .flags = true,
                                                     .intdate = true,
                                                     .rfc822_size = true,
                                                     .envelope = true,
                                                     .body = true }; }
           | fetch_attr
           | '(' fetch_attrs_1 ')'  { $$ = $fetch_attrs_1; }
;

fetch_attrs_1: fetch_attr
             | fetch_attrs_1[a] SP fetch_attr[b] { FUSE_FETCH_ATTR($$, $a, $b); }
;

fetch_attr: ENVELOPE      { $$ = (ie_fetch_attr_t){ .envelope = true }; }
          | FLAGS         { $$ = (ie_fetch_attr_t){ .flags = true }; }
          | INTDATE       { $$ = (ie_fetch_attr_t){ .intdate = true }; }
          | UID           { $$ = (ie_fetch_attr_t){ .uid = true }; }
          | RFC822        { $$ = (ie_fetch_attr_t){ .uid = true }; }
          | RFC822_HEADER { $$ = (ie_fetch_attr_t){ .rfc822_header = true }; }
          | RFC822_SIZE   { $$ = (ie_fetch_attr_t){ .rfc822_size = true }; }
          | RFC822_TEXT   { $$ = (ie_fetch_attr_t){ .rfc822_text = true }; }
          | BODYSTRUCT    { $$ = (ie_fetch_attr_t){ .bodystruct = true }; }
          | BODY          { $$ = (ie_fetch_attr_t){ .body = true }; }
          | BODY '[' sect ']' partial
                          { $sect->peek = false;
                            $sect->partial = $partial;
                            $$ = (ie_fetch_attr_t){ .extra = $sect }; }
          | BODY_PEEK '[' sect ']' partial
                          { $sect->peek = true;
                            $sect->partial = $partial;
                            $$ = (ie_fetch_attr_t){ .extra = $sect }; }
;

sect: %empty                       { SECT($$, NULL, (ie_sect_txt_t){0}); }
    | sect_msgtxt[t]               { SECT($$, NULL, $t); }
    | sect_part[p]                 { SECT($$, $p, (ie_sect_txt_t){0}); }
    | sect_part[p] '.' sect_txt[t] { SECT($$, $p, $t); }
;

sect_part: num                  { SECT_PART($$, NULL, $num); }
         | sect_part[s] '.' num { SECT_PART($$, $s, $num); }
;

sect_txt: sect_msgtxt
        | MIME              { $$ = (ie_sect_txt_t){ .type = IE_SECT_MIME }; }
;

sect_msgtxt: HEADER         { $$ = (ie_sect_txt_t){ .type = IE_SECT_HEADER }; }
           | TEXT           { $$ = (ie_sect_txt_t){ .type = IE_SECT_TEXT }; }
           | hdr_flds       { MODE(FETCH); $$ = $hdr_flds; }
           | hdr_flds_not   { MODE(FETCH); $$ = $hdr_flds_not; }
;

hdr_flds: HDR_FLDS SP { MODE(ASTRING); } '(' header_list_1[h] ')'
          { $$ = (ie_sect_txt_t){ .type = IE_SECT_HDR_FLDS,
                                  .headers = $h }; };

hdr_flds_not: HDR_FLDS_NOT SP { MODE(ASTRING); } '(' header_list_1[h] ')'
              { $$ = (ie_sect_txt_t){ .type = IE_SECT_HDR_FLDS_NOT,
                                      .headers = $h }; };

header_list_1: keep_astring[h]                   { HEADER($$, NULL, $h); }
             | header_list_1[l] SP keep_astring[h] { HEADER($$, $l, $h); }
;

partial: %empty                     { $$ = (ie_partial_t){0}; }
       | '<' num[a] '.' num[b] '>'  { $$.found = 1; $$.p1 = $a; $$.p2 = $b; }
;

/*** STORE command ***/

store_cmd: tag SP uid_mode[u] STORE SP seq_set[seq]
           { MODE(STORE); } SP store_sign[sign] FLAGS store_silent[silent] SP
           { MODE(FLAG); } store_flags[f]
           { STORE_CMD($tag, $u, $seq, $sign, $silent, $f); };

store_sign: %empty  { $$ = 0; }
          | '-'     { $$ = -1; }
          | '+'     { $$ = 1; }
;

store_silent: %empty    { $$ = false; }
            | SILENT    { $$ = true; }
;

store_flags: '(' store_flags_0[f] ')'   { $$ = $f; }
           | store_flags_1
;

store_flags_0: %empty           { $$ = (ie_flag_list_t){0}; }
             | store_flags_1
;

store_flags_1: keep_flag[f]      { ADD_FLAG($$, (ie_flag_list_t){0}, $f); }
             | store_flags_1[l] SP keep_flag[f]   { ADD_FLAG($$, $l, $f); }
;

/*** COPY command ***/

copy_cmd: tag SP uid_mode[u] COPY SP seq_set[set] SP keep_mailbox[mbx]
          { COPY_CMD($tag, $u, $set, $mbx); };

/*** status-type handling.  Thanks for the the shitty grammar, IMAP4rev1 ***/

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

sc_uidnext: UIDNEXT { MODE(NUM); };
sc_uidvld: UIDVLD   { MODE(NUM); };
sc_unseen: UNSEEN   { MODE(NUM); };

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
   be able to trigger CAPA_RESP_END via the %destructor, therefore capa_start
   has a semantic value we need to explicitly ignore to avoid warnings */
capa_resp: capa_start capa_list { CAPA_RESP_END(true); (void)$1; };

capa_start: %empty { CAPA_RESP_START; MODE(ATOM); $$ = NULL; };

capa_list: keep_atom               { CAPA_RESP($keep_atom); }
         | capa_list SP keep_atom  { CAPA_RESP($keep_atom); }
;

/*** PERMANENTFLAG handling ***/
/* %destructor is used to guarantee RESP_END gets called, as with CAPABILITY */
pflag_resp: { MODE(FLAG); } '(' pflags_0[f] ')' { PFLAG_RESP($f); };

pflags_0: %empty    { $$ = (ie_flag_list_t){0}; }
        | pflags_1
;

pflags_1: keep_pflag[f]              { ADD_FLAG($$, (ie_flag_list_t){0}, $f); }
        | pflags_1[l] SP keep_pflag[f]                { ADD_FLAG($$, $l, $f); }
;

/*** LIST/LSUB responses ***/
list_resp: { MODE(FLAG); } '(' mflags_0[f] ')' SP nqchar SP keep_mailbox[m]
           { LIST_RESP($f, $nqchar, $m); };

lsub_resp: { MODE(FLAG); } '(' mflags_0[f] ')' SP nqchar SP keep_mailbox[m]
           { LSUB_RESP($f, $nqchar, $m); };

mflags_0: %empty    { $$ = (ie_flag_list_t){0}; }
        | mflags_1
;

mflags_1: keep_mflag[f]              { ADD_FLAG($$, (ie_flag_list_t){0}, $f); }
        | mflags_1[l] SP keep_mflag[f]                { ADD_FLAG($$, $l, $f); }
;

/*** STATUS responses ***/
status_resp: keep_mailbox[m]
             { MODE(ST_ATTR); } SP '(' st_attr_rlist_0[sa] ')'
             { STATUS_RESP($m, $sa); };

st_attr_rlist_0: %empty          { $$ = (ie_st_attr_resp_t){0}; }
              | st_attr_rlist_1
;

st_attr_rlist_1: st_attr[s] SP num[n]
                    { $$ = add_resp_st_attr(NULL, $s, $n); }
               | st_attr_rlist_1[old] SP st_attr[s] SP num[n]
                    { $$ = add_resp_st_attr(&$old, $s, $n); }
;

st_attr: MESSAGES    { $$ = IE_ST_ATTR_MESSAGES; }
       | RECENT      { $$ = IE_ST_ATTR_RECENT; }
       | UIDNEXT     { $$ = IE_ST_ATTR_UIDNEXT; }
       | UIDVLD      { $$ = IE_ST_ATTR_UIDVLD; }
       | UNSEEN      { $$ = IE_ST_ATTR_UNSEEN; }
;

/*** FLAGS responses ***/
flags_resp: { MODE(FLAG); } '(' flags_0[f] ')' { FLAGS_RESP($f); };

flags_0: %empty    { $$ = (ie_flag_list_t){0}; }
       | flags_1
;

flags_1: keep_flag[f]                { ADD_FLAG($$, (ie_flag_list_t){0}, $f); }
       | flags_1[l] SP keep_flag[f]  { ADD_FLAG($$, $l, $f); }
;

/*** EXISTS responses ***/
exists_resp: num SP EXISTS { EXISTS_RESP($num); };

/*** RECENT responses ***/
recent_resp: num SP RECENT { RECENT_RESP($num); };

/*** EXPUNGE responses ***/
expunge_resp: num SP EXPUNGE { EXPUNGE_RESP($num); };

/*** FETCH responses ***/

fetch_resp: pre_fetch_resp SP '(' msg_attrs_0 ')'
            { FETCH_RESP_END(true); (void)$1; }
;

pre_fetch_resp: num SP FETCH
                { FETCH_RESP_START($num); $$ = NULL; MODE(FETCH); };

msg_attrs_0: %empty
           | msg_attrs_1
;

msg_attrs_1: msg_attr
           | msg_attrs_1 SP msg_attr
;

/* most of these get ignored completely, we only really need:
     - FLAGS,
     - UID,
     - INTERNALDATE,
     - the fully body text
   Anything else is going to be encrypted anyway. */
msg_attr: msg_attr_ { MODE(FETCH); };

msg_attr_: f_flags_resp
         | f_uid_resp
         | f_intdate_resp
         | f_rfc822_resp
         | ENVELOPE SP '(' envelope ')'
         | RFC822_TEXT SP nstring
         | RFC822_HEADER SP nstring
         | RFC822_SIZE SP NUM
         /* don't even parse anything that starts with BODY */
;

/*** FETCH FLAGS ***/
f_flags_resp: FLAGS SP { MODE(FLAG); } '(' f_flags_0[f] ')'
              { F_FLAGS_RESP($f); };

f_flags_0: %empty   { $$ = (ie_flag_list_t){0}; }
         | f_flags_1
;

f_flags_1: keep_fflag[f]             { ADD_FLAG($$, (ie_flag_list_t){0}, $f); }
         | f_flags_1[l] SP keep_fflag[f]              { ADD_FLAG($$, $l, $f); }
;

/*** FETCH RFC822 ***/
f_rfc822_resp: pre_f_rfc822_resp rfc822_nstring
               { F_RFC822_RESP_END(true); (void)$1; }

pre_f_rfc822_resp: RFC822 SP { F_RFC822_RESP_START; $$ = NULL; MODE(NSTRING); }

rfc822_nstring: NIL
              | LITERAL { F_RFC822_RESP_LITERAL; } LITERAL_END
              | '"' { START_QSTR; } rfc822_qstr_body '"' { END_QSTR; }
;

rfc822_qstr_body: RAW                   { F_RFC822_RESP_QSTR; }
                | rfc822_qstr_body RAW  { F_RFC822_RESP_QSTR; }
;

/*** FETCH UID ***/
f_uid_resp: UID SP { MODE(NUM); } num { F_UID_RESP($num); };

/*** FETCH INTERNALDATE ***/
f_intdate_resp: INTDATE SP date_time { F_INTDATE_RESP($date_time); };


/*        date    subj    from       sender     reply-to */
envelope: nstring nstring naddr_list naddr_list nstring
/*        to         cc         bcc        in-reply-to message-id */
          naddr_list naddr_list naddr_list naddr_list  nstring
;

naddr_list: NIL
          | '(' addr_list ')'
;

addr_list: %empty
         | addr_list address
;

/*           addr-name addr-adl addr-mailbox addr-host */
address: '(' nstring   nstring  nstring      nstring   ')'
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
atom: RAW             { KEEP_INIT; KEEP(RAW); }
    | atom atom_like  { KEEP(RAW); }
;

atom_like: RAW
         | NUM
         | keyword
;

string: qstring
      | literal
;

qstring: '"' preqstring qstring_body '"' { END_QSTR; (void)$preqstring; };

preqstring: %empty { START_QSTR; KEEP_INIT; $$ = NULL; }

qstring_body: %empty
            | qstring_body atom_like    { KEEP(QSTRING); }
;

/* note that LITERAL_END is passed by the application after it finishes reading
   the literal from the stream; it is never returned by the scanner */
literal: LITERAL { LITERAL_RESP; } LITERAL_END;

/* nqchar can't handle spaces, so an post-nqchar MODE() call is required */
nqchar: prenqchar NIL                 { $$ = 0; MODE(MAILBOX); }
      | prenqchar '"' keep_qchar '"'  { $$ = $keep_qchar; MODE(MAILBOX); }
;

prenqchar: %empty { MODE(NQCHAR); };

keep_qchar: QCHAR   { $$ = qchar_to_char; };

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

mailbox: prembx astring        { $$ = false; /* not an INBOX */ }
       | prembx INBOX          { $$ = true;  /* is an INBOX */ }
;

prembx: %empty { MODE(MAILBOX); };

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

seq_set: preseq seq_spec              { $$ = $seq_spec; }
       | seq_set[s1] ',' seq_spec[s2] { $$ = $s2; $s2->next = $s1; }
;

preseq: %empty { MODE(SEQSET); };

seq_spec: seq_num[n]                  { SEQ_SPEC($$, $n, $n); }
        | seq_num[n1] ':' seq_num[n2] { SEQ_SPEC($$, $n1, $n2); }
;

seq_num: '*'    { $$ = 0; }
       | num
;

/* dummy grammar to make sure KEEP_CANCEL gets called in error handling */
prekeep: %empty { KEEP_START; $$ = NULL; };

/* the "keep" variations of the above (except tag, which is always kept) */
keep_atom: prekeep atom { $$ = KEEP_REF($prekeep); };
keep_astring: prekeep astring { $$ = KEEP_REF($prekeep); }
keep_flag: prekeep flag { $$ = (ie_flag_t){$flag, KEEP_REF($prekeep)}; };
keep_fflag: prekeep fflag { $$ = (ie_flag_t){$fflag, KEEP_REF($prekeep)}; };
keep_pflag: prekeep pflag { $$ = (ie_flag_t){$pflag, KEEP_REF($prekeep)}; };
keep_mflag: prekeep mflag { $$ = (ie_flag_t){$mflag, KEEP_REF($prekeep)}; };
keep_mailbox: prekeep mailbox { $$ = (ie_mailbox_t){$mailbox, KEEP_REF($prekeep)}; };

SP: ' ';
