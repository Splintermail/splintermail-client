#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <stdint.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "libdstr.h"

// define standard error types
// derr_type_t E_NONE = NULL;

DSTR_STATIC(E_ANY_dstr, "ANY");
static bool E_ANY_matches(derr_type_t self, derr_type_t other){
    (void)self;
    return other != E_NONE;
}
derr_type_t E_ANY = &(struct derr_type_t){
    .name = &E_ANY_dstr,
    .msg = &E_ANY_dstr,
    .matches = E_ANY_matches,
};

REGISTER_ERROR_TYPE(E_NOMEM, "NOMEM", "out of memory");
REGISTER_ERROR_TYPE(E_SOCK, "SOCKERROR", "socket error");
REGISTER_ERROR_TYPE(E_CONN, "CONNERROR", "connection error");
REGISTER_ERROR_TYPE(E_VALUE, "VALUEERROR", "invalid value");
REGISTER_ERROR_TYPE(E_FIXEDSIZE, "FIXEDSIZE", "fixed-size buffer too small");
REGISTER_ERROR_TYPE(E_OS, "OSERROR", "error in system call");
REGISTER_ERROR_TYPE(E_BADIDX, "BADIDX", "bad index");
REGISTER_ERROR_TYPE(E_OPEN, "OPEN", "error opening file");
REGISTER_ERROR_TYPE(E_PARAM, "PARAM", "bad parameter");
REGISTER_ERROR_TYPE(E_INTERNAL, "INTERNAL", "internal error or bug");
REGISTER_ERROR_TYPE(E_FS, "FILESYSTEM", "error from filesystem");
REGISTER_ERROR_TYPE(E_RESPONSE, "RESPONSE", "bad response from server");
REGISTER_ERROR_TYPE(E_USERMSG, "USERMSG", "usermsg");
REGISTER_ERROR_TYPE(E_CANCELED, "CANCELED", "operation canceled");
REGISTER_ERROR_TYPE(E_BUSY, "BUSY", "resource in use");

// support for error type groups
bool derr_type_group_matches(derr_type_t self, derr_type_t other){
    struct derr_type_group_arg_t *arg = self->data;
    for(size_t i = 0; i < arg->ntypes; i++){
        // allow matching against E_NONE
        if(arg->types[i] == E_NONE && other == E_NONE){
            return true;
        }
        if(arg->types[i]->matches(arg->types[i], other)){
            return true;
        }
    }
    return false;
}

dstr_t error_to_dstr(derr_type_t type){
    if(type == E_NONE){
        DSTR_STATIC(E_OK_dstr, "OK");
        return E_OK_dstr;
    }
    return *type->name;
}

dstr_t error_to_msg(derr_type_t type){
    if(type == E_NONE){
        DSTR_STATIC(E_OK_dstr, "OK");
        return E_OK_dstr;
    }
    return *type->msg;
}

/* take an E_USERMSG message, copy the user message substring with truncation
   and guaranteed null-termination to a buffer, and DROP_VAR the error.  buf
   should be a fixed-size buffer on the stack */
void consume_e_usermsg(derr_t *e, dstr_t *buf){
    if(e->msg.data == NULL){
        /* this only happens in the ENOMEM-after-real-user-facing-error case;
           it's fine to just return an empty string in that case */
        dstr_append_quiet(buf, &DSTR_LIT("(no error message!)"));
        DROP_VAR(e);
        return;
    }

    // use just the first line
    dstr_t line;
    dstr_split2_soft(e->msg, DSTR_LIT("\n"), NULL, &line, NULL);

    dstr_t msg;

    // strip the initial ERROR: string
    DSTR_STATIC(line_prefix, "ERROR: ");
    if(dstr_beginswith(&e->msg, &line_prefix)){
        msg = dstr_sub(&line, line_prefix.len, line.len);
    }else{
        msg = line;
    }

    // truncate, with space for '\0'
    msg = dstr_sub2(msg, 0, buf->size - 1);
    dstr_append_quiet(buf, &msg);
    dstr_null_terminate_quiet(buf);
    DROP_VAR(e);
}

derr_type_t dstr_new_quiet(dstr_t *ds, size_t size){
    // only malloc in power of 2
    size_t new_size = 2;
    while(new_size < size){
        new_size *= 2;
    }
    ds->data = (char*)malloc(new_size);
    if(!ds->data) return E_NOMEM;
    ds->size = new_size;
    ds->len = 0;
    ds->fixed_size = false;
    return E_NONE;
}
derr_t dstr_new(dstr_t *ds, size_t size){
    derr_t e = E_OK;
    derr_type_t type = dstr_new_quiet(ds, size);
    if(type) ORIG(&e, type, "unable to allocate dstr");
    return e;
}

// zeroize the whole contents of a buffer, such as to erase key material
/* Note that dstr_grow() and friends could realloc the buffer, so sensitive
   dstr_t's should prefer fixed-size buffers */
void dstr_zeroize(dstr_t *ds){
    size_t size = ds->size;
    char *data = ds->data;
#ifdef _WIN32
    SecureZeroMemory(data, size);
#elif __APPLE__
    memset_s(data, size, 0, size);
#else // linux
    explicit_bzero(data, size);
#endif
}

void dstr_free(dstr_t* ds){
    if(!ds || !ds->data) return;
    free(ds->data);
    *ds = (dstr_t){0};
}

// same as dstr_free, but zeroize first
void dstr_free0(dstr_t* ds){
    if(!ds || !ds->data) return;
    dstr_zeroize(ds);
    free(ds->data);
    *ds = (dstr_t){0};
}

LIST_FUNCTIONS(dstr_t)
LIST_FUNCTIONS(size_t)
LIST_FUNCTIONS(bool)

