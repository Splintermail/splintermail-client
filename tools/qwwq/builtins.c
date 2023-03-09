#include "tools/qwwq/libqw.h"

#include <sys/stat.h>
#include <errno.h>

/* builtins never return functions nor contain binds other than what is
   explicitly created, so they can all safely share a single scope_id */
#define BUILTIN 0

#define STATIC_DSTR_LIT(s) { \
    .data = s, .len = sizeof(s)-1, .size = sizeof(s), .fixed_size = true, \
}

static dstr_t newline = STATIC_DSTR_LIT("\n");
static dstr_t space = STATIC_DSTR_LIT(" ");
static dstr_t doublespace = STATIC_DSTR_LIT("  ");

// given a value, put a method binding the value on the stack
static void method(
    qw_env_t env,
    qw_val_t *val,
    void **instr,  // assumes ninstr==1
    dstr_t *params,
    uintptr_t nparams,
    qw_val_t **defaults
){
    qw_func_t *func = qw_malloc(env, sizeof(*func), PTRSIZE);
    // always bind the provided string
    qw_val_t **binds = qw_malloc(env, PTRSIZE, PTRSIZE);
    binds[0] = val;
    *func = (qw_func_t){
        .type = QW_VAL_FUNC,
        .scope_id = BUILTIN,
        .instr = instr,
        .ninstr = 1,
        .params = params,
        .nparams = nparams,
        .defaults = defaults,
        .binds = binds,
        .nbinds = 1,
    };
    qw_stack_put(env.engine, &func->type);
}

static qw_string_t strip_chars_default = {
    .type = QW_VAL_STRING,
    .dstr = STATIC_DSTR_LIT(" \n\r\t"),
};

static dstr_t strip_params[] = { STATIC_DSTR_LIT("chars") };
static qw_val_t *strip_defaults[] = { &strip_chars_default.type };

typedef dstr_t (*strip_f)(const dstr_t in, const char *chars, size_t n);

// execute the strip logic inside a call to STRING.strip()
static void _instr_strip(qw_env_t env, strip_f strip){
    // get the first argument, the chars to strip
    qw_val_t *charsval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 0));
    if(*charsval != QW_VAL_STRING){
        qw_error(env.engine,
            "strip(chars=) must be a string, not %x",
            FS(qw_val_name(*charsval))
        );
    }
    // get the string input, a bound value of the method
    qw_val_t *inval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 1));
    if(*inval != QW_VAL_STRING) qw_error(env.engine, "invalid method bind");
    qw_string_t *chars = CONTAINER_OF(charsval, qw_string_t, type);
    qw_string_t *in = CONTAINER_OF(inval, qw_string_t, type);
    // do the strip
    dstr_t result = strip(
        in->dstr, chars->dstr.data, chars->dstr.len
    );
    // put the result on the stack
    qw_string_t *out = qw_malloc(env, sizeof(qw_string_t), PTRSIZE);
    *out = (qw_string_t){
        .type = QW_VAL_STRING,
        .dstr = result
    };
    qw_stack_put(env.engine, &out->type);
}

static void qw_instr_strip(qw_env_t env){
    _instr_strip(env, _dstr_strip_chars);
}

void qw_method_strip(qw_env_t env, qw_string_t *string){
    static void *instr = (void*)qw_instr_strip;
    method(env, &string->type, &instr, strip_params, 1, strip_defaults);
}

static void qw_instr_lstrip(qw_env_t env){
    _instr_strip(env, _dstr_lstrip_chars);
}

void qw_method_lstrip(qw_env_t env, qw_string_t *string){
    static void *instr = (void*)qw_instr_lstrip;
    method(env, &string->type, &instr, strip_params, 1, strip_defaults);
}

static void qw_instr_rstrip(qw_env_t env){
    _instr_strip(env, _dstr_rstrip_chars);
}

void qw_method_rstrip(qw_env_t env, qw_string_t *string){
    static void *instr = (void*)qw_instr_rstrip;
    method(env, &string->type, &instr, strip_params, 1, strip_defaults);
}

typedef void (*case_f)(dstr_t *in);

