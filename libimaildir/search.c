#include "libimaildir.h"

// args which are constant through the whole recursion
typedef struct {
    const msg_view_t *view;
    unsigned int seq;
    unsigned int seq_max;
    unsigned int uid_dn_max;
    // get a read-only copy of either headers or whole body, must be idempotent
    derr_t (*get_hdrs)(void*, const imf_hdrs_t**);
    void *get_hdrs_data;
    derr_t (*get_imf)(void*, const imf_t**);
    void *get_imf_data;
} search_args_t;


static bool in_seq_set(unsigned int val, const ie_seq_set_t *seq_set,
        unsigned int max){
    for(; seq_set != NULL; seq_set = seq_set->next){
        // straighten out the range
        unsigned int n1 = seq_set->n1 ? seq_set->n1 : max;
        unsigned int n2 = seq_set->n2 ? seq_set->n2 : max;
        unsigned int a = MIN(n1, n2);
        unsigned int b = MAX(n1, n2);
        if(val >= a && val <= b){
            return true;
        }
    }
    return false;
}


// look up the value of a header named `name`
static derr_t find_header(
    search_args_t *args, const dstr_t name, dstr_t *out
){
    derr_t e = E_OK;
    *out = (dstr_t){0};

    const imf_hdrs_t *hdrs;
    IF_PROP(&e, args->get_hdrs(args->get_hdrs_data, &hdrs) ){
        TRACE(&e, "failed to parse message for header SEARCH\n");
        DUMP(e);
        DROP_VAR(&e);
        return e;
    }

    // find a matching header field
    for(const imf_hdr_t *hdr = hdrs->hdr; hdr; hdr = hdr->next){
        // does this header name match?
        dstr_t hname = dstr_from_off(hdr->name);
        if(dstr_icmp2(hname, name) == 0){
            // return the content of the header value
            *out = dstr_from_off(hdr->value);
            return e;
        }
    }

    return e;
}


// find a header matching `name`, return if its value contains `value`
static derr_t search_headers(
    search_args_t *args, const dstr_t name, const dstr_t value, bool *out
){
    derr_t e = E_OK;
    *out = false;

    dstr_t hvalue;
    PROP(&e, find_header(args, name, &hvalue) );
    if(!hvalue.data) return e;

    if(dstr_icount2(hvalue, value) > 0) *out = true;

    return e;
}

static derr_t parse_date(search_args_t *args, imap_time_t *out){
    derr_t e = E_OK;
    *out = (imap_time_t){0};

    dstr_t hvalue;
    PROP(&e, find_header(args, DSTR_LIT("Date"), &hvalue) );
    if(!hvalue.data) return e;

    IF_PROP(&e, imf_parse_date(hvalue, out) ){
        TRACE(&e, "failed to parse Date header in SEARCH\n");
        DUMP(e);
        DROP_VAR(&e);
        return e;
    }

    return e;
}

// ON = date matches, disregarding time and timezone
bool date_a_is_on_b(imap_time_t a, imap_time_t b){
    return a.year == b.year && a.month == b.month && a.day == b.day;
}

// BEFORE = date is earlier, disregarding time and timezone
bool date_a_is_before_b(imap_time_t a, imap_time_t b){
    if(a.year < b.year) return true;
    if(a.year > b.year) return false;
    // a.year == b.year
    if(a.month < b.month) return true;
    if(a.month > b.month) return false;
    // a.month == b.month
    return a.day < b.day;
}

// SINCE = date is on or after, disregarding time and timezone
bool date_a_is_since_b(imap_time_t a, imap_time_t b){
    if(a.year > b.year) return true;
    if(a.year < b.year) return false;
    // a.year == b.year
    if(a.month > b.month) return true;
    if(a.month < b.month) return false;
    // a.month == b.month
    return a.day >= b.day;
}


