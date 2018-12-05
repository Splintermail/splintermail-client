#include "imap_expression.h"

DSTR_STATIC(status_code_NONE_dstr, "STATUS_CODE_NONE");
DSTR_STATIC(status_code_ALERT_dstr, "STATUS_CODE_ALERT");
DSTR_STATIC(status_code_CAPA_dstr, "STATUS_CODE_CAPA");
DSTR_STATIC(status_code_PARSE_dstr, "STATUS_CODE_PARSE");
DSTR_STATIC(status_code_PERMFLAGS_dstr, "STATUS_CODE_PERMFLAGS");
DSTR_STATIC(status_code_READ_ONLY_dstr, "STATUS_CODE_READ_ONLY");
DSTR_STATIC(status_code_READ_WRITE_dstr, "STATUS_CODE_READ_WRITE");
DSTR_STATIC(status_code_TRYCREATE_dstr, "STATUS_CODE_TRYCREATE");
DSTR_STATIC(status_code_UIDNEXT_dstr, "STATUS_CODE_UIDNEXT");
DSTR_STATIC(status_code_UIDVLD_dstr, "STATUS_CODE_UIDVLD");
DSTR_STATIC(status_code_UNSEEN_dstr, "STATUS_CODE_UNSEEN");
DSTR_STATIC(status_code_ATOM_dstr, "STATUS_CODE_ATOM");
DSTR_STATIC(status_code_UNK_dstr, "unknown status code");

const dstr_t *st_code_to_dstr(status_code_t code){
    switch(code){
    case STATUS_CODE_NONE: return &status_code_NONE_dstr;
    case STATUS_CODE_ALERT: return &status_code_ALERT_dstr;
    case STATUS_CODE_CAPA: return &status_code_CAPA_dstr;
    case STATUS_CODE_PARSE: return &status_code_PARSE_dstr;
    case STATUS_CODE_PERMFLAGS: return &status_code_PERMFLAGS_dstr;
    case STATUS_CODE_READ_ONLY: return &status_code_READ_ONLY_dstr;
    case STATUS_CODE_READ_WRITE: return &status_code_READ_WRITE_dstr;
    case STATUS_CODE_TRYCREATE: return &status_code_TRYCREATE_dstr;
    case STATUS_CODE_UIDNEXT: return &status_code_UIDNEXT_dstr;
    case STATUS_CODE_UIDVLD: return &status_code_UIDVLD_dstr;
    case STATUS_CODE_UNSEEN: return &status_code_UNSEEN_dstr;
    case STATUS_CODE_ATOM: return &status_code_ATOM_dstr;
    default: return &status_code_UNK_dstr;
    }
};

DSTR_STATIC(flag_type_ANSWERED_dstr, "FLAG_ANSWERED");
DSTR_STATIC(flag_type_FLAGGED_dstr, "FLAG_FLAGGED");
DSTR_STATIC(flag_type_DELETED_dstr, "FLAG_DELETED");
DSTR_STATIC(flag_type_SEEN_dstr, "FLAG_SEEN");
DSTR_STATIC(flag_type_DRAFT_dstr, "FLAG_DRAFT");
DSTR_STATIC(flag_type_RECENT_dstr, "FLAG_RECENT");
DSTR_STATIC(flag_type_NOSELECT_dstr, "FLAG_NOSELECT");
DSTR_STATIC(flag_type_MARKED_dstr, "FLAG_MARKED");
DSTR_STATIC(flag_type_UNMARKED_dstr, "FLAG_UNMARKED");
DSTR_STATIC(flag_type_ASTERISK_dstr, "FLAG_ASTERISK");
DSTR_STATIC(flag_type_KEYWORD_dstr, "FLAG_KEYWORD");
DSTR_STATIC(flag_type_EXTENSION_dstr, "FLAG_EXTENSION");
DSTR_STATIC(flag_type_UNK_dstr, "UNKNOWN FLAG");

const dstr_t *flag_type_to_dstr(ie_flag_type_t f){
    switch(f){
        case IE_FLAG_ANSWERED: return &flag_type_ANSWERED_dstr;
        case IE_FLAG_FLAGGED: return &flag_type_FLAGGED_dstr;
        case IE_FLAG_DELETED: return &flag_type_DELETED_dstr;
        case IE_FLAG_SEEN: return &flag_type_SEEN_dstr;
        case IE_FLAG_DRAFT: return &flag_type_DRAFT_dstr;
        case IE_FLAG_RECENT: return &flag_type_RECENT_dstr;
        case IE_FLAG_NOSELECT: return &flag_type_NOSELECT_dstr;
        case IE_FLAG_MARKED: return &flag_type_MARKED_dstr;
        case IE_FLAG_UNMARKED: return &flag_type_UNMARKED_dstr;
        case IE_FLAG_ASTERISK: return &flag_type_ASTERISK_dstr;
        case IE_FLAG_KEYWORD: return &flag_type_KEYWORD_dstr;
        case IE_FLAG_EXTENSION: return &flag_type_EXTENSION_dstr;
        default: return &flag_type_UNK_dstr;
    }
}

DSTR_STATIC(status_attr_MESSAGES_dstr, "ATTR_MESSAGES");
DSTR_STATIC(status_attr_RECENT_dstr, "ATTR_RECENT");
DSTR_STATIC(status_attr_UIDNEXT_dstr, "ATTR_UIDNEXT");
DSTR_STATIC(status_attr_UIDVLD_dstr, "ATTR_UIDVLD");
DSTR_STATIC(status_attr_UNSEEN_dstr, "ATTR_UNSEEN");
DSTR_STATIC(status_attr_UNK_dstr, "Unknown attribute");

const dstr_t *st_attr_to_dstr(ie_st_attr_t attr){
    switch(attr){
        case IE_ST_ATTR_MESSAGES: return &status_attr_MESSAGES_dstr;
        case IE_ST_ATTR_RECENT: return &status_attr_RECENT_dstr;
        case IE_ST_ATTR_UIDNEXT: return &status_attr_UIDNEXT_dstr;
        case IE_ST_ATTR_UIDVLD: return &status_attr_UIDVLD_dstr;
        case IE_ST_ATTR_UNSEEN: return &status_attr_UNSEEN_dstr;
        default: return &status_attr_UNK_dstr;
    }
}