static void _instr_case(qw_env_t env, case_f casefn){
    // get the string input, a bound value of the method
    qw_val_t *inval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 0));
    if(*inval != QW_VAL_STRING) qw_error(env.engine, "invalid method bind");
    qw_string_t *in = CONTAINER_OF(inval, qw_string_t, type);
    // create a new string on the stack
    dstr_t *out = qw_stack_put_new_string(env, in->dstr.len);
    // copy the string
    dstr_append_quiet(out, &in->dstr);
    // apply case function
    casefn(out);
}

static void qw_instr_upper(qw_env_t env){
    _instr_case(env, dstr_upper);
}

void qw_method_upper(qw_env_t env, qw_string_t *string){
    static void *instr = (void*)qw_instr_upper;
    method(env, &string->type, &instr, NULL, 0, NULL);
}

static void qw_instr_lower(qw_env_t env){
    _instr_case(env, dstr_lower);
}

void qw_method_lower(qw_env_t env, qw_string_t *string){
    static void *instr = (void*)qw_instr_lower;
    method(env, &string->type, &instr, NULL, 0, NULL);
}

// returns bool should_continue
static bool next_token(dstr_t *text, dstr_t *token){
rescan: (void)text;
    size_t len = text->len;
    char *data = text->data;
    if(len == 0) return false;
    size_t i = 0;
    switch(data[0]){
        case '\x00':
        case '\x01':
        case '\x02':
        case '\x03':
        case '\x04':
        case '\x05':
        case '\x06':
        case '\x07':
        case '\x08':
        case '\x09':
        // case '\x0a': \n
        case '\x0b':
        case '\x0c':
        case '\x0d':
        case '\x0e':
        case '\x0f':
        case '\x10':
        case '\x11':
        case '\x12':
        case '\x13':
        case '\x14':
        case '\x15':
        case '\x16':
        case '\x17':
        case '\x18':
        case '\x19':
        case '\x1a':
        case '\x1b':
        case '\x1c':
        case '\x1d':
        case '\x1e':
        case '\x1f':
            // non-'\n' control characters
            *token = dstr_sub2(*text, 0, 1);
            *text = dstr_sub2(*text, 1, SIZE_MAX);
            return true;

        case '\n':
            // find consecutive newlines
            while(++i < len){ if(data[i] != '\n') break; }
            *token = dstr_sub2(*text, 0, i);
            *text = dstr_sub2(*text, i, SIZE_MAX);
            // return multiple or final newlines
            if(i > 1 || len == 1) return true;
            // otherwise drop single newlines
            goto rescan;

        case ' ':
            // find consecutive spaces
            while(++i < len){ if(data[i] != ' ') break; }
            // drop all the spaces and rescan
            *text = dstr_sub2(*text, i, SIZE_MAX);
            goto rescan;

        default:
            // find consecutive non-ws, non-punctuation characters
            while(i++ < len){
                unsigned char u = ((unsigned char*)data)[i];
                if(u <= ' ') break;
            }
            // return a non-ws token
            *token = dstr_sub2(*text, 0, i);
            *text = dstr_sub2(*text, i, SIZE_MAX);
            return true;
    }
}

typedef derr_type_t (*append_f)(dstr_t*, const dstr_t*);

static void _wrap(
    qw_env_t env,
    dstr_t in,
    size_t width,
    dstr_t indent,
    dstr_t hang,
    append_f append,
    dstr_t *out
){
    size_t line = 0;
    bool paragraph = true;
    bool sentence = false;
    dstr_t text = in;
    dstr_t tok;
    while(next_token(&text, &tok)){
        unsigned char u = ((unsigned char*)tok.data)[0];
        if(u == '\n'){
            // multiple newlines
            append(out, &tok);
            line = 0;
            paragraph = true;
            continue;
        }
        if(u <= ' '){
            // illegal control character
            qw_error(env.engine,
                "illegal control character in wrap (%x)", FU(u)
            );
        }
        // text token
        if(line == 0){
            // start of a line
            if(paragraph){
                append(out, &indent);
                line = indent.len;
                paragraph = false;
            }else{
                append(out, &hang);
                line = hang.len;
            }
            // always emit at least one token, no matter the size
            append(out, &tok);
            line += tok.len;
        }else if(line + 1 + sentence + tok.len <= width){
            // token fits in line
            append(out, sentence ? &doublespace : &space);
            append(out, &tok);
            line += 1 + sentence + tok.len;
        }else{
            // token does not fit on this line
            append(out, &newline);
            append(out, &hang);
            append(out, &tok);
            line = hang.len + tok.len;
        }
        // check if we'll need a doublespace next
        char last = tok.data[tok.len-1];
        sentence = (last == '.' || last == '!' || last == '?');
    }
}