dstr_t dstr_from_cstr(char *cstr){
    size_t len = strlen(cstr);
    return (dstr_t){
        .data = cstr,
        .len = len,
        .size = len + 1,
        .fixed_size = true,
    };
}

dstr_t dstr_from_cstrn(char *cstr, size_t n, bool null_terminated){
    return (dstr_t){
        .data = cstr,
        .len = n,
        .size = n + null_terminated,
        .fixed_size = true,
    };
}

dstr_t dstr_sub(const dstr_t* in, size_t start, size_t end){
    dstr_t out;

    // decide start-offset
    size_t so = MIN(in->len, start);
    // decide end-offset
    size_t eo = MIN(in->len, end ? end : in->len);

    // don't let eo < so
    eo = MAX(so, eo);

    out.data = in->data + so;
    out.len = eo - so;
    out.size = out.len;
    out.fixed_size = true;

    return out;
}

dstr_t dstr_sub2(const dstr_t in, size_t start, size_t end){
    dstr_t out;

    // decide start-offset
    size_t so = MIN(in.len, start);
    // decide end-offset
    size_t eo = MIN(in.len, end);

    // don't let eo < so
    eo = MAX(so, eo);

    out.data = in.data + so;
    out.len = eo - so;
    out.size = out.len;
    out.fixed_size = true;

    return out;
}

dstr_t dstr_empty_space(const dstr_t in){
    return (dstr_t){
        .data = in.data + in.len,
        .size = in.size - MIN(in.size, in.len),
        .len = 0,
        .fixed_size = true,
    };
}

dstr_t _dstr_lstrip_chars(const dstr_t in, const char *chars, size_t n){
    dstr_t out = in;
    while(out.len){
        char c = out.data[0];
        bool found = false;
        for(size_t i = 0; i < n; i++){
            if(c != chars[i]) continue;
            out = dstr_sub2(out, 1, out.len);
            found = true;
            break;
        }
        if(!found) break;
    }
    return out;
}

dstr_t _dstr_rstrip_chars(const dstr_t in, const char *chars, size_t n){
    dstr_t out = in;
    while(out.len){
        char c = out.data[out.len-1];
        bool found = false;
        for(size_t i = 0; i < n; i++){
            if(c != chars[i]) continue;
            out.len--;
            found = true;
            break;
        }
        if(!found) break;
    }
    return out;
}

dstr_t _dstr_strip_chars(const dstr_t in, const char *chars, size_t n){
    return _dstr_lstrip_chars(_dstr_rstrip_chars(in, chars, n), chars, n);
}

// case-insensitive character
static char ichar(char c){
    if(c >= 'a' && c <= 'z') return (char)(c - 32);
    return c;
}

static char noichar(char c){
    return c;
}

static int do_dstr_cmp2(const dstr_t a, const dstr_t b, bool sensitive){
    // two NULL strings are considered matching
    if(!a.data && !b.data){
        return 0;
    }
    // as are two zero-length strings
    if(a.len == 0 && b.len == 0){
        return 0;
    }
    // but one NULL and one not are not matching
    if(!a.data || !b.data){
        return a.data ? *a.data : *b.data;
    }
    // don't read past the end of either string
    size_t max = MIN(a.len, b.len);

    char (*char_fn)(char) = sensitive ? noichar : ichar;

    for(size_t i = 0; i < max; i++){
        char ca = char_fn(a.data[i]);
        char cb = char_fn(b.data[i]);
        if(ca != cb){
            return ca - cb;
        }
    }

    // one string might be longer than the other
    if(a.len > b.len){
        int ca = (int)(char_fn(a.data[b.len]));
        return ca;
    }
    if(b.len > a.len){
        int cb = (int)(char_fn(b.data[a.len]));
        return -cb;
    }

    // strings match
    return 0;
}

static int do_dstr_eq(const dstr_t a, const dstr_t b, bool sensitive){
    return a.len == b.len && do_dstr_cmp2(a, b, sensitive) == 0;
}

int dstr_cmp(const dstr_t *a, const dstr_t *b){
    return do_dstr_cmp2(*a, *b, true);
}

int dstr_cmp2(const dstr_t a, const dstr_t b){
    return do_dstr_cmp2(a, b, true);
}

bool dstr_eq(const dstr_t a, const dstr_t b){
    return do_dstr_eq(a, b, true);
}

int dstr_icmp(const dstr_t *a, const dstr_t *b){
    return do_dstr_cmp2(*a, *b, false);
}

int dstr_icmp2(const dstr_t a, const dstr_t b){
    return do_dstr_cmp2(a, b, false);
}

bool dstr_ieq(const dstr_t a, const dstr_t b){
    return do_dstr_eq(a, b, false);
}

void dstr_upper(dstr_t* text){
    for(size_t i = 0; i < text->len; i++){
        char c = text->data[i];
        if(c >= 'a' && c <= 'z'){
            /* because of the range check, this int->signed char cast
               cannot be undefined behavior */
            text->data[i] = (char)(text->data[i] - 32);
        }
    }
}

void dstr_lower(dstr_t* text){
    for(size_t i = 0; i < text->len; i++){
        char c = text->data[i];
        if(c >= 'A' && c <= 'Z'){
            /* because of the range check, this int->signed char cast
               cannot be undefined behavior */
            text->data[i] = (char)(text->data[i] + 32);
        }
    }
}

