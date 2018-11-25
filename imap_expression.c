#include "imap_expression.h"

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
