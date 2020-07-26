#include "libimaildir.h"

// args which are constant through the whole recursion
typedef struct {
    const msg_view_t *view;
    unsigned int seq;
    unsigned int seq_max;
    unsigned int uid_dn_max;
} search_args_t;

static bool get_recent(const msg_view_t *view){
    // TODO: support \Recent flag
    LOG_ERROR("\\Recent flag not supported!\n");
    return view->recent;
}

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


static derr_t do_eval(const search_args_t *args, size_t lvl,
        const ie_search_key_t *key, bool *out){
    derr_t e = E_OK;

    // recursion limit
    if(lvl > 1000){
        *out = false;
        return e;
    }

    const msg_view_t *view = args->view;
    union ie_search_param_t param = key->param;

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
            *out = get_recent(view) && !view->flags.seen;
            break;

        case IE_SEARCH_OLD:
            *out = !get_recent(view);
            break;

        case IE_SEARCH_RECENT:
            *out = get_recent(view);
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
            *out = in_seq_set(
                view->uid_dn, param.seq_set, args->uid_dn_max
            );
            break;

        case IE_SEARCH_SEQ_SET:     // uses param.seq_set
            *out = in_seq_set(args->seq, param.seq_set,args-> seq_max);
            break;

        case IE_SEARCH_SUBJECT:     // uses param.dstr
        case IE_SEARCH_BCC:         // uses param.dstr
        case IE_SEARCH_BODY:        // uses param.dstr
        case IE_SEARCH_CC:          // uses param.dstr
        case IE_SEARCH_FROM:        // uses param.dstr
        case IE_SEARCH_KEYWORD:     // uses param.dstr
        case IE_SEARCH_TEXT:        // uses param.dstr
        case IE_SEARCH_TO:          // uses param.dstr
        case IE_SEARCH_UNKEYWORD:   // uses param.dstr
        case IE_SEARCH_HEADER:      // uses param.header
        case IE_SEARCH_BEFORE:      // uses param.date
        case IE_SEARCH_ON:          // uses param.date
        case IE_SEARCH_SINCE:       // uses param.date
        case IE_SEARCH_SENTBEFORE:  // uses param.date
        case IE_SEARCH_SENTON:      // uses param.date
        case IE_SEARCH_SENTSINCE:   // uses param.date
        case IE_SEARCH_LARGER:      // uses param.num
        case IE_SEARCH_SMALLER:     // uses param.num

        case IE_SEARCH_MODSEQ:      // uses param.modseq
            ORIG(&e, E_INTERNAL, "not implemented");
    }

    return e;
}

derr_t search_key_eval(const ie_search_key_t *key, const msg_view_t *view,
        unsigned int seq, unsigned int seq_max, unsigned int uid_dn_max,
        bool *out){
    derr_t e = E_OK;

    search_args_t args = {
        .view = view,
        .seq = seq,
        .seq_max = seq_max,
        .uid_dn_max = uid_dn_max,
    };
    PROP(&e, do_eval(&args, 0, key, out) );

    return e;
}