char* dstr_find(const dstr_t* text, const LIST(dstr_t)* patterns,
                size_t* which_pattern, size_t* partial_match_len){
    size_t max_partial = 0;

    // loop through all characters
    for(size_t i = 0; i < text->len; i++){
        // loop through each pattern
        for(size_t p = 0; p < patterns->len; p++){
            dstr_t* pat = &patterns->data[p];
            // try and match each character in the pattern
            bool match = true;
            for(size_t j = 0; j < pat->len; j++){
                // first check the length limit for identifying partial matches
                if(i + j == text->len){
                    max_partial = MAX(max_partial, j);
                    match = false;
                    // done with this pattern
                    break;
                }
                if(text->data[i + j] != pat->data[j]){
                    // nope, no match. done with this pattern.
                    match = false;
                    break;
                }
            }
            if(match == true){
                // we found a match!
                if(which_pattern) *which_pattern = p;
                if(partial_match_len) *partial_match_len = 0;
                return &text->data[i];
            }
        }
    }

    // no matches found
    if(which_pattern) *which_pattern = 0;
    if(partial_match_len) *partial_match_len = max_partial;
    return NULL;
}

static derr_t do_dstr_recode(const dstr_t* in,
                             dstr_t* out,
                             const LIST(dstr_t)* search_strs,
                             const LIST(dstr_t)* replace_strs,
                             bool force_end,
                             size_t stop_str_index,
                             bool* found_stop,
                             size_t* consumed){
    derr_t e = E_OK;
    if(found_stop) *found_stop = false;

    char* cpy_from = in->data;
    while((uintptr_t)cpy_from - (uintptr_t)in->data < in->len){
        // search *in for one of the search strings
        char* position;
        size_t which_pattern;
        size_t partial = 0;
        dstr_t sub = dstr_sub(in, (uintptr_t)cpy_from - (uintptr_t)in->data, 0);
        if(force_end){
            position = dstr_find(&sub, search_strs, &which_pattern, NULL);
        }else{
            position = dstr_find(&sub, search_strs, &which_pattern, &partial);
        }
        char* cpy_until;
        if(position == NULL){
            // if pattern not found, copy all but the partial match
            cpy_until = in->data + in->len - partial;
            if(cpy_until > cpy_from){
                sub = dstr_sub(in, (uintptr_t)cpy_from - (uintptr_t)in->data,
                               (uintptr_t)cpy_until - (uintptr_t)in->data);
                PROP(&e, dstr_append(out, &sub) );
            }
            // keep track of how much we have copied
            cpy_from = cpy_until;
            break;
        }else{
            cpy_until = position;
            // append everything until the pattern
            if(cpy_until > cpy_from){
                sub = dstr_sub(in, (uintptr_t)cpy_from - (uintptr_t)in->data,
                               (uintptr_t)cpy_until - (uintptr_t)in->data);
                PROP(&e, dstr_append(out, &sub) );
            }
            // then append the replacement for the pattern
            PROP(&e, dstr_append(out, &replace_strs->data[which_pattern]) );
            // keep track of how much we have copied
            cpy_from = cpy_until + search_strs->data[which_pattern].len;
        }
        // if the pattern found was the end pattern, don't copy any more
        if(found_stop && which_pattern == stop_str_index){
            *found_stop = true;
            break;
        }
    }
    if(consumed){
        *consumed = (uintptr_t)cpy_from - (uintptr_t)in->data;
    }
    return e;
}

// primary difference here is *in is not const
derr_t dstr_recode_stream(dstr_t* in,
                          dstr_t* out,
                          const LIST(dstr_t)* search_strs,
                          const LIST(dstr_t)* replace_strs,
                          bool force_end,
                          size_t stop_str_index,
                          bool* found_stop){
    derr_t e = E_OK;
    size_t consumed;
    PROP(&e, do_dstr_recode(in, out, search_strs, replace_strs, force_end,
                         stop_str_index, found_stop, &consumed) );
    dstr_leftshift(in, consumed);
    return e;
}

// primary difference here is *in IS const
derr_t dstr_recode(const dstr_t* in,
                   dstr_t* out,
                   const LIST(dstr_t)* search_strs,
                   const LIST(dstr_t)* replace_strs,
                   bool append){
    derr_t e = E_OK;
    if(append == false){
        out->len = 0;
    }
    PROP(&e,
        do_dstr_recode(in, out, search_strs, replace_strs, true, 0, NULL, NULL)
    );
    return e;
}

static size_t do_dstr_count2(
    const dstr_t text, const dstr_t pattern, bool sensitive, bool exitfirst
){
    char (*char_fn)(char) = sensitive ? noichar : ichar;
    size_t count = 0;
    for(size_t i = 0; i + pattern.len <= text.len; i++){
        // we know from loop boundary there's always room for a full match
        // so now we can safely just check the whole pattern
        bool match = true;
        for(size_t j = 0; j < pattern.len; j++){
            if(char_fn(text.data[i + j]) != char_fn(pattern.data[j])){
                // nope, not a match.  Continue search from i
                match = false;
                break;
            }
        }
        if(match == true){
            if(exitfirst) return 1;
            count++;
            // no need to search anymore until end of current pattern
            // (compensating for the fact that the loop adds 1 to i)
            i += pattern.len - 1;
        }
    }
    return count;
}

size_t dstr_count(const dstr_t* text, const dstr_t* pattern){
    return do_dstr_count2(*text, *pattern, true, false);
}
size_t dstr_count2(const dstr_t text, const dstr_t pattern){
    return do_dstr_count2(text, pattern, true, false);
}
size_t dstr_icount2(const dstr_t text, const dstr_t pattern){
    return do_dstr_count2(text, pattern, false, false);
}

bool dstr_contains(const dstr_t text, const dstr_t pattern){
    return do_dstr_count2(text, pattern, true, true);
}
bool dstr_icontains(const dstr_t text, const dstr_t pattern){
    return do_dstr_count2(text, pattern, false, true);
}

static bool do_beginswith(
    const dstr_t str, const dstr_t pattern, bool sensitive
){
    dstr_t sub = dstr_sub2(str, 0, pattern.len);
    return sensitive ? dstr_eq(pattern, sub) : dstr_ieq(pattern, sub);
}
bool dstr_beginswith(const dstr_t *str, const dstr_t *pattern){
    return do_beginswith(*str, *pattern, true);
}
bool dstr_beginswith2(const dstr_t str, const dstr_t pattern){
    return do_beginswith(str, pattern, true);
}
bool dstr_ibeginswith2(const dstr_t str, const dstr_t pattern){
    return do_beginswith(str, pattern, false);
}

