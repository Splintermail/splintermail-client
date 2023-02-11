#include "tools/qwerrewq/libqw.h"

// singletons
qw_val_t thetrue = QW_VAL_TRUE;
qw_val_t thefalse = QW_VAL_FALSE;
qw_val_t thenull = QW_VAL_NULL;
qw_val_t theskip = QW_VAL_SKIP;

const char *qw_val_name(qw_val_t val){
    switch(val){
        case QW_VAL_FALSE: return "FALSE";
        case QW_VAL_TRUE: return "TRUE";
        case QW_VAL_NULL: return "NULL";
        case QW_VAL_SKIP: return "SKIP";
        case QW_VAL_STRING: return "STRING";
        case QW_VAL_LIST: return "LIST";
        case QW_VAL_LAZY: return "LAZY";
        case QW_VAL_DICT: return "DICT";
        case QW_VAL_FUNC: return "FUNC";
    }
    return NULL;
}

const void *jsw_get_qw_key(const jsw_anode_t *node){
    qw_key_t *key = CONTAINER_OF(node, qw_key_t, node);
    return &key->text;
}

void qw_keymap_add_key(qw_engine_t *engine, jsw_atree_t *keymap, dstr_t text){
    size_t index = jsw_asize(keymap);
    qw_key_t *key = qw_engine_malloc(engine, sizeof(qw_key_t), PTRSIZE);
    *key = (qw_key_t){ .text = text, .index = index };
    jsw_ainsert(keymap, &key->node);
}

qw_val_t *qw_dict_get(qw_engine_t *engine, qw_dict_t *dict, dstr_t key){
    jsw_anode_t *node = jsw_afind(dict->keymap, &key, NULL);
    if(!node) return NULL;
    qw_key_t *k = CONTAINER_OF(node, qw_key_t, node);
    qw_val_t *val = dict->vals[k->index];
    // evaluate and cache any lazy values
    if(*val == QW_VAL_LAZY){
        qw_lazy_t *lazy = CONTAINER_OF(val, qw_lazy_t, type);
        // execute the contained instructions
        qw_engine_exec(engine, lazy->instr, lazy->ninstr);
        // replace this val's contents to match the result
        val = qw_stack_pop(engine);
        dict->vals[k->index] = val;
    }
    return val;
}

bool qw_val_eq(qw_engine_t *engine, qw_val_t *a, qw_val_t *b){
    switch(*a){
        case QW_VAL_FALSE: return *b == QW_VAL_FALSE;
        case QW_VAL_TRUE: return *b == QW_VAL_TRUE;
        case QW_VAL_NULL: return *b == QW_VAL_NULL;

        case QW_VAL_STRING:
            if(*b != QW_VAL_STRING) return false;
            qw_string_t *da = CONTAINER_OF(a, qw_string_t, type);
            qw_string_t *db = CONTAINER_OF(b, qw_string_t, type);
            return dstr_eq(da->dstr, db->dstr);

        case QW_VAL_SKIP:
        case QW_VAL_LIST:
        case QW_VAL_LAZY:
        case QW_VAL_DICT:
        case QW_VAL_FUNC:
            break;
    }
    qw_error(engine, "equality not defined for this type");
}

static char nibble(char c, bool *ok){
    switch(c){
        case '0':           return 0;
        case '1':           return 1;
        case '2':           return 2;
        case '3':           return 3;
        case '4':           return 4;
        case '5':           return 5;
        case '6':           return 6;
        case '7':           return 7;
        case '8':           return 8;
        case '9':           return 9;
        case 'a': case 'A': return 10;
        case 'b': case 'B': return 11;
        case 'c': case 'C': return 12;
        case 'd': case 'D': return 13;
        case 'e': case 'E': return 14;
        case 'f': case 'F': return 15;
        default: *ok = false; return 0;
    }
}

dstr_t parse_string_literal(qw_engine_t *engine, dstr_t in){
    size_t limit = in.len - 1;
    size_t len = 0;
    bool saw_escape = false;
    // first pass: partial validation and length calculation
    for(size_t i = 1; i < limit; i++){
        char c = in.data[i];
        if(c != '\\'){
            len++;
            continue;
        }
        saw_escape = true;
        if(++i == limit) goto fatal;
        c = in.data[i];
        switch(c){
            case '\\':
            case '\'':
            case '\"':
            case 'n':
            case 'r':
            case 't':
                len++;
                continue;
            case 'x':
                if(++i == limit) goto fatal;
                if(++i == limit) goto fatal;
                len += 2;
                break;
            default: goto fatal;
        }
    }

    // if there were no escapes, pass the input string directly
    if(!saw_escape) return dstr_sub2(in, 1, limit);

    // second pass: write the output
    char *data = qw_engine_malloc(engine, len, 1);
    char *inp = in.data + 1;
    char *outp = data;
    for(size_t i = 0; i < len; i++, outp++){
        char c = *(inp++);
        if(c != '\\'){
            *outp = c;
            continue;
        }
        c = *(inp++);
        switch(c){
            case '\\':
            case '\'':
            case '\"': *outp = c; continue;
            case 'n': *outp = '\n'; continue;
            case 'r': *outp = '\r'; continue;
            case 't': *outp = '\t'; continue;
        }
        // hex decoding
        char x1 = (*inp++);
        char x2 = (*inp++);
        bool ok = true;
        c = (char)(16 * nibble(x1, &ok)) + nibble(x2, &ok);
        if(!ok) goto fatal;
        *outp = c;
    }
    return dstr_from_cstrn(data, len, false);

fatal:
    qw_error(engine, "invalid string from scanner");
}

derr_type_t fmthook_qwval(dstr_t *out, const void *arg){
    const qw_val_t *val = arg;
    switch(*val){
        case QW_VAL_FALSE: return FMT_QUIET(out, "false");
        case QW_VAL_TRUE: return FMT_QUIET(out, "true");
        case QW_VAL_NULL: return FMT_QUIET(out, "null");
        case QW_VAL_SKIP: return FMT_QUIET(out, "skip");

        case QW_VAL_STRING:
            qw_string_t *str = CONTAINER_OF(val, qw_string_t, type);
            return FMT_QUIET(out, "\"%x\"", FD_DBG(&str->dstr));

        case QW_VAL_LIST: return FMT_QUIET(out, "list");
        case QW_VAL_LAZY: return FMT_QUIET(out, "lazy");
        case QW_VAL_DICT: return FMT_QUIET(out, "dict");
        case QW_VAL_FUNC: return FMT_QUIET(out, "func");
    }
    return FMT_QUIET(out, "(unrecognized value)");
}
