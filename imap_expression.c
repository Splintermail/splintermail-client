#include <stdlib.h>

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

void dstr_link_free(dstr_link_t *h){
    dstr_link_t *ptr, *next;
    ptr = h;
    while(ptr){
        next = ptr->next;
        dstr_free(&ptr->dstr);
        free(ptr);
        ptr = next;
    }
}

void ie_flag_list_free(ie_flag_list_t* fl){
    dstr_link_free(fl->keywords);
    dstr_link_free(fl->extensions);
}

void ie_seq_set_free(ie_seq_set_t *s){
    ie_seq_set_t *ptr, *next;
    ptr = s;
    while(ptr){
        next = ptr->next;
        free(ptr);
        ptr = next;
    }
}

void ie_search_key_free(ie_search_key_t *key){
    ie_search_key_t *ptr, *next;
    ptr = key;
    while(ptr){
        next = ptr->next;
        switch(ptr->type){
            case IE_SEARCH_ALL:         //
            case IE_SEARCH_ANSWERED:    //
            case IE_SEARCH_DELETED:     //
            case IE_SEARCH_FLAGGED:     //
            case IE_SEARCH_NEW:         //
            case IE_SEARCH_OLD:         //
            case IE_SEARCH_RECENT:      //
            case IE_SEARCH_SEEN:        //
            case IE_SEARCH_SUBJECT:     //
            case IE_SEARCH_UNANSWERED:  //
            case IE_SEARCH_UNDELETED:   //
            case IE_SEARCH_UNFLAGGED:   //
            case IE_SEARCH_UNSEEN:      //
            case IE_SEARCH_DRAFT:       //
            case IE_SEARCH_UNDRAFT:     //
            case IE_SEARCH_BEFORE:      // uses search_param.date
            case IE_SEARCH_ON:          // uses search_param.date
            case IE_SEARCH_SINCE:       // uses search_param.date
            case IE_SEARCH_SENTBEFORE:  // uses search_param.date
            case IE_SEARCH_SENTON:      // uses search_param.date
            case IE_SEARCH_SENTSINCE:   // uses search_param.date
            case IE_SEARCH_LARGER:      // uses search_param.num
            case IE_SEARCH_SMALLER:     // uses search_param.num
                break;
            case IE_SEARCH_BCC:         // uses search_param.dstr
            case IE_SEARCH_BODY:        // uses search_param.dstr
            case IE_SEARCH_CC:          // uses search_param.dstr
            case IE_SEARCH_FROM:        // uses search_param.dstr
            case IE_SEARCH_KEYWORD:     // uses search_param.dstr
            case IE_SEARCH_TEXT:        // uses search_param.dstr
            case IE_SEARCH_TO:          // uses search_param.dstr
            case IE_SEARCH_UNKEYWORD:   // uses search_param.dstr
                dstr_free(&ptr->param.dstr);
                break;
            case IE_SEARCH_HEADER:      // uses search_param.header
                dstr_free(&ptr->param.header.name);
                dstr_free(&ptr->param.header.value);
                break;
            case IE_SEARCH_UID:         // uses search_param.seq_set
            case IE_SEARCH_SEQ_SET:     // uses search_param.seq_set
                ie_seq_set_free(ptr->param.seq_set);
                break;
            case IE_SEARCH_NOT:         // uses search_param.search_key
            case IE_SEARCH_GROUP:       // uses search_param.search_key
                ie_search_key_free(ptr->param.search_key);
                break;
            case IE_SEARCH_OR:          // uses search_param.search_or
                ie_search_key_free(ptr->param.search_or.a);
                ie_search_key_free(ptr->param.search_or.b);
                break;
        }
        free(ptr);
        ptr = next;
    }
}

void ie_section_part_free(ie_section_part_t *s){
    ie_section_part_t *ptr, *next;
    ptr = s;
    while(ptr){
        next = ptr->next;
        free(ptr);
        ptr = next;
    }
}

void ie_fetch_extra_free(ie_fetch_extra_t *extra){
    ie_fetch_extra_t *ptr, *next;
    ptr = extra;
    while(ptr){
        next = ptr->next;
        ie_section_part_free(ptr->sect_part);
        dstr_link_free(ptr->sect_txt.headers);
        free(ptr);
        ptr = next;
    }
}

void ie_fetch_attr_free(ie_fetch_attr_t *attr){
    ie_fetch_extra_free(attr->extra);
}