static bool do_endswith(
    const dstr_t str, const dstr_t pattern, bool sensitive
){
    dstr_t sub = dstr_sub2(str, str.len - MIN(str.len, pattern.len), str.len);
    return sensitive ? dstr_eq(pattern, sub) : dstr_ieq(pattern, sub);
}
bool dstr_endswith(const dstr_t *str, const dstr_t *pattern){
    return do_endswith(*str, *pattern, true);
}
bool dstr_endswith2(const dstr_t str, const dstr_t pattern){
    return do_endswith(str, pattern, true);
}
bool dstr_iendswith2(const dstr_t str, const dstr_t pattern){
    return do_endswith(str, pattern, false);
}

derr_type_t dstr_grow_quiet(dstr_t *ds, size_t min_size){
    if(ds->size >= min_size) return E_NONE;
    // we can't realloc if out is fixed-size
    if(ds->fixed_size) return E_FIXEDSIZE;
    // upgrade buffer size by powers of 2
    size_t newsize = MAX(ds->size, 2);
    while(newsize < min_size){
        newsize *= 2;
    }
    void* new = realloc(ds->data, newsize);
    if(!new) return E_NOMEM;
    ds->size = newsize;
    ds->data = new;
    return E_NONE;
}
derr_t dstr_grow(dstr_t* ds, size_t min_size){
    derr_t e = E_OK;
    derr_type_t type = dstr_grow_quiet(ds, min_size);
    if(type) ORIG(&e, type, "unable to grow dstr");
    return e;
}

derr_type_t dstr_grow0_quiet(dstr_t *ds, size_t min_size){
    if(ds->size >= min_size) return E_NONE;
    // we can't realloc if out is fixed-size
    if(ds->fixed_size) return E_FIXEDSIZE;
    // upgrade buffer size by powers of 2
    size_t newsize = MAX(ds->size, 2);
    while(newsize < min_size){
        newsize *= 2;
    }
    void* new = malloc(newsize);
    if(!new) return E_NOMEM;
    memcpy(new, ds->data, ds->len);
    dstr_zeroize(ds);
    free(ds->data);
    ds->data = new;
    return E_NONE;
}
derr_t dstr_grow0(dstr_t* ds, size_t min_size){
    derr_t e = E_OK;
    derr_type_t type = dstr_grow_quiet(ds, min_size);
    if(type) ORIG(&e, type, "unable to grow dstr");
    return e;
}

// append one dstr to another
static derr_type_t do_dstr_append(
    dstr_t *dstr, const dstr_t *new_text, derr_type_t grow_fn(dstr_t*, size_t)
){
    derr_type_t type;
    if(dstr->len + new_text->len > dstr->size){
        type = grow_fn(dstr, dstr->len + new_text->len);
        if(type) return type;
    }

    memcpy(dstr->data + dstr->len, new_text->data, new_text->len);
    dstr->len += new_text->len;
    return E_NONE;
}
derr_type_t dstr_append_quiet(dstr_t *dstr, const dstr_t *new_text){
    return do_dstr_append(dstr, new_text, dstr_grow_quiet);
}
derr_t dstr_append(dstr_t* dstr, const dstr_t* new_text){
    derr_t e = E_OK;
    derr_type_t type = do_dstr_append(dstr, new_text, dstr_grow_quiet);
    if(type) ORIG(&e, type, "unable to append to dstr");
    return e;
}
derr_type_t dstr_append0_quiet(dstr_t *dstr, const dstr_t *new_text){
    return do_dstr_append(dstr, new_text, dstr_grow0_quiet);
}
derr_t dstr_append0(dstr_t* dstr, const dstr_t* new_text){
    derr_t e = E_OK;
    derr_type_t type = do_dstr_append(dstr, new_text, dstr_grow0_quiet);
    if(type) ORIG(&e, type, "unable to append to dstr");
    return e;
}

derr_t dstr_copystrn(const char *in, size_t n, dstr_t *out){
    derr_t e = E_OK;

    if(n){
        PROP(&e, dstr_grow(out, n) );
        memcpy(out->data, in, n);
    }
    out->len = n;

    return e;
}

derr_t dstr_copystr(const char *in, dstr_t *out){
    derr_t e = E_OK;

    if(in){
        PROP(&e, dstr_copystrn(in, strlen(in), out) );
    }else{
        out->len = 0;
    }

    return e;
}

derr_t dstr_copy(const dstr_t* in, dstr_t* out){
    derr_t e = E_OK;

    PROP(&e, dstr_copystrn(in->data, in->len, out) );

    return e;
}

derr_t dstr_copy2(const dstr_t in, dstr_t* out){
    derr_t e = E_OK;

    PROP(&e, dstr_copystrn(in.data, in.len, out) );

    return e;
}


// like dupstr
derr_t dstr_dupstr(const dstr_t in, char** out){
    derr_t e = E_OK;

    *out = dmalloc(&e, in.len + 1);
    CHECK(&e);

    memcpy(*out, in.data, in.len);
    (*out)[in.len] = '\0';

    return e;
}