static derr_type_t dstr_append_count(dstr_t *out, const dstr_t *in){
    out->len += in->len;
    return E_NONE;
}

// wrapping text: width:int, indent:str, hang:str
static void qw_instr_wrap(qw_env_t env){
    // args are (width, indent, hang)
    qw_val_t *widthval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 0));
    qw_val_t *indentval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 1));
    qw_val_t *hangval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 2));
    // input is the first bound value
    qw_val_t *inval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 3));
    // type checks
    if(*widthval != QW_VAL_STRING){
        qw_error(env.engine, "wrap(width=) must be a string");
    }
    if(*indentval != QW_VAL_STRING){
        qw_error(env.engine, "wrap(indent=) must be a string");
    }
    if(*hangval != QW_VAL_STRING){
        qw_error(env.engine, "wrap(hang=) must be a string");
    }
    if(*inval != QW_VAL_STRING){
        qw_error(env.engine, "invalid method bind");
    }
    dstr_t widthstr = CONTAINER_OF(widthval, qw_string_t, type)->dstr;
    dstr_t indent = CONTAINER_OF(indentval, qw_string_t, type)->dstr;
    dstr_t hang = CONTAINER_OF(hangval, qw_string_t, type)->dstr;
    dstr_t in = CONTAINER_OF(inval, qw_string_t, type)->dstr;
    // value checks
    size_t width;
    derr_type_t etype = dstr_tosize_quiet(widthstr, &width, 10);
    if(etype){
        qw_error(env.engine,
            "invalid width in wrap(width=%x)", FD_DBG(&widthstr)
        );
    }
    if(indent.len >= width){
        qw_error(env.engine,
            "indent (\"%x\") exceeds width (%x)", FD_DBG(&indent), FU(width)
        );
    }
    if(hang.len >= width){
        qw_error(env.engine,
            "hang (\"%x\") exceeds width (%x)", FD_DBG(&hang), FU(width)
        );
    }
    // first pass to figure length
    dstr_t len = {0};
    _wrap(env, in, width, indent, hang, dstr_append_count, &len);
    // allocate output
    dstr_t *out = qw_stack_put_new_string(env, len.len);
    // second pass to actually build string
    _wrap(env, in, width, indent, hang, dstr_append_quiet, out);
}

void qw_method_wrap(qw_env_t env, qw_string_t *string){
    static void *instr = (void*)qw_instr_wrap;
    static dstr_t params[] = {
        STATIC_DSTR_LIT("width"),
        STATIC_DSTR_LIT("indent"),
        STATIC_DSTR_LIT("hang"),
    };
    static qw_string_t empty = { .type = QW_VAL_STRING };
    static qw_val_t *defaults[] = { NULL, &empty.type, &empty.type };
    method(env, &string->type, &instr, params, 3, defaults);
}

typedef void (*line_f)(
    dstr_t *out, dstr_t *line, dstr_t *text, append_f append
);

static void prefix_line(
    dstr_t *out, dstr_t *line, dstr_t *text, append_f append
){
    append(out, text);
    append(out, line);
}

static void postfix_line(
    dstr_t *out, dstr_t *line, dstr_t *text, append_f append
){
    append(out, line);
    append(out, text);
}

static void _fix(
    dstr_t *out,
    dstr_t in,
    dstr_t *text,
    dstr_t split,
    bool skip_empty,
    line_f linefn,
    append_f append
){
    while(in.len){
        dstr_t line;
        size_t count;
        dstr_split2_soft(in, split, &count, &line, &in);
        if(!skip_empty || line.len) linefn(out, &line, text, append);
        if(count > 1) append(out, &newline);
    }
}