static derr_t do_eval(search_args_t *args, size_t lvl,
        const ie_search_key_t *key, bool *out){
    derr_t e = E_OK;

    // recursion limit
    if(lvl > 1000){
        *out = false;
        return e;
    }

    const msg_view_t *view = args->view;
    union ie_search_param_t param = key->param;
    imap_time_t date;

    switch(key->type){
        case IE_SEARCH_ALL: *out = true; break;

        case IE_SEARCH_ANSWERED:    *out =  view->flags.answered;  break;
        case IE_SEARCH_UNANSWERED:  *out = !view->flags.answered;  break;
        case IE_SEARCH_DELETED:     *out =  view->flags.deleted;   break;
        case IE_SEARCH_UNDELETED:   *out = !view->flags.deleted;   break;
        case IE_SEARCH_FLAGGED:     *out =  view->flags.flagged;   break;
        case IE_SEARCH_UNFLAGGED:   *out = !view->flags.flagged;   break;
        case IE_SEARCH_SEEN:        *out =  view->flags.seen;      break;
        case IE_SEARCH_UNSEEN:      *out = !view->flags.seen;      break;
        case IE_SEARCH_DRAFT:       *out =  view->flags.draft;     break;
        case IE_SEARCH_UNDRAFT:     *out = !view->flags.draft;     break;

        case IE_SEARCH_NEW:
            // "NEW" means "recent and not seen", and we don't support recent
            *out = false;
            break;

        case IE_SEARCH_OLD:
            // "OLD" means "not recent", and we don't support recent
            *out = true;
            break;

        case IE_SEARCH_RECENT:
            // we don't support recent
            *out = false;
            break;

        case IE_SEARCH_NOT: {
            bool temp;
            PROP(&e, do_eval(args, lvl+1, param.key, &temp) );
            *out = !temp;
            } break;

        case IE_SEARCH_GROUP:
            // exists only to express parens from the grammar
            PROP(&e, do_eval(args, lvl+1, param.key, out) );
            break;

        case IE_SEARCH_OR: {
            bool a, b;
            PROP(&e, do_eval(args, lvl+1, param.pair.a, &a) );
            PROP(&e, do_eval(args, lvl+1, param.pair.b, &b) );
            *out = a || b;
            } break;

        case IE_SEARCH_AND: {
            bool a, b;
            PROP(&e, do_eval(args, lvl+1, param.pair.a, &a) );
            PROP(&e, do_eval(args, lvl+1, param.pair.b, &b) );
            *out = a && b;
            } break;

        case IE_SEARCH_UID:
            *out = in_seq_set(view->uid_dn, param.seq_set, args->uid_dn_max);
            break;

        case IE_SEARCH_SEQ_SET:     // uses param.seq_set
            *out = in_seq_set(args->seq, param.seq_set, args->seq_max);
            break;

        #define HEADER_SEARCH(hdrname) \
            PROP(&e, \
                search_headers(args, DSTR_LIT(hdrname), param.dstr->dstr, out \
                ) \
            )
        // use param.dstr
        case IE_SEARCH_SUBJECT: HEADER_SEARCH("Subject"); break;
        case IE_SEARCH_BCC: HEADER_SEARCH("Bcc"); break;
        case IE_SEARCH_CC: HEADER_SEARCH("Cc"); break;
        case IE_SEARCH_FROM: HEADER_SEARCH("From"); break;
        case IE_SEARCH_TO: HEADER_SEARCH("To"); break;
        #undef HEADER_SEARCH

        case IE_SEARCH_BODY: {      // uses param.dstr
                // search just the body
                const imf_t *imf;
                IF_PROP(&e, args->get_imf(args->get_imf_data, &imf) ){
                    TRACE(&e, "failed to parse message for SEARCH BODY\n");
                    DUMP(e);
                    DROP_VAR(&e);
                    *out = false;
                    break;
                }

                dstr_t searchable = dstr_from_off(imf->body);
                *out = dstr_icount2(searchable, param.dstr->dstr) > 0;
            } break;

        case IE_SEARCH_TEXT: {      // uses param.dstr
                const dstr_t tgt = param.dstr->dstr;
                // search the headers first, then search the body if necessary
                const imf_hdrs_t *hdrs;
                IF_PROP(&e, args->get_hdrs(args->get_hdrs_data, &hdrs) ){
                    TRACE(&e, "failed to parse message for SEARCH TEXT\n");
                    DUMP(e);
                    DROP_VAR(&e);
                    *out = false;
                    break;
                }
                dstr_t searchable = dstr_from_off(hdrs->bytes);
                if(dstr_icount2(searchable, tgt) > 0){
                    *out = true;
                    break;
                }

                // remember how far we already searched
                size_t hdrs_end = hdrs->bytes.start + hdrs->bytes.len;

                const imf_t *imf;
                IF_PROP(&e, args->get_imf(args->get_imf_data, &imf) ){
                    TRACE(&e, "failed to parse message for SEARCH TEXT\n");
                    DUMP(e);
                    DROP_VAR(&e);
                    *out = false;
                    break;
                }
                /* avoid searching things we already searched, allowing for
                   boundary conditions */
                size_t start = hdrs_end - MIN(hdrs_end, tgt.len);
                size_t end = imf->bytes.start + imf->bytes.len;
                searchable = dstr_sub2(dstr_from_off(imf->bytes), start, end);

                *out = dstr_icount2(searchable, param.dstr->dstr) > 0;
            } break;

        case IE_SEARCH_KEYWORD:     // uses param.dstr
            // we don't support keyword flags
            *out = false;
            break;

        case IE_SEARCH_UNKEYWORD:   // uses param.dstr
            // we don't support keyword flags
            *out = true;
            break;

        case IE_SEARCH_HEADER:      // uses param.header
            PROP(&e,
                search_headers(
                    args,
                    param.header.name->dstr,
                    param.header.value->dstr,
                    out
                )
            );
            break;

        case IE_SEARCH_BEFORE:      // uses param.date
            *out = date_a_is_before_b(args->view->internaldate, param.date);
            break;

        case IE_SEARCH_ON:          // uses param.date
            *out = date_a_is_on_b(args->view->internaldate, param.date);
            break;

        case IE_SEARCH_SINCE:       // uses param.date
            *out = date_a_is_since_b(args->view->internaldate, param.date);
            break;

        case IE_SEARCH_SENTBEFORE:  // uses param.date
            PROP(&e, parse_date(args, &date) );
            *out = date.year != 0 && date_a_is_before_b(date, param.date);
            break;

        case IE_SEARCH_SENTON:      // uses param.date
            PROP(&e, parse_date(args, &date) );
            *out = date.year != 0 && date_a_is_on_b(date, param.date);
            break;

        case IE_SEARCH_SENTSINCE:   // uses param.date
            PROP(&e, parse_date(args, &date) );
            *out = date.year != 0 && date_a_is_since_b(date, param.date);
            break;

        case IE_SEARCH_LARGER:      // uses param.num
            *out = args->view->length > param.num;
            break;

        case IE_SEARCH_SMALLER:     // uses param.num
            *out = args->view->length < param.num;
            break;

        case IE_SEARCH_MODSEQ:      // uses param.modseq
            ORIG(&e, E_INTERNAL, "not implemented");
    }

    return e;
}

derr_t search_key_eval(
    const ie_search_key_t *key,
    const msg_view_t *view,
    unsigned int seq,
    unsigned int seq_max,
    unsigned int uid_dn_max,
    // get a read-only copy of either headers or whole body, must be idempotent
    derr_t (*get_hdrs)(void*, const imf_hdrs_t**),
    void *get_hdrs_data,
    derr_t (*get_imf)(void*, const imf_t**),
    void *get_imf_data,
    bool *out
){
    derr_t e = E_OK;

    search_args_t args = {
        .view = view,
        .seq = seq,
        .seq_max = seq_max,
        .uid_dn_max = uid_dn_max,
        // message is parsed lazily
        .get_hdrs = get_hdrs,
        .get_hdrs_data = get_hdrs_data,
        .get_imf = get_imf,
        .get_imf_data = get_imf_data,
    };
    PROP(&e, do_eval(&args, 0, key, out) );

    return e;
}