static derr_t do_dstr_split(const dstr_t* text, const dstr_t* pattern,
        LIST(dstr_t)* out, bool soft){
    derr_t e = E_OK;
    // empty *out
    out->len = 0;

    // get ready for dstr_find
    dstr_t pat = *pattern;
    const LIST(dstr_t) patterns = {&pat, 1, 1, true};
    char* position;

    size_t word_start = 0;
    size_t word_end;

    bool should_continue = true;
    while(should_continue){
        // check if there's another string
        dstr_t sub = dstr_sub(text, word_start, text->len);
        position = dstr_find(&sub, &patterns, NULL, NULL);

        if(position != NULL){
            // if we have a break, add a word to the list
            word_end = (uintptr_t)position - (uintptr_t)text->data;
        }else{
            // if no more breaks, add one last word to the list
            word_end = text->len;
            should_continue = false;
        }

        // append the new dstr that points into the text
        dstr_t new;
        if(word_end == 0){
            /* in this special case, we manually add an empty string, because
               dstr_sub will interpret the 0 specially */
            new.data = position;
            new.len = 0;
            new.size = 0;
            new.fixed_size = true;
        }else{
            new = dstr_sub(text, word_start, word_end);
        }
        // try to append the new dstr_t to the list
        IF_PROP(&e, LIST_APPEND(dstr_t, out, new)){
            // check for soft failure case
            if(soft && e.type == E_FIXEDSIZE){
                DROP_VAR(&e);
                // set the last token to point to the remainder of the text
                if(out->len > 0){
                    dstr_t *last = &out->data[out->len-1];
                    last->len = text->len -
                        ((uintptr_t)last->data - (uintptr_t)text->data);
                }
                // return without error
                break;
            }
            return e;
        }

        // get ready for the next dstr_find()
        word_start = word_end + pattern->len;
    }

    return e;
}

derr_t dstr_split(const dstr_t* text, const dstr_t* pattern,
        LIST(dstr_t)* out){
    derr_t e = E_OK;
    PROP(&e, do_dstr_split(text, pattern, out, false) );
    return e;
}

derr_t dstr_split_soft(const dstr_t* text, const dstr_t* pattern,
        LIST(dstr_t)* out){
    derr_t e = E_OK;
    PROP(&e, do_dstr_split(text, pattern, out, true) );
    return e;
}

static derr_t do_dstr_split2(
    const dstr_t text,
    const dstr_t pattern,
    size_t *len_out,
    dstr_t **outs,
    size_t nouts,
    bool soft
){
    derr_t e = E_OK;

    // zeroize outputs
    for(size_t i = 0; i < nouts; i++){
        if(outs[i] != NULL){
            *outs[i] = (dstr_t){0};
        }
    }
    // in the special case of nouts=2; len_out may be actually useless
    if(len_out) *len_out = 0;
    size_t len = 0;

    if(nouts < 1){
        ORIG(&e, E_FIXEDSIZE, "zero-length output array");
    }

    // get ready for dstr_find
    // TODO: find a better way than this
    dstr_t pat = dstr_sub2(pattern, 0, SIZE_MAX);
    const LIST(dstr_t) patterns = {&pat, 1, 1, true};
    char* position;

    size_t word_start = 0;
    size_t word_end;

    bool should_continue = true;
    while(should_continue){
        // check if there's another string
        dstr_t sub = dstr_sub2(text, word_start, text.len);
        position = dstr_find(&sub, &patterns, NULL, NULL);

        if(position != NULL){
            // if we have a break, add a word to the list
            word_end = (uintptr_t)position - (uintptr_t)text.data;
        }else{
            // if no more breaks, add one last word to the list
            word_end = text.len;
            should_continue = false;
        }

        // append the new dstr that points into the text
        dstr_t new = dstr_sub2(text, word_start, word_end);

        // check if the list is full
        if(len == nouts){
            if(soft){
                // set the last token to point to the remainder of the text
                dstr_t *last = outs[len-1];
                if(last != NULL){
                    last->len = text.len -
                        ((uintptr_t)last->data - (uintptr_t)text.data);
                }
                // return without error
                break;
            }else{
                ORIG(&e, E_FIXEDSIZE, "too many words");
            }
        }

        dstr_t *dest = outs[len++];
        if(dest != NULL){
            *dest = new;
        }

        // get ready for the next dstr_find()
        word_start = word_end + pattern.len;
    }

    if(len_out) *len_out = len;
    return e;
}

derr_t _dstr_split2(
    const dstr_t text,
    const dstr_t pattern,
    size_t *len,
    dstr_t **outs,
    size_t nouts
){
    derr_t e = E_OK;
    PROP(&e, do_dstr_split2(text, pattern, len, outs, nouts, false) );
    return e;
}

void _dstr_split2_soft(
    const dstr_t text,
    const dstr_t pattern,
    size_t *len,
    dstr_t **outs,
    size_t nouts
){
    DROP_CMD( do_dstr_split2(text, pattern, len, outs, nouts, true) );
}

void dstr_leftshift(dstr_t* buffer, size_t count){
    // detect noops
    if(count == 0) return;

    size_t shift = MIN(count, buffer->len);

    size_t newlen = buffer->len - shift;

    // check if the memmove would be trivial
    if(newlen == 0){
        buffer->len = 0;
        return;
    }

    // if the string remaining fits entirely in the shift amount
    // then we can do a memcpy instead of memmove
    if(newlen <= shift){
        memcpy(buffer->data, buffer->data + shift, newlen);
    }else{
        memmove(buffer->data, buffer->data + shift, newlen);
    }

    buffer->len = newlen;
}

derr_type_t dstr_null_terminate_quiet(dstr_t* ds){
    // make sure that the ds is long enough to null-terminate
    if(ds->len >= ds->size){
        derr_type_t type = dstr_grow_quiet(ds, ds->len + 1);
        if(type) return type;
    }

    // add a null-terminating character
    ds->data[ds->len] = '\0';
    return E_NONE;
}
derr_t dstr_null_terminate(dstr_t* ds){
    derr_t e = E_OK;
    derr_type_t type = dstr_null_terminate_quiet(ds);
    if(type) ORIG(&e, type, "unable to null terminate");
    return e;
}

