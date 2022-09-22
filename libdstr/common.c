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

const dstr_t *error_to_dstr(derr_type_t type){
    if(type == E_NONE){
        DSTR_STATIC(E_OK_dstr, "OK");
        return &E_OK_dstr;
    }
    return type->name;
}

const dstr_t *error_to_msg(derr_type_t type){
    if(type == E_NONE){
        DSTR_STATIC(E_OK_dstr, "OK");
        return &E_OK_dstr;
    }
    return type->msg;
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

void dstr_free(dstr_t* ds){
    if(ds && ds->data) {
        free(ds->data);
        *ds = (dstr_t){0};
    }
}

LIST_FUNCTIONS(dstr_t)
LIST_FUNCTIONS(size_t)
LIST_FUNCTIONS(bool)

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
    // but one NULL and one not are not matching
    if(!a.data != !b.data) return false;
    // otherwise expect length to match, or content to match
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

/* behaves differently than atoi; it will err out if it sees a non-number
   character and it doesn't handle whitespace; the idea is that you should know
   what you think is a number and there should be built-in error handling if it
   is not a number */
/*  features I want:
        strict parsing of the entire string
        accept any type that json can output
        range checking
    parser summary:
       base-10/16 float: [+|-] [0x|0X] 0|d*[.d*] [ [+|-] E|e d* ]
                     or: inf or nan (case insensitive)
       int: [+|-] [0|0x] d* [.d*] (last part is either ignored or rejected)
   */
derr_t dstr_toi(const dstr_t* in, int* out, int base){
    derr_t e = E_OK;
    // copy the version into a null-terminated string to read into atof()
    DSTR_VAR(temp, 128);
    derr_t e2 = dstr_copy(in, &temp);
    CATCH(e2, E_ANY){
        RETHROW(&e, &e2, E_PARAM);
    }
    e2 = dstr_null_terminate(&temp);
    CATCH(e2, E_ANY){
        RETHROW(&e, &e2, E_PARAM);
    }
    // now parse
    char* endptr;
    errno = 0; long result = strtol(temp.data, &endptr, base);
    // check for error
    if(errno){
        TRACE(&e, "strtol(%x): %x\n", FD(in), FE(&errno));
        ORIG(&e, E_PARAM, "invalid number string");
    }
    // make sure everything was parsed
    if(endptr != &temp.data[temp.len]){
        TRACE(&e, "input was \"%x\"\n", FD(in));
        ORIG(&e, E_PARAM, "invalid number string");
    }
    // check bounds
    if(result > INT_MAX || result < INT_MIN){
        TRACE(&e, "input was \"%x\"\n", FD(in));
        ORIG(&e, E_PARAM, "number out of range");
    }
    // return value
    *out = (int)result;
    return e;
}
derr_t dstr_tou(const dstr_t* in, unsigned int* out, int base){
    derr_t e = E_OK;
    // copy the version into a null-terminated string to read into atof()
    DSTR_VAR(temp, 128);
    derr_t e2 = dstr_copy(in, &temp);
    CATCH(e2, E_ANY){
        RETHROW(&e, &e2, E_PARAM);
    }
    e2 = dstr_null_terminate(&temp);
    CATCH(e2, E_ANY){
        RETHROW(&e, &e2, E_PARAM);
    }
    // now parse
    char* endptr;
    errno = 0;
    unsigned long result = strtoul(temp.data, &endptr, base);
    // check for error
    if(errno){
        TRACE(&e, "srtoul(%x): %x\n", FD(in), FE(&errno));
        ORIG(&e, E_PARAM, "invalid number string");
    }
    // make sure everything was parsed
    if(endptr != &temp.data[temp.len]){
        TRACE(&e, "input was \"%x\"\n", FD(in));
        ORIG(&e, E_PARAM, "invalid number string");
    }
    // check bounds
    if(result > UINT_MAX){
        TRACE(&e, "input was \"%x\"\n", FD(in));
        ORIG(&e, E_PARAM, "number out of range");
    }
    // return value
    *out = (unsigned int)result; return e;
}
derr_t dstr_tol(const dstr_t* in, long* out, int base){
    derr_t e = E_OK;
    // copy the version into a null-terminated string to read into atof()
    DSTR_VAR(temp, 128);
    derr_t e2 = dstr_copy(in, &temp);
    CATCH(e2, E_ANY){
        RETHROW(&e, &e2, E_PARAM);
    }
    e2 = dstr_null_terminate(&temp);
    CATCH(e2, E_ANY){
        RETHROW(&e, &e2, E_PARAM);
    }
    // now parse
    char* endptr;
    errno = 0;
    long result = strtol(temp.data, &endptr, base);
    // check for error
    if(errno){
        TRACE(&e, "srtol(%x): %x\n", FD(in), FE(&errno));
        ORIG(&e, E_PARAM, "invalid number string");
    }
    // make sure everything was parsed
    if(endptr != &temp.data[temp.len]){
        TRACE(&e, "input was \"%x\"\n", FD(in));
        ORIG(&e, E_PARAM, "invalid number string");
    }
    // return value
    *out = result;
    return e;
}
derr_t dstr_toul(const dstr_t* in, unsigned long* out, int base){
    derr_t e = E_OK;
    // copy the version into a null-terminated string to read into atof()
    DSTR_VAR(temp, 128);
    derr_t e2 = dstr_copy(in, &temp);
    CATCH(e2, E_ANY){
        RETHROW(&e, &e2, E_PARAM);
    }
    e2 = dstr_null_terminate(&temp);
    CATCH(e2, E_ANY){
        RETHROW(&e, &e2, E_PARAM);
    }
    // now parse
    char* endptr;
    errno = 0;
    unsigned long result = strtoul(temp.data, &endptr, base);
    // check for error
    if(errno){
        TRACE(&e, "srtoul(%x): %x\n", FD(in), FE(&errno));
        ORIG(&e, E_PARAM, "invalid number string");
    }
    // make sure everything was parsed
    if(endptr != &temp.data[temp.len]){
        TRACE(&e, "input was \"%x\"\n", FD(in));
        ORIG(&e, E_PARAM, "invalid number string");
    }
    // return value
    *out = result;
    return e;
}
derr_t dstr_toll(const dstr_t* in, long long* out, int base){
    derr_t e = E_OK;
    // copy the version into a null-terminated string to read into atof()
    DSTR_VAR(temp, 128);
    derr_t e2 = dstr_copy(in, &temp);
    CATCH(e2, E_ANY){
        RETHROW(&e, &e2, E_PARAM);
    }
    e2 = dstr_null_terminate(&temp);
    CATCH(e2, E_ANY){
        RETHROW(&e, &e2, E_PARAM);
    }
    // now parse
    char* endptr;
    errno = 0;
    long long result = strtoll(temp.data, &endptr, base);
    // check for error
    if(errno){
        TRACE(&e, "srtoll(%x): %x\n", FD(in), FE(&errno));
        ORIG(&e, E_PARAM, "invalid number string");
    }
    // make sure everything was parsed
    if(endptr != &temp.data[temp.len]){
        TRACE(&e, "input was \"%x\"\n", FD(in));
        ORIG(&e, E_PARAM, "invalid number string");
    }
    // return value
    *out = result;
    return e;
}
derr_t dstr_toull(const dstr_t* in, unsigned long long* out, int base){
    derr_t e = E_OK;
    // copy the version into a null-terminated string to read into atof()
    DSTR_VAR(temp, 128);
    derr_t e2 = dstr_copy(in, &temp);
    CATCH(e2, E_ANY){
        RETHROW(&e, &e2, E_PARAM);
    }
    e2 = dstr_null_terminate(&temp);
    CATCH(e2, E_ANY){
        RETHROW(&e, &e2, E_PARAM);
    }
    // now parse
    char* endptr;
    errno = 0;
    unsigned long long result = strtoull(temp.data, &endptr, base);
    // check for error
    if(errno){
        TRACE(&e, "srtoull(%x): %x\n", FD(in), FE(&errno));
        ORIG(&e, E_PARAM, "invalid number string");
    }
    // make sure everything was parsed
    if(endptr != &temp.data[temp.len]){
        TRACE(&e, "input was \"%x\"\n", FD(in));
        ORIG(&e, E_PARAM, "invalid number string");
    }
    // return value
    *out = result;
    return e;
}
derr_t dstr_tou64(const dstr_t* in, uint64_t* out, int base){
    derr_t e = E_OK;
    // copy the version into a null-terminated string to read into atof()
    DSTR_VAR(temp, 128);
    derr_t e2 = dstr_copy(in, &temp);
    CATCH(e2, E_ANY){
        RETHROW(&e, &e2, E_PARAM);
    }
    e2 = dstr_null_terminate(&temp);
    CATCH(e2, E_ANY){
        RETHROW(&e, &e2, E_PARAM);
    }
    // now parse
    char* endptr;
    errno = 0;
    uintmax_t result = strtoumax(temp.data, &endptr, base);
    // check for error
    if(errno){
        TRACE(&e, "srtoull(%x): %x\n", FD(in), FE(&errno));
        ORIG(&e, E_PARAM, "invalid number string");
    }
    // make sure everything was parsed
    if(endptr != &temp.data[temp.len]){
        TRACE(&e, "input was \"%x\"\n", FD(in));
        ORIG(&e, E_PARAM, "invalid number string");
    }
    // check bounds
    if(result > UINT64_MAX){
        TRACE(&e, "input was \"%x\"\n", FD(in));
        ORIG(&e, E_PARAM, "number out of range");
    }
    // return value
    *out = (uint64_t)result;
    return e;
}
derr_t dstr_tof(const dstr_t* in, float* out){
    derr_t e = E_OK;
    // copy the version into a null-terminated string to read into atof()
    DSTR_VAR(temp, 128);
    derr_t e2 = dstr_copy(in, &temp);
    CATCH(e2, E_ANY){
        RETHROW(&e, &e2, E_PARAM);
    }
    e2 = dstr_null_terminate(&temp);
    CATCH(e2, E_ANY){
        RETHROW(&e, &e2, E_PARAM);
    }
    // now parse
    char* endptr;
    errno = 0;
    float result = strtof(temp.data, &endptr);
    // check for error
    if(errno){
        TRACE(&e, "srtof(%x): %x\n", FD(in), FE(&errno));
        ORIG(&e, E_PARAM, "invalid number string");
    }
    // make sure everything was parsed
    if(endptr != &temp.data[temp.len]){
        TRACE(&e, "input was \"%x\"\n", FD(in));
        ORIG(&e, E_PARAM, "invalid number string");
    }
    // return value
    *out = result;
    return e;
}
derr_t dstr_tod(const dstr_t* in, double* out){
    derr_t e = E_OK;
    // copy the version into a null-terminated string to read into atof()
    DSTR_VAR(temp, 128);
    derr_t e2 = dstr_copy(in, &temp);
    CATCH(e2, E_ANY){
        RETHROW(&e, &e2, E_PARAM);
    }
    e2 = dstr_null_terminate(&temp);
    CATCH(e2, E_ANY){
        RETHROW(&e, &e2, E_PARAM);
    }
    // now parse
    char* endptr;
    errno = 0;
    double result = strtod(temp.data, &endptr);
    // check for error
    if(errno){
        TRACE(&e, "srtod(%x): %x\n", FD(in), FE(&errno));
        ORIG(&e, E_PARAM, "invalid number string");
    }
    // make sure everything was parsed
    if(endptr != &temp.data[temp.len]){
        TRACE(&e, "input was \"%x\"\n", FD(in));
        ORIG(&e, E_PARAM, "invalid number string");
    }
    // return value
    *out = result;
    return e;
}
derr_t dstr_told(const dstr_t* in, long double* out){
    derr_t e = E_OK;
    // copy the version into a null-terminated string to read into atof()
    DSTR_VAR(temp, 128);
    derr_t e2 = dstr_copy(in, &temp);
    CATCH(e2, E_ANY){
        RETHROW(&e, &e2, E_PARAM);
    }
    e2 = dstr_null_terminate(&temp);
    CATCH(e2, E_ANY){
        RETHROW(&e, &e2, E_PARAM);
    }
    // now parse
    char* endptr;
    errno = 0;
    long double result = strtold(temp.data, &endptr);
    // check for error
    if(errno){
        TRACE(&e, "srtod(%x): %x\n", FD(in), FE(&errno));
        ORIG(&e, E_PARAM, "invalid number string");
    }
    // make sure everything was parsed
    if(endptr != &temp.data[temp.len]){
        TRACE(&e, "input was \"%x\"\n", FD(in));
        ORIG(&e, E_PARAM, "invalid number string");
    }
    // return value
    *out = result;
    return e;
}
derr_t dstr_tosize(const dstr_t* in, size_t* out, int base){
    derr_t e = E_OK;
    // first copy into a unsigned long long
    unsigned long long temp;
    PROP(&e, dstr_toull(in, &temp, base) );

#if ULLONG_MAX > SIZE_MAX
    if(temp > SIZE_MAX){
        TRACE(&e, "input was \"%x\"\n", FD(in));
        ORIG(&e, E_PARAM, "number string too large for size_t");
    }
#endif

    *out = (size_t)temp;
    return e;
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
                             bool ignore_partial,
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
        if(ignore_partial == true){
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
                          bool ignore_partial,
                          size_t stop_str_index,
                          bool* found_stop){
    derr_t e = E_OK;
    size_t consumed;
    PROP(&e, do_dstr_recode(in, out, search_strs, replace_strs, ignore_partial,
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
    PROP(&e, do_dstr_recode(in, out, search_strs, replace_strs, false, 0, NULL,
                         NULL) );
    return e;
}

static size_t do_dstr_count2(
    const dstr_t text, const dstr_t pattern, bool sensitive
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
            count++;
            // no need to search anymore until end of current pattern
            // (compensating for the fact that the loop adds 1 to i)
            i += pattern.len - 1;
        }
    }
    return count;
}

size_t dstr_count(const dstr_t* text, const dstr_t* pattern){
    return do_dstr_count2(*text, *pattern, true);
}

size_t dstr_count2(const dstr_t text, const dstr_t pattern){
    return do_dstr_count2(text, pattern, true);
}

size_t dstr_icount2(const dstr_t text, const dstr_t pattern){
    return do_dstr_count2(text, pattern, false);
}

bool dstr_beginswith(const dstr_t *str, const dstr_t *pattern){
    if(!pattern->len) return true;
    if(str->len < pattern->len) return false;
    dstr_t sub = dstr_sub(str, 0, pattern->len);
    return dstr_cmp(pattern, &sub) == 0;
}

bool dstr_endswith(const dstr_t *str, const dstr_t *pattern){
    if(!pattern->len) return true;
    if(str->len < pattern->len) return false;
    dstr_t sub = dstr_sub(str, str->len - pattern->len, str->len);
    return dstr_cmp(pattern, &sub) == 0;
}

derr_type_t dstr_grow_quiet(dstr_t *ds, size_t min_size){
    if(ds->size < min_size){
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
    }
    return E_NONE;
}
derr_t dstr_grow(dstr_t* ds, size_t min_size){
    derr_t e = E_OK;
    derr_type_t type = dstr_grow_quiet(ds, min_size);
    if(type) ORIG(&e, type, "unable to grow dstr");
    return e;
}

// append one dstr to another
derr_type_t dstr_append_quiet(dstr_t *dstr, const dstr_t *new_text){
    derr_type_t type;
    type = dstr_grow_quiet(dstr, dstr->len + new_text->len);
    if(type) return type;

    memcpy(dstr->data + dstr->len, new_text->data, new_text->len);
    dstr->len += new_text->len;
    return E_NONE;
}
derr_t dstr_append(dstr_t* dstr, const dstr_t* new_text){
    derr_t e = E_OK;
    derr_type_t type = dstr_append_quiet(dstr, new_text);
    if(type) ORIG(&e, type, "unable to append to dstr");
    return e;
}

// copy one dstr to another
derr_t dstr_copy(const dstr_t* in, dstr_t* out){
    derr_t e = E_OK;
    PROP(&e, dstr_grow(out, in->len) );

    memcpy(out->data, in->data, in->len);
    out->len = in->len;

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
    derr_type_t type = dstr_grow_quiet(ds, ds->len + 1);
    if(type) return type;

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
        TRACE(&e, "%x: %x\n", FS("read"), FE(&errno));
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
            TRACE(&e, "%x: %x\n", FS("write"), FE(&errno));
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
        TRACE(&e, "%x: %x\n", FS("fread"), FE(&errno));
        ORIG(&e, E_OS, "error in fread");
    }
    if(amnt_read) *amnt_read = ar;
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

///////////////////////////////
// FMT()-related stuff below //
///////////////////////////////

/* fmt_dstr_append_quiet is just like dstr_append_quiet except when it fails
   due to a size limit it will try to fill the buffer as much as possible
   before returning the error */
derr_type_t fmt_dstr_append_quiet(dstr_t *dstr, const dstr_t *new_text){
    derr_type_t type = dstr_append_quiet(dstr, new_text);
    if(type != E_NONE && dstr->len < dstr->size){
        dstr_t sub_text = dstr_sub(new_text, 0, dstr->size - dstr->len);
        dstr_append_quiet(dstr, &sub_text);
    }
    return type;
}

static inline derr_type_t dstr_append_hex(dstr_t* dstr, unsigned char val){
    DSTR_VAR(buffer, 8);
    int len = snprintf(buffer.data, buffer.size, "%.2x", val);
    if(len < 0) return E_INTERNAL;
    buffer.len = (size_t)len;
    return fmt_dstr_append_quiet(dstr, &buffer);
}
static inline derr_type_t dstr_append_char(dstr_t* dstr, char val){
    derr_type_t type = dstr_grow_quiet(dstr, dstr->len+1);
    if(type) return type;
    dstr->data[dstr->len++] = val;
    return E_NONE;
}

#define SNPRINTF_WITH_RETRY(fmtstr, arg) \
    ret = snprintf(out->data + out->len, out->size - out->len, fmtstr, arg); \
    if(ret < 0) return E_INTERNAL; \
    sret = (size_t) ret; \
    if(sret + 1 > out->size - out->len){ \
        derr_type_t type = dstr_grow_quiet(out, out->len + sret + 1); \
        if(type){ \
            /* allow the partially-written stuff to show */ \
            out->len = out->size; \
            return type; \
        } \
        snprintf(out->data + out->len, out->size - out->len, fmtstr, arg); \
    } \
    out->len += sret

static inline derr_type_t fmt_arg(dstr_t* out, fmt_t arg){
    int ret;
    size_t sret;
    switch(arg.type){
        case FMT_UINT: SNPRINTF_WITH_RETRY("%ju", arg.data.u); break;
        case FMT_INT: SNPRINTF_WITH_RETRY("%jd", arg.data.i); break;
        case FMT_FLOAT: SNPRINTF_WITH_RETRY("%Lf", arg.data.f); break;
        case FMT_CHAR: SNPRINTF_WITH_RETRY("%c", arg.data.c); break;
        case FMT_CSTR: SNPRINTF_WITH_RETRY("%s", arg.data.cstr); break;
        case FMT_PTR: SNPRINTF_WITH_RETRY("%p", arg.data.ptr); break;
        case FMT_DSTR:
            return fmt_dstr_append_quiet(out, arg.data.dstr);
        case FMT_BOOL:
            return fmt_dstr_append_quiet(
                out, arg.data.boolean ? &DSTR_LIT("true") : &DSTR_LIT("false")
            );
        case FMT_EXT:
            return arg.data.ext.hook(out, arg.data.ext.arg);
        case FMT_EXT_NOCONST:
            return arg.data.ext_noconst.hook(out, arg.data.ext_noconst.arg);
    }
    return E_NONE;
}

derr_type_t pvt_fmt_quiet(
        dstr_t* out, const char* fstr, const fmt_t* args, size_t nargs){
    derr_type_t type;
    // how far into the list of args we are
    size_t idx = 0;
    // first parse through the fmt string looking for %
    const char *c = fstr;
    while(*c){
        if(*c != '%'){
            // copy this character over
            type = dstr_append_char(out, *c);
            if(type) return type;
            c += 1;
            continue;
        }
        // if we got a '%', check for the %x or %% patterns
        const char* cc = c + 1;
        if(*cc == 0){
            // oops, end of string, dump the '%'
            type = dstr_append_char(out, *c);
            if(type) return type;
            break;
        }
        if(*cc == '%'){
            // copy a literal '%' over
            type = dstr_append_char(out, '%');
            if(type) return type;
            c += 2;
            continue;
        }
        if(*cc != 'x'){
            // copy both characters over
            type = dstr_append_char(out, *c);
            if(type) return type;
            type = dstr_append_char(out, *cc);
            if(type) return type;
            c += 2;
            continue;
        }
        c += 2;
        // if it is "%x" dump another arg, unless we are already out of args
        if(idx >= nargs) continue;
        type = fmt_arg(out, args[idx++]);
        if(type) return type;
    }
    // now just print space-delineated arguments till we run out
    while(idx < nargs){
        type = dstr_append_char(out, ' ');
        if(type) return type;
        type = fmt_arg(out, args[idx++]);
        if(type) return type;
    }
    // always null terminate
    return dstr_null_terminate_quiet(out);
}
derr_t pvt_fmt(dstr_t* out, const char* fstr, const fmt_t* args, size_t nargs){
    derr_t e = E_OK;
    derr_type_t type = pvt_fmt_quiet(out, fstr, args, nargs);
    if(type) ORIG(&e, type, "unable to format string");
    return e;
}

derr_type_t pvt_ffmt_quiet(
        FILE* f, size_t* written, const char* fstr, const fmt_t* args,
        size_t nargs){
    /* For now, just buffer the entire string into memory and then dump it.
       This is not ideal for writing large formatted strings to a file, but
       frankly this is probably not the right function for that anyway. */
    dstr_t buffer;
    derr_type_t type = dstr_new_quiet(&buffer, 1024);
    if(type) return type;

    type = pvt_fmt_quiet(&buffer, fstr, args, nargs);
    if(type) goto cu;

    type = dstr_fwrite_quiet(f, &buffer);
    if(written) *written = buffer.len;

cu:
    dstr_free(&buffer);
    return type;
}
derr_t pvt_ffmt(
        FILE* f, size_t* written, const char* fstr, const fmt_t* args,
        size_t nargs){
    derr_t e = E_OK;
    derr_type_t type = pvt_ffmt_quiet(f, written, fstr, args, nargs);
    if(type) ORIG(&e, type, "unable to format string for FILE pointer");
    return e;
}

derr_type_t fmthook_dstr_dbg(dstr_t* out, const void* arg){
    // cast the input
    const dstr_t* in = (const dstr_t*)arg;
    // some useful constants
    DSTR_STATIC(cr,"\\r");
    DSTR_STATIC(nl,"\\n");
    DSTR_STATIC(nc,"\\0");
    DSTR_STATIC(bs,"\\\\");
    DSTR_STATIC(pre,"\\x");
    DSTR_STATIC(tab,"\\t");
    DSTR_STATIC(quote,"\\\"");

    derr_type_t type;
    for(size_t i = 0; i < in->len; i++){
        char c = in->data[i];
        unsigned char u = (unsigned char)in->data[i];
        if     (c == '\r') type = fmt_dstr_append_quiet(out, &cr);
        else if(c == '\n') type = fmt_dstr_append_quiet(out, &nl);
        else if(c == '\0') type = fmt_dstr_append_quiet(out, &nc);
        else if(c == '\t') type = fmt_dstr_append_quiet(out, &tab);
        else if(c == '\\') type = fmt_dstr_append_quiet(out, &bs);
        else if(c == '"') type = fmt_dstr_append_quiet(out, &quote);
        else if(u > 31 && u < 128) type = dstr_append_char(out, c);
        else {
            type = fmt_dstr_append_quiet(out, &pre);
            if(type) return type;
            type = dstr_append_hex(out, u);
        }
        if(type) return type;
    }
    return E_NONE;
}

derr_type_t fmthook_strerror(dstr_t* out, const void* arg){
    // cast the input
    const int* err = (const int*)arg;
    // we'll just assume that 512 characters is quite long enough
    DSTR_VAR(temp, 512);
    compat_strerror_r(*err, temp.data, temp.size);
    temp.len = strnlen(temp.data, temp.size);
    return fmt_dstr_append_quiet(out, &temp);
}

derr_type_t fmthook_bin2hex(dstr_t* out, const void* arg){
    // cast the inpu
    const dstr_t* bin = arg;
    return bin2hex_quiet(bin, out);
}

////////////////////////////////
// String Builder stuff below //
////////////////////////////////

static derr_type_t sb_append_to_dstr_quiet(
        const string_builder_t* sb, const dstr_t* joiner, dstr_t* out){
    derr_type_t type;
    if(sb == NULL) return E_NONE;
    if(sb->prev != NULL){
        type = sb_append_to_dstr_quiet(sb->prev, joiner, out);
        if(type) return type;
        if(joiner){
            type = dstr_append_quiet(out, joiner);
            if(type) return type;
        }
    }
    type = fmt_arg(out, sb->elem);
    if(type) return type;
    if(sb->next != NULL){
        if(joiner){
            type = dstr_append_quiet(out, joiner);
            if(type) return type;
        }
        type = sb_append_to_dstr_quiet(sb->next, joiner, out);
        if(type) return type;
    }
    return E_NONE;
}
derr_t sb_append_to_dstr(
        const string_builder_t* sb, const dstr_t* joiner, dstr_t* out){
    derr_t e = E_OK;
    derr_type_t type = sb_append_to_dstr_quiet(sb, joiner, out);
    if(type) ORIG(&e, type, "failed to append string builder to dstr");
    return e;
}

derr_t sb_to_dstr(const string_builder_t* sb, const dstr_t* joiner, dstr_t* out){
    derr_t e = E_OK;
    out->len = 0;
    PROP(&e, sb_append_to_dstr(sb, joiner, out) );
    return e;
}

derr_t sb_expand(const string_builder_t* sb, const dstr_t* joiner,
                 dstr_t* stack_dstr, dstr_t* heap_dstr, dstr_t** out){
    derr_t e = E_OK;
    // try and expand into stack_dstr
    PROP_GO(&e, sb_to_dstr(sb, joiner, stack_dstr), use_heap);
    // sb_expand is often for a path, so null-terminate it
    PROP_GO(&e, dstr_null_terminate(stack_dstr), use_heap);
    // it worked, return stack_dstr as *out
    *out = stack_dstr;
    return e;

use_heap:
    DROP_VAR(&e);
    // we will need to allocate the heap_dstr to be bigger than stack_dstr
    PROP(&e, dstr_new(heap_dstr, stack_dstr->size * 2) );
    PROP_GO(&e, sb_to_dstr(sb, joiner, heap_dstr), fail_heap);
    // sb_expand is often for a path, so null-terminate it
    PROP_GO(&e, dstr_null_terminate(heap_dstr), fail_heap);
    return e;

fail_heap:
    dstr_free(heap_dstr);
    return e;
}

derr_type_t fmthook_sb(dstr_t* out, const void* arg){
    // cast the input
    const string_builder_format_t* sbf = (const string_builder_format_t*) arg;
    // write to memory, just append everything to the output
    return sb_append_to_dstr_quiet(sbf->sb, sbf->joiner, out);
}