static void _instr_fix(qw_env_t env, char *name, line_f linefn){
    // first arg: prefix or postfix text
    qw_val_t *textval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 0));
    if(*textval != QW_VAL_STRING){
        qw_error(env.engine,
            "%x(text=) must be a string, not %x",
            FS(name),
            FS(qw_val_name(*textval))
        );
    }
    // second arg: split text
    qw_val_t *splitval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 1));
    if(*splitval != QW_VAL_STRING){
        qw_error(env.engine,
            "%x(split=) must be a string, not %x",
            FS(name),
            FS(qw_val_name(*splitval))
        );
    }
    // third arg: skip_empty
    qw_val_t *skipval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 2));
    if(*skipval > QW_VAL_TRUE){
        qw_error(env.engine,
            "%x(skip_empty=) must be a bool, not %x (%x)",
            FS(name),
            FS(qw_val_name(*skipval))
        );
    }
    // get the string input, a bound value of the method
    qw_val_t *inval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 3));
    if(*inval != QW_VAL_STRING) qw_error(env.engine, "invalid method bind");
    dstr_t text = CONTAINER_OF(textval, qw_string_t, type)->dstr;
    dstr_t split = CONTAINER_OF(splitval, qw_string_t, type)->dstr;
    bool skip_empty = *skipval == QW_VAL_TRUE;
    dstr_t in = CONTAINER_OF(inval, qw_string_t, type)->dstr;
    // first pass to figure length
    dstr_t len = {0};
    _fix(&len, in, &text, split, skip_empty, linefn, dstr_append_count);
    // allocate output
    dstr_t *out = qw_stack_put_new_string(env, len.len);
    // second pass to actually build string
    _fix(out, in, &text, split, skip_empty, linefn, dstr_append_quiet);
}

static void qw_instr_pre(qw_env_t env){
    _instr_fix(env, "pre", prefix_line);
}

static void qw_instr_post(qw_env_t env){
    _instr_fix(env, "post", postfix_line);
}

static dstr_t fix_params[] = {
    STATIC_DSTR_LIT("text"),
    STATIC_DSTR_LIT("split"),
    STATIC_DSTR_LIT("skip_empty"),
};

static qw_string_t fix_split_default = {
    .type = QW_VAL_STRING,
    .dstr = STATIC_DSTR_LIT("\n"),
};

static qw_val_t *fix_defaults[] = { NULL, &fix_split_default.type, &thefalse };

void qw_method_pre(qw_env_t env, qw_string_t *string){
    static void *instr = (void*)qw_instr_pre;
    method(env, &string->type, &instr, fix_params, 3, fix_defaults);
}

void qw_method_post(qw_env_t env, qw_string_t *string){
    static void *instr = (void*)qw_instr_post;
    method(env, &string->type, &instr, fix_params, 3, fix_defaults);
}

static void qw_instr_repl(qw_env_t env){
    // get the first argument, the text to find
    qw_val_t *findval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 0));
    if(*findval != QW_VAL_STRING){
        qw_error(env.engine,
            "repl(find=) must be a string, not %x",
            FS(qw_val_name(*findval))
        );
    }
    // get the second argument, the replcement text
    qw_val_t *replval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 1));
    if(*replval != QW_VAL_STRING){
        qw_error(env.engine,
            "repl(repl=) must be a string, not %x",
            FS(qw_val_name(*replval))
        );
    }
    // get the string input, a bound value of the method
    qw_val_t *inval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 2));
    if(*inval != QW_VAL_STRING) qw_error(env.engine, "invalid method bind");
    dstr_t find = CONTAINER_OF(findval, qw_string_t, type)->dstr;
    dstr_t repl = CONTAINER_OF(replval, qw_string_t, type)->dstr;
    dstr_t in = CONTAINER_OF(inval, qw_string_t, type)->dstr;
    // figure out our output length
    size_t count = dstr_count2(in, find);
    // detect noop optimization
    if(count == 0){
        qw_stack_put(env.engine, inval);
        return;
    }
    size_t len = (in.len + count * repl.len) - count * find.len;
    dstr_t *out = qw_stack_put_new_string(env, len);
    LIST_VAR(dstr_t, find_l, 1);
    LIST_VAR(dstr_t, repl_l, 1);
    DROP_CMD( LIST_APPEND(dstr_t, &find_l, find) );
    DROP_CMD( LIST_APPEND(dstr_t, &repl_l, repl) );
    derr_t e = dstr_recode(&in, out, &find_l, &repl_l, false);
    if(is_error(e)){
        // should never happen, but we want to know if it does
        qw_error(env.engine,
            "failure in dstr_recode(): %x: %x\n",
            FD(error_to_dstr(e.type)),
            FD(&e.msg)
        );
    }
}
void qw_method_repl(qw_env_t env, qw_string_t *string){
    static void *instr = (void*)qw_instr_repl;
    static dstr_t params[] = {
        STATIC_DSTR_LIT("find"), STATIC_DSTR_LIT("repl"),
    };
    static qw_val_t *defaults[] = { NULL, NULL };
    method(env, &string->type, &instr, params, 2, defaults);
}