derr_t list_append_with_mem(LIST(dstr_t)* list, dstr_t* mem, dstr_t in,
                            bool null_term){
    derr_t e = E_OK;
    // remember how mem started
    size_t start = mem->len;
    char* oldp = mem->data;
    /* do the block memory allocation all at once (so there's only one chance
       for reallocation) */
    size_t to_grow = in.len + (null_term ? 1 : 0);
    PROP(&e, dstr_grow(mem, mem->len + to_grow) );
    // now do the appending
    // append *in to the backing memory
    dstr_append(mem, &in);
    size_t end = mem->len;
    // null_term if that was requested
    if(null_term){
        dstr_null_terminate(mem);
        mem->len++;
    }
    // detect reallocations
    char* newp = mem->data;
    if(oldp != newp){
        for(size_t i = 0; i < list->len; i++){
            if(list->data[i].data){
                list->data[i].data = newp + (list->data[i].data - oldp);
            }
        }
    }
    // append the new chunk of *mem to *list
    dstr_t sub = dstr_sub(mem, start, end);
    PROP_GO(&e, LIST_APPEND(dstr_t, list, sub), fail_1);
    return e;

fail_1:
    // if we failed in the LIST_APPEND, remove stuff from the mem
    mem->len = start;
    return e;
}

bool in_list(const dstr_t* val, const LIST(dstr_t)* list, size_t* idx){
    for(size_t i = 0; i < list->len; i++){
        int result = dstr_cmp(val, &list->data[i]);
        if(result == 0){
            if(idx) *idx = i;
            return true;
        }
    }
    if(idx) *idx = (size_t) -1;
    return false;
}

derr_t dstr_read(int fd, dstr_t* buffer, size_t count, size_t* amnt_read){
    derr_t e = E_OK;
    if(amnt_read) *amnt_read = 0;
    // compat_read() returns a signed size_t (ssize_t)
    ssize_t ar = 0;
    if(count == 0){
        // 0 means "try to fill buffer"
        count = buffer->size - buffer->len;
        // make sure the buffer isn't full
        if(count == 0){
            ORIG(&e, E_FIXEDSIZE, "buffer is full");
        }
    }else{
        // grow buffer to fit
        PROP(&e, dstr_grow(buffer, buffer->len + count) );
    }
    ar = compat_read(fd, buffer->data + buffer->len, count);
    if(ar < 0){
        TRACE(&e, "%x: %x\n", FS("read"), FE(errno));
        ORIG(&e, E_OS, "error in read");
    }
    buffer->len += (size_t)ar;
    if(amnt_read) *amnt_read = (size_t)ar;
    return e;
}

derr_t dstr_read_all(int fd, dstr_t *out){
    derr_t e = E_OK;

    // prepare a revertable state
    dstr_t *freeable = NULL;
    size_t old_len = out->len;
    if(out->size == 0){
        PROP(&e, dstr_grow(out, 1024) );
        freeable = out;
    }

    size_t amnt_read;
    do {
        size_t space = out->size - out->len;
        if(!space){
            PROP_GO(&e, dstr_grow(out, out->len * 2), fail);
            space = out->len;
        }

        PROP_GO(&e, dstr_read(fd, out, 0, &amnt_read),  fail);
    } while(amnt_read);

    return e;

fail:
    // revert changes
    dstr_free(freeable);
    out->len = old_len;
    return e;
}

derr_t dstr_write(int fd, const dstr_t* buffer){
    derr_t e = E_OK;
    // return early if buffer is empty
    if(buffer->len == 0) return e;
    // repeatedly try to write until we write the whole buffer
    size_t total = 0;
    size_t zero_writes = 0;
    while(total < buffer->len){
        ssize_t amnt_written = compat_write(fd, buffer->data + total, buffer->len - total);
        // writing zero bytes is a failure mode
        if(amnt_written < 0){
            TRACE(&e, "%x: %x\n", FS("write"), FE(errno));
            ORIG(&e, E_OS, "dstr_write failed");
        }
        /* there is not a great way to handle this, since `man 2 write` says:
           "if count is zero and fd referes to a non-regular file, results are
           not specified", but we are going to to assume that the calling
           function needs the whole buffer written, and throw an error if it
           fails. TODO: a better way to handle this would be to only use
           dstr_write() for regular files and have a separate dstr_send() for
           the rest of the library... but that wouldn't help with writing to
           stdout... */
        if(amnt_written == 0){
            if(zero_writes++ > 100){
                ORIG(&e, E_OS, "too many zero writes");
            }
        }
        total += (size_t)amnt_written;
    }

    return e;
}

derr_t dstr_fread(FILE* f, dstr_t* buffer, size_t count, size_t* amnt_read){
    derr_t e = E_OK;
    size_t ar = 0;

    if(count == 0){
        // 0 means "try to fill buffer"
        count = buffer->size - buffer->len;
    }else{
        // grow buffer to fit
        PROP(&e, dstr_grow(buffer, buffer->len + count) );
    }
    // make sure the buffer isn't full
    if(count == 0){
        ORIG(&e, E_FIXEDSIZE, "buffer is full");
    }
    ar = fread(buffer->data + buffer->len, 1, count, f);
    if(ar > 0){
        buffer->len += ar;
    }else if(ferror(f)){
        ORIG(&e, E_OS, "fread: %x\n", FE(errno));
    }
    if(amnt_read) *amnt_read = ar;
    return e;
}

derr_t dstr_fread_all(FILE* f, dstr_t* buffer){
    derr_t e = E_OK;

    do {
        // support both dynamically-sized and fixed-size buffers
        size_t count;
        if(buffer->fixed_size){
            count = buffer->size - buffer->len;
            if(count == 0) ORIG(&e, E_FIXEDSIZE, "buffer is full");
        }else{
            count = 4096;
            PROP(&e, dstr_grow(buffer, buffer->len + count) );
        }
        buffer->len += fread(buffer->data + buffer->len, 1, count, f);
        if(ferror(f)) ORIG(&e, E_OS, "fread: %x\n", FE(errno));
    } while(!feof(f));

    return e;
}

derr_type_t dstr_fwrite_quiet(FILE* f, const dstr_t* buffer){
    // return early if buffer is empty
    if(buffer->len == 0) return E_NONE;

    size_t amnt_written = fwrite(buffer->data, 1, buffer->len, f);
    if(amnt_written < buffer->len){
        // TODO: check for EOF or ENOMEM or other such things
        return E_OS;
    }

    return E_NONE;
}
derr_t dstr_fwrite(FILE* f, const dstr_t* buffer){
    derr_t e = E_OK;

    derr_type_t type = dstr_fwrite_quiet(f, buffer);
    if(type) ORIG(&e, type, "unable to write to FILE pointer");

    return e;
}

derr_type_t bin2b64_quiet(
    const dstr_t* bin,
    dstr_t* b64,
    size_t line_width,
    bool force_end,
    size_t *consumed
){
    derr_type_t type;
    unsigned char ch[3];
    memset(ch, 0, sizeof(ch));
    size_t ch_idx = 0;
    if(consumed) *consumed = 0;

    DSTR_STATIC(line_break, "\n");

    size_t chunk_size = (line_width / 4) * 3;
    size_t end_condition;
    if(line_width > 0){
        size_t end_of_full_lines = bin->len - (bin->len % chunk_size);
        end_condition = force_end ? bin->len : end_of_full_lines;
    }else{
        end_condition = bin->len;
    }

    size_t i;
    // encode each byte
    for(i = 0; i < end_condition; i++){
        ch[ch_idx++] = (unsigned char)bin->data[i];
        // check to flush chunk
        if(ch_idx == 3 || i == bin->len - 1){
            DSTR_VAR(buff, 4);
            for(size_t j = 0; j < 4; j++){
                // unfilled chunk condition
                if(j > ch_idx){
                    buff.data[buff.len++] = '=';
                    continue;
                }
                unsigned int val = 0;
                switch(j){
                    // 0x3 = 0b0011
                    // 0xc = 0x1100
                    case 0: val = 0x3f & (ch[0] >> 2); break;
                    case 1: val = (unsigned char)( (0x30 & (ch[0] << 4)) | (0x0f & (ch[1] >> 4)));break;
                    case 2: val = (unsigned char)((0x3c & (ch[1] << 2)) | (0x03 & (ch[2] >> 6))); break;
                    case 3: val = 0x3f & ch[2]; break;
                }
                /* due to the range checks, these casts cannot result in
                   undefined behavior */
                if(val < 26){
                    buff.data[buff.len++] = (char)('A' + val);
                }else if(val < 52){
                    buff.data[buff.len++] = (char)('a' + val - 26);
                }else if(val < 62){
                    buff.data[buff.len++] = (char)('0' + val - 52);
                }else if(val == 62){
                    buff.data[buff.len++] = '+';
                }else if(val == 63){
                    buff.data[buff.len++] = '/';
                }
            }
            // append buffer to dstr
            type = dstr_append_quiet(b64, &buff);
            if(type) return type;
            // reset chunk
            memset(ch, 0, sizeof(ch));
            ch_idx = 0;
            // check to append line break
            if(line_width > 0){
                if((i + 1) % chunk_size == 0 || i + 1 == bin->len){
                    type = dstr_append_quiet(b64, &line_break);
                    if(type) return type;
                }
            }
        }
    }
    if(consumed) *consumed = i;
    return E_NONE;
}

derr_t bin2b64(const dstr_t *bin, dstr_t *b64){
    derr_t e = E_OK;

    derr_type_t type = bin2b64_quiet(bin, b64, 0, true, NULL);
    if(type) ORIG(&e, type, "failed to append to output");

    return e;
}

derr_t bin2b64_stream(
    dstr_t* bin, dstr_t* b64, size_t line_width, bool force_end
){
    derr_t e = E_OK;

    size_t consumed = 0;
    derr_type_t type = bin2b64_quiet(bin, b64, line_width, force_end, &consumed);
    if(type) ORIG(&e, type, "failed to append to output");

    dstr_leftshift(bin, consumed);
    return e;
}

derr_type_t b642bin_quiet(const dstr_t* b64, dstr_t* bin, size_t *consumed){
    derr_type_t type;
    // build chunks of b64 characters to decode all-at-once
    unsigned char ch[4];
    memset(ch, 0, sizeof(ch));
    int ch_idx = 0;
    int skip = 0;
    size_t total_read = 0;
    if(consumed) *consumed = 0;

    for(size_t i = 0; i < b64->len; i++){
        char c = b64->data[i];
        /* due to the range checks, these casts cannot result in undefined
           behavior */
        if(c >= 'A' && c <= 'Z'){
            ch[ch_idx++] = (unsigned char)(c - 'A');
        }else if(c >= 'a' && c <= 'z'){
            ch[ch_idx++] = (unsigned char)(c - 'a' + 26);
        }else if(c >= '0' && c <= '9'){
            ch[ch_idx++] = (unsigned char)(c - '0' + 52);
        }else if(c == '+'){
            ch[ch_idx++] = 62;
        }else if(c == '/'){
            ch[ch_idx++] = 63;
        }else if(c == '='){
            skip++;
            ch_idx++;
        }
        // check if we should flush
        if(ch_idx == 4){
            DSTR_VAR(buffer, 3);
            unsigned char u = (unsigned char)(ch[0] << 2 | ch[1] >> 4);
            buffer.data[buffer.len++] = uchar_to_char(u);
            if(skip < 2){
                u = (unsigned char)(ch[1] << 4 | ch[2] >> 2);
                buffer.data[buffer.len++] = uchar_to_char(u);
            }
            if(skip < 1){
                u = (unsigned char)(ch[2] << 6 | ch[3]);
                buffer.data[buffer.len++] = uchar_to_char(u);
            }
            type = dstr_append_quiet(bin, &buffer);
            if(type) return type;
            ch_idx = 0;
            total_read = i + 1;
            if(skip > 0) break;
        }
    }
    if(consumed) *consumed = total_read;
    return E_NONE;
}