static void _instr_pad(qw_env_t env, const char *name, bool left){
    // args are width and char (and bound string)
    qw_val_t *widthval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 0));
    qw_val_t *charval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 1));
    qw_val_t *textval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 2));
    // type checks
    if(*widthval != QW_VAL_STRING){
        qw_error(env.engine, "%s(width=) must be a string", FS(name));
    }
    if(*charval != QW_VAL_STRING){
        qw_error(env.engine, "%s(char=) must be a string", FS(name));
    }
    if(*textval != QW_VAL_STRING){
        qw_error(env.engine, "invalid method bind");
    }
    dstr_t widthstr = CONTAINER_OF(widthval, qw_string_t, type)->dstr;
    dstr_t charstr = CONTAINER_OF(charval, qw_string_t, type)->dstr;
    dstr_t text = CONTAINER_OF(textval, qw_string_t, type)->dstr;
    // value checks
    size_t width;
    derr_type_t etype = dstr_tosize_quiet(widthstr, &width, 10);
    if(etype){
        qw_error(env.engine,
            "invalid width in %s(width=%x)", FS(name), FD_DBG(&widthstr)
        );
    }
    if(charstr.len != 1){
        qw_error(env.engine,
            "invalid char in %s(char=%x)", FS(name), FD_DBG(&charstr)
        );
    }
    // detect noop
    if(text.len >= width){
        qw_stack_put(env.engine, textval);
        return;
    }
    dstr_t *out = qw_stack_put_new_string(env, width);
    if(left){
        // left-pad
        dstr_append_char_n(out, charstr.data[0], width - text.len);
        dstr_append_quiet(out, &text);
    }else{
        // right-pad
        dstr_append_quiet(out, &text);
        dstr_append_char_n(out, charstr.data[0], width - text.len);
    }
}

static dstr_t pad_params[] = {
    STATIC_DSTR_LIT("width"),  STATIC_DSTR_LIT("char")
};

static qw_string_t pad_char_default = {
    .type = QW_VAL_STRING,
    .dstr = STATIC_DSTR_LIT(" "),
};

static qw_val_t *pad_defaults[] = { NULL, &pad_char_default.type };

static void qw_instr_lpad(qw_env_t env){
    _instr_pad(env, "lpad", true);
}

void qw_method_lpad(qw_env_t env, qw_string_t *string){
    static void *instr = (void*)qw_instr_lpad;
    method(env, &string->type, &instr, pad_params, 2, pad_defaults);
}

static void qw_instr_rpad(qw_env_t env){
    _instr_pad(env, "rpad", false);
}

void qw_method_rpad(qw_env_t env, qw_string_t *string){
    static void *instr = (void*)qw_instr_rpad;
    method(env, &string->type, &instr, pad_params, 2, pad_defaults);
}

//

static void qw_instr_get(qw_env_t env){
    // get the first argument, the key to fetch
    qw_val_t *keyval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 0));
    if(*keyval != QW_VAL_STRING){
        qw_error(env.engine,
            "get(key=) must be a string, not %x",
            FS(qw_val_name(*keyval))
        );
    }
    // get the dict input, a bound value of the method
    qw_val_t *dictval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 1));
    if(*dictval != QW_VAL_DICT) qw_error(env.engine, "invalid method bind");
    dstr_t key = CONTAINER_OF(keyval, qw_string_t, type)->dstr;
    qw_dict_t *dict = CONTAINER_OF(dictval, qw_dict_t, type);
    // do the dereference
    qw_val_t *out = qw_dict_get(env, dict, key);
    if(!out){
        qw_error(env.engine, "missing key in get(key=\"%x\")", FD_DBG(&key));
    }
    qw_stack_put(env.engine, out);
}

void qw_method_get(qw_env_t env, qw_dict_t *dict){
    static void *instr = (void*)qw_instr_get;
    static dstr_t params[] = { STATIC_DSTR_LIT("key") };
    static qw_val_t *defaults[] = { NULL };
    method(env, &dict->type, &instr, params, 1, defaults);
}