derr_t b642bin(const dstr_t *b64, dstr_t *bin){
    derr_t e = E_OK;

    derr_type_t type = b642bin_quiet(b64, bin, NULL);
    if(type) ORIG(&e, type, "failed to append to output");

    return e;
}

derr_t b642bin_stream(dstr_t* b64, dstr_t* bin){
    derr_t e = E_OK;

    size_t consumed = 0;
    derr_type_t type = b642bin_quiet(b64, bin, &consumed);
    if(type) ORIG(&e, type, "failed to append to output");

    dstr_leftshift(b64, consumed);
    return e;
}

size_t b642bin_output_len(size_t in){
    // every four bytes return three output bytes
    // in should always be divisble by 4, but we'll round up just in case.
    return ((in + 3) / 4) * 3;
}

size_t bin2b64_output_len(size_t in){
    // every three bytes (rounding up) return four output bytes
    return ((in + 2) / 3) * 4;
}

// no error checking
char hex_high_nibble(unsigned char u){
    switch(u >> 4){
        case 0:  return '0';
        case 1:  return '1';
        case 2:  return '2';
        case 3:  return '3';
        case 4:  return '4';
        case 5:  return '5';
        case 6:  return '6';
        case 7:  return '7';
        case 8:  return '8';
        case 9:  return '9';
        case 10: return 'a';
        case 11: return 'b';
        case 12: return 'c';
        case 13: return 'd';
        case 14: return 'e';
        default: return 'f';
    }
}
char hex_low_nibble(unsigned char u){
    switch(u & 0xf){
        case 0:  return '0';
        case 1:  return '1';
        case 2:  return '2';
        case 3:  return '3';
        case 4:  return '4';
        case 5:  return '5';
        case 6:  return '6';
        case 7:  return '7';
        case 8:  return '8';
        case 9:  return '9';
        case 10: return 'a';
        case 11: return 'b';
        case 12: return 'c';
        case 13: return 'd';
        case 14: return 'e';
        default: return 'f';
    }
}

static derr_type_t bin2hex_quiet(const dstr_t* bin, dstr_t* hex){
    derr_type_t type;
    DSTR_VAR(temp, 3);
    temp.len = 2;
    for(size_t i = 0; i < bin->len; i++){
        int ret = snprintf(
            temp.data, temp.size, "%.2x", (unsigned char)bin->data[i]
        );
        if(ret != 2){
            return E_INTERNAL;
        }
        type = dstr_append_quiet(hex, &temp);
        if(type != E_NONE) return type;
    }
    return E_NONE;
}
derr_t bin2hex(const dstr_t* bin, dstr_t* hex){
    derr_t e = E_OK;
    derr_type_t type = bin2hex_quiet(bin, hex);
    if(type == E_INTERNAL) ORIG(&e, type, "snprintf printed the wrong amount");
    if(type == E_FIXEDSIZE) ORIG(&e, type, "output too short");
    if(type == E_NOMEM) ORIG(&e, type, "failed to grow output");
    if(type) ORIG(&e, type, "bin2hex failed");
    return e;
}

derr_t hex2bin(const dstr_t* hex, dstr_t* bin){
    derr_t e = E_OK;
    int shift = 0;

    // get a one-character long dstr_t to make error handling easy
    DSTR_VAR(one_char, 1);
    one_char.len = 1;
    // get an unsigned char* pointing into the dstr_t to make bitmath safe
    unsigned char* byte = (unsigned char*)one_char.data;
    *byte = 0;

    // decode each character
    for(size_t i = 0; i < hex->len; i++){
        unsigned char val;
        if(hex->data[i] >= '0' && hex->data[i] <= '9'){
            val = (unsigned char)(hex->data[i] - '0');
        }else if (hex->data[i] >= 'A' && hex->data[i] <= 'F'){
            val = (unsigned char)(hex->data[i] - 'A' + 10);
        }else if (hex->data[i] >= 'a' && hex->data[i] <= 'f'){
            val = (unsigned char)(hex->data[i] - 'a' + 10);
        }else if (isspace(hex->data[i])){
            // ignore whitespace
            continue;
        }else{
            ORIG(&e, E_PARAM, "bad hex input");
        }
        shift = 1 - shift;
        *byte |= (unsigned char)(val << (4 * shift));
        if(!shift){
            // append byte to dstr
            PROP(&e, dstr_append(bin, &one_char) );
            *byte = 0;
        }
    }
    return e;
}

derr_type_t dstr_append_char(dstr_t* dstr, char val){
    /* in benchmarking, an if statement is negligible compared to the function
       call, so if append_char is in a hot loop this makes about a 4x diff */
    if(dstr->len >= dstr->size){
        derr_type_t type = dstr_grow_quiet(dstr, dstr->len+1);
        if(type) return type;
    }
    dstr->data[dstr->len++] = val;
    return E_NONE;
}

// append the same char multiple times, such as for padding a string
derr_type_t dstr_append_char_n(dstr_t* dstr, char val, size_t n){
    if(dstr->len + n > dstr->size){
        derr_type_t type = dstr_grow_quiet(dstr, dstr->len+n);
        if(type) return type;
    }
    memset(dstr->data+dstr->len, val, n);
    dstr->len += n;
    return E_NONE;
}