//

static void cell_dims(dstr_t cell, size_t *w, size_t *h){
    size_t wout = 0;
    size_t hout = 0;
    while(cell.len){
        dstr_t line;
        size_t count;
        dstr_split2_soft(cell, newline, &count, &line, &cell);
        wout = MAX(wout, line.len);
        hout++;
    }
    *w = wout;
    *h = MAX(1, hout);
}

static void generate_row(dstr_t *out, size_t *widths, qw_list_t row){
    // start with a copy of each cell
    dstr_t cells[256];
    for(uintptr_t i = 0; i < row.len; i++){
        cells[i] = CONTAINER_OF(row.vals[i], qw_string_t, type)->dstr;
    }
    // iterate through lines from every cell at once
    bool more = true;
    while(more){
        more = false;
        for(uintptr_t i = 0; i < row.len; i++){
            dstr_t line;
            size_t count;
            dstr_split2_soft(cells[i], newline, &count, &line, &cells[i]);
            dstr_append_quiet(out, &line);
            // either pad the cell or terminate the line
            if(i+1 < row.len){
                dstr_append_char_n(out, ' ', widths[i] + 1 - line.len);
            }else{
                // trim whitespace in case of empty cells
                *out = dstr_rstrip_chars(*out, ' ');
                dstr_append_char(out, '\n');
            }
            if(count > 1) more = true;
        }
    }
}

static void qw_instr_table(qw_env_t env){
    // get the first argument, the list of rows
    qw_val_t *rowsval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 0));
    if(*rowsval != QW_VAL_LIST){
        qw_error(env.engine,
            "table(rows=) must be a list, not %x", FS(qw_val_name(*rowsval))
        );
    }
    qw_list_t rows = *CONTAINER_OF(rowsval, qw_list_t, type);
    if(rows.len == 0){
        qw_error(env.engine, "table(rows=) may not be an empty list");
    }
    // first pass: validate types, get column widths, count total rows
    uintptr_t ncols = 0;
    size_t widths[256] = {0};
    size_t nlines = 0;
    for(uintptr_t i = 0; i < rows.len; i++){
        if(*rows.vals[i] != QW_VAL_LIST){
            qw_error(env.engine,
                "table(rows=) must be a list of lists, but found rows[%x] of "
                "type %x",
                FU(i), FS(qw_val_name(*rowsval))
            );
        }
        qw_list_t row = *CONTAINER_OF(rows.vals[i], qw_list_t, type);
        if(i == 0){
            if(row.len == 0){
                qw_error(env.engine, "zero-column table not allowed");
            }
            if(row.len > sizeof(widths)/sizeof(*widths)){
                qw_error(env.engine, "too many columns in table(rows=)");
            }
            ncols = row.len;
        }else if(row.len != ncols){
            qw_error(env.engine,
                "table(rows=) must each have equal lengths, but found "
                "len(rows[%x])=%x while len(rows[0])=%x",
                FU(i), FU(row.len), FU(ncols)
            );
        }
        size_t row_height = 0;
        for(uintptr_t j = 0; j < ncols; j++){
            if(*row.vals[j] != QW_VAL_STRING){
                qw_error(env.engine,
                    "table(rows=) must be a list of lists of strings, but "
                    "found rows[%x][%x] of type %x",
                    FU(i), FU(j), FS(qw_val_name(*row.vals[j]))
                );
            }
            dstr_t cell = CONTAINER_OF(row.vals[j], qw_string_t, type)->dstr;
            size_t w, h;
            cell_dims(cell, &w, &h);
            widths[j] = MAX(widths[j], w);
            row_height = MAX(row_height, h);
        }
        nlines += row_height;
    }
    // figure out how much space we need (overallocating a bit for brevity)
    size_t linesize = ncols;
    for(uintptr_t i = 0; i < ncols; i++){
        linesize += widths[i];
    }
    // allocate output
    dstr_t *out = qw_stack_put_new_string(env, linesize*nlines);
    // generate output
    for(uintptr_t i = 0; i < rows.len; i++){
        qw_list_t row = *CONTAINER_OF(rows.vals[i], qw_list_t, type);
        generate_row(out, widths, row);
    }
    // drop the final newline
    out->len--;
}

static void *void_instr_table = (void*)qw_instr_table;

static dstr_t table_params[] = { STATIC_DSTR_LIT("rows") };

static qw_val_t *table_defaults[] = { NULL };

qw_func_t qw_builtin_table = {
    .type = QW_VAL_FUNC,
    .scope_id = BUILTIN,
    .instr = &void_instr_table,
    .ninstr = 1,
    .params = table_params,
    .nparams = 1,
    .defaults = table_defaults,
    .binds = NULL,
    .nbinds = 0,
};

static void qw_instr_relpath(qw_env_t env){
    if(!env.origin->dirname){
        qw_error(env.engine,
            "relpath() may not be used with reading from stdin"
        );
    }
    // get the first argument, the path to relativify
    qw_val_t *pathval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 0));
    if(*pathval != QW_VAL_STRING){
        qw_error(env.engine,
            "relpath(path=) must be a str, not %x", FS(qw_val_name(*pathval))
        );
    }
    dstr_t path = CONTAINER_OF(pathval, qw_string_t, type)->dstr;
    if(isabs(path)){
        // absolute path is unaffected
        qw_stack_put(env.engine, pathval);
        return;
    }
    size_t len = env.origin->dirname->len + 1 + path.len;
    dstr_t *out = qw_stack_put_new_string(env, len);
    dstr_append_quiet(out, env.origin->dirname);
    dstr_append_char(out, '/');
    dstr_append_quiet(out, &path);
}
static void *void_instr_relpath = (void*)qw_instr_relpath;
static dstr_t relpath_params[] = { STATIC_DSTR_LIT("path") };
static qw_val_t *relpath_defaults[] = { NULL };
qw_func_t qw_builtin_relpath = {
    .type = QW_VAL_FUNC,
    .scope_id = BUILTIN,
    .instr = &void_instr_relpath,
    .ninstr = 1,
    .params = relpath_params,
    .nparams = 1,
    .defaults = relpath_defaults,
    .binds = NULL,
    .nbinds = 0,
};

static void qw_instr_cat(qw_env_t env){
    // get the first argument, the path to cat
    qw_val_t *pathval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 0));
    if(*pathval != QW_VAL_STRING){
        qw_error(env.engine,
            "cat(path=) must be a str, not %x", FS(qw_val_name(*pathval))
        );
    }
    dstr_t path = CONTAINER_OF(pathval, qw_string_t, type)->dstr;

    qw_stack_put_file(env, path);
}
static void *void_instr_cat = (void*)qw_instr_cat;
static dstr_t cat_params[] = { STATIC_DSTR_LIT("path") };
static qw_val_t *cat_defaults[] = { NULL };
qw_func_t qw_builtin_cat = {
    .type = QW_VAL_FUNC,
    .scope_id = BUILTIN,
    .instr = &void_instr_cat,
    .ninstr = 1,
    .params = cat_params,
    .nparams = 1,
    .defaults = cat_defaults,
    .binds = NULL,
    .nbinds = 0,
};

static void qw_instr_exists(qw_env_t env){
    // get the first argument, the path to exists
    qw_val_t *pathval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 0));
    if(*pathval != QW_VAL_STRING){
        qw_error(env.engine,
            "exists(path=) must be a str, not %x", FS(qw_val_name(*pathval))
        );
    }
    dstr_t path = CONTAINER_OF(pathval, qw_string_t, type)->dstr;

    // null-terminate path
    DSTR_VAR(buf, 4096);
    derr_type_t etype = FMT_QUIET(&buf, "%x", FD(&path));
    if(etype) qw_error(env.engine, "filename too long");

    struct stat s;
    int ret = stat(buf.data, &s);
    if(ret && errno != ENOENT && errno != ENOTDIR){
        qw_error(env.engine, "exists(%x): %x", FD_DBG(&path), FE(&errno));
    }
    qw_stack_put_bool(env.engine, ret == 0);
}
static void *void_instr_exists = (void*)qw_instr_exists;
static dstr_t exists_params[] = { STATIC_DSTR_LIT("path") };
static qw_val_t *exists_defaults[] = { NULL };
qw_func_t qw_builtin_exists = {
    .type = QW_VAL_FUNC,
    .scope_id = BUILTIN,
    .instr = &void_instr_exists,
    .ninstr = 1,
    .params = exists_params,
    .nparams = 1,
    .defaults = exists_defaults,
    .binds = NULL,
    .nbinds = 0,
};
