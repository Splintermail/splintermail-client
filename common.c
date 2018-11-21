#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <stdint.h>
#include <ctype.h>

#include "common.h"
#include "logger.h"

#include "win_compat.h"

// static strings for the error_to_dstr function
DSTR_STATIC(derr_ok_dstr, "OK");
DSTR_STATIC(derr_unknown_dstr, "UNKNOWN_ERROR_CODE");
DSTR_STATIC(derr_io_dstr, "IOERROR");
DSTR_STATIC(derr_nomem_dstr, "NOMEM");
DSTR_STATIC(derr_sock_dstr, "SOCKERROR");
DSTR_STATIC(derr_conn_dstr, "CONNERROR");
DSTR_STATIC(derr_value_dstr, "VALUEERROR");
DSTR_STATIC(derr_fixedsize_dstr, "FIXEDSIZE");
DSTR_STATIC(derr_os_dstr, "OSERROR");
DSTR_STATIC(derr_badidx_dstr, "BADIDX");
DSTR_STATIC(derr_ssl_dstr, "SSLERROR");
DSTR_STATIC(derr_sql_dstr, "SQLERROR");
DSTR_STATIC(derr_not4me_dstr, "NOT4ME");
DSTR_STATIC(derr_open_dstr, "OPEN");
DSTR_STATIC(derr_param_dstr, "PARAM");
DSTR_STATIC(derr_internal_dstr, "INTERNAL");
DSTR_STATIC(derr_fs_dstr, "FILESYSTEM");
DSTR_STATIC(derr_response_dstr, "RESPONSE");
DSTR_STATIC(derr_nokeys_dstr, "NOKEYS");
DSTR_STATIC(derr_uv_dstr, "UVERROR");
DSTR_STATIC(derr_any_dstr, "ANY");

dstr_t* error_to_dstr(derr_t error){
    switch(error){
        case E_OK: return &derr_ok_dstr;
        case E_IO: return &derr_io_dstr;
        case E_NOMEM: return &derr_nomem_dstr;
        case E_SOCK: return &derr_sock_dstr;
        case E_CONN: return &derr_conn_dstr;
        case E_VALUE: return &derr_value_dstr;
        case E_FIXEDSIZE: return &derr_fixedsize_dstr;
        case E_OS: return &derr_os_dstr;
        case E_BADIDX: return &derr_badidx_dstr;
        case E_SSL: return &derr_ssl_dstr;
        case E_SQL: return &derr_sql_dstr;
        case E_NOT4ME: return &derr_not4me_dstr;
        case E_OPEN: return &derr_open_dstr;
        case E_PARAM: return &derr_param_dstr;
        case E_INTERNAL: return &derr_internal_dstr;
        case E_FS: return &derr_fs_dstr;
        case E_RESPONSE: return &derr_response_dstr;
        case E_NOKEYS: return &derr_nokeys_dstr;
        case E_UV: return &derr_uv_dstr;
        case E_ANY: return &derr_any_dstr;
    }
    return &derr_unknown_dstr;
}

derr_t dstr_new(dstr_t* ds, size_t size){
    // only malloc in power of 2
    size_t new_size = 2;
    while(new_size < size){
        new_size *= 2;
    }
    ds->data = (char*)malloc(new_size);
    if(!ds->data) ORIG(E_NOMEM, "unable to allocate dstr");
    ds->size = new_size;
    ds->len = 0;
    ds->fixed_size = false;
    return E_OK;
}

void dstr_free(dstr_t* ds){
    if(ds->data) {
        free(ds->data);
        ds->data = NULL;
        ds->size = 0;
        ds->len = 0;
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

int dstr_cmp(const dstr_t* a, const dstr_t* b){
    // two NULL strings are considered matching
    if(!a->data && !b->data){
        return 0;
    }
    // but one NULL and one not are not matching
    if(!a->data || !b->data){
        return a->data ? *a->data : *b->data;
    }
    // don't read past the end of either string
    size_t max = MIN(a->len, b->len);

    for(size_t i = 0; i < max; i++){
        char ca = a->data[i];
        char cb = b->data[i];
        if(ca != cb){
            return ca - cb;
        }
    }

    // one string might be longer than the other
    if(a->len > b->len){
        int ca = (int)(a->data[b->len]);
        return ca;
    }
    if(b->len > a->len){
        int cb = (int)(b->data[a->len]);
        return -cb;
    }

    // strings match
    return 0;
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
            text->data[i] = (char)(text->data[i]+ 32);
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
    // copy the version into a null-terminated string to read into atof()
    DSTR_VAR(temp, 128);
    derr_t error = dstr_copy(in, &temp);
    CATCH(E_ANY){
        RETHROW(E_PARAM)
    }
    error = dstr_null_terminate(&temp);
    CATCH(E_ANY){
        RETHROW(E_PARAM)
    }
    // now parse
    char* endptr;
    errno = 0; long result = strtol(temp.data, &endptr, base);
    // check for error
    if(errno){
        LOG_ERROR("%x: %x\n", FS("srtol"), FE(&errno));
        ORIG(E_PARAM, "invalid number string");
    }
    // make sure everything was parsed
    if(endptr != &temp.data[temp.len]){
        ORIG(E_PARAM, "invalid number string");
    }
    // check bounds
    if(result > INT_MAX || result < INT_MIN){
        ORIG(E_PARAM, "number out of range");
    }
    // return value
    *out = (int)result;
    return E_OK;
}
derr_t dstr_tou(const dstr_t* in, unsigned int* out, int base){
    // copy the version into a null-terminated string to read into atof()
    DSTR_VAR(temp, 128);
    derr_t error = dstr_copy(in, &temp);
    CATCH(E_ANY){
        RETHROW(E_PARAM);
    }
    error = dstr_null_terminate(&temp);
    CATCH(E_ANY){
        RETHROW(E_PARAM);
    }
    // now parse
    char* endptr;
    errno = 0;
    unsigned long result = strtoul(temp.data, &endptr, base);
    // check for error
    if(errno){
        LOG_ERROR("%x: %x\n", FS("srtoul"), FE(&errno));
        ORIG(E_PARAM, "invalid number string");
    }
    // make sure everything was parsed
    if(endptr != &temp.data[temp.len]){
        ORIG(E_PARAM, "invalid number string");
    }
    // check bounds
    if(result > UINT_MAX){
        ORIG(E_PARAM, "number out of range");
    }
    // return value
    *out = (unsigned int)result; return E_OK;
}
derr_t dstr_tol(const dstr_t* in, long* out, int base){
    // copy the version into a null-terminated string to read into atof()
    DSTR_VAR(temp, 128);
    derr_t error = dstr_copy(in, &temp);
    CATCH(E_ANY){
        RETHROW(E_PARAM);
    }
    error = dstr_null_terminate(&temp);
    CATCH(E_ANY){
        RETHROW(E_PARAM);
    }
    // now parse
    char* endptr;
    errno = 0;
    long result = strtol(temp.data, &endptr, base);
    // check for error
    if(errno){
        LOG_ERROR("%x: %x\n", FS("srtol"), FE(&errno));
        ORIG(E_PARAM, "invalid number string");
    }
    // make sure everything was parsed
    if(endptr != &temp.data[temp.len]){
        ORIG(E_PARAM, "invalid number string");
    }
    // return value
    *out = result;
    return E_OK;
}
derr_t dstr_toul(const dstr_t* in, unsigned long* out, int base){
    // copy the version into a null-terminated string to read into atof()
    DSTR_VAR(temp, 128);
    derr_t error = dstr_copy(in, &temp);
    CATCH(E_ANY){
        RETHROW(E_PARAM);
    }
    error = dstr_null_terminate(&temp);
    CATCH(E_ANY){
        RETHROW(E_PARAM);
    }
    // now parse
    char* endptr;
    errno = 0;
    unsigned long result = strtoul(temp.data, &endptr, base);
    // check for error
    if(errno){
        LOG_ERROR("%x: %x\n", FS("srtoul"), FE(&errno));
        ORIG(E_PARAM, "invalid number string");
    }
    // make sure everything was parsed
    if(endptr != &temp.data[temp.len]){
        ORIG(E_PARAM, "invalid number string");
    }
    // return value
    *out = result;
    return E_OK;
}
derr_t dstr_toll(const dstr_t* in, long long* out, int base){
    // copy the version into a null-terminated string to read into atof()
    DSTR_VAR(temp, 128);
    derr_t error = dstr_copy(in, &temp);
    CATCH(E_ANY){
        RETHROW(E_PARAM);
    }
    error = dstr_null_terminate(&temp);
    CATCH(E_ANY){
        RETHROW(E_PARAM);
    }
    // now parse
    char* endptr;
    errno = 0;
    long long result = strtoll(temp.data, &endptr, base);
    // check for error
    if(errno){
        LOG_ERROR("%x: %x\n", FS("srtoll"), FE(&errno));
        ORIG(E_PARAM, "invalid number string");
    }
    // make sure everything was parsed
    if(endptr != &temp.data[temp.len]){
        ORIG(E_PARAM, "invalid number string");
    }
    // return value
    *out = result;
    return E_OK;
}
derr_t dstr_toull(const dstr_t* in, unsigned long long* out, int base){
    // copy the version into a null-terminated string to read into atof()
    DSTR_VAR(temp, 128);
    derr_t error = dstr_copy(in, &temp);
    CATCH(E_ANY){
        RETHROW(E_PARAM);
    }
    error = dstr_null_terminate(&temp);
    CATCH(E_ANY){
        RETHROW(E_PARAM);
    }
    // now parse
    char* endptr;
    errno = 0;
    unsigned long long result = strtoull(temp.data, &endptr, base);
    // check for error
    if(errno){
        LOG_ERROR("%x: %x\n", FS("srtoull"), FE(&errno));
        ORIG(E_PARAM, "invalid number string");
    }
    // make sure everything was parsed
    if(endptr != &temp.data[temp.len]){
        ORIG(E_PARAM, "invalid number string");
    }
    // return value
    *out = result;
    return E_OK;
}
derr_t dstr_tof(const dstr_t* in, float* out){
    // copy the version into a null-terminated string to read into atof()
    DSTR_VAR(temp, 128);
    derr_t error = dstr_copy(in, &temp);
    CATCH(E_ANY){
        RETHROW(E_PARAM);
    }
    error = dstr_null_terminate(&temp);
    CATCH(E_ANY){
        RETHROW(E_PARAM);
    }
    // now parse
    char* endptr;
    errno = 0;
    float result = strtof(temp.data, &endptr);
    // check for error
    if(errno){
        LOG_ERROR("%x: %x\n", FS("srtof"), FE(&errno));
        ORIG(E_PARAM, "invalid number string");
    }
    // make sure everything was parsed
    if(endptr != &temp.data[temp.len]){
        ORIG(E_PARAM, "invalid number string");
    }
    // return value
    *out = result;
    return E_OK;
}
derr_t dstr_tod(const dstr_t* in, double* out){
    // copy the version into a null-terminated string to read into atof()
    DSTR_VAR(temp, 128);
    derr_t error = dstr_copy(in, &temp);
    CATCH(E_ANY){
        RETHROW(E_PARAM);
    }
    error = dstr_null_terminate(&temp);
    CATCH(E_ANY){
        RETHROW(E_PARAM);
    }
    // now parse
    char* endptr;
    errno = 0;
    double result = strtod(temp.data, &endptr);
    // check for error
    if(errno){
        LOG_ERROR("%x: %x\n", FS("srtod"), FE(&errno));
        ORIG(E_PARAM, "invalid number string");
    }
    // make sure everything was parsed
    if(endptr != &temp.data[temp.len]){
        ORIG(E_PARAM, "invalid number string");
    }
    // return value
    *out = result;
    return E_OK;
}
derr_t dstr_told(const dstr_t* in, long double* out){
    // copy the version into a null-terminated string to read into atof()
    DSTR_VAR(temp, 128);
    derr_t error = dstr_copy(in, &temp);
    CATCH(E_ANY){
        RETHROW(E_PARAM);
    }
    error = dstr_null_terminate(&temp);
    CATCH(E_ANY){
        RETHROW(E_PARAM);
    }
    // now parse
    char* endptr;
    errno = 0;
    long double result = strtold(temp.data, &endptr);
    // check for error
    if(errno){
        LOG_ERROR("%x: %x\n", FS("srtod"), FE(&errno));
        ORIG(E_PARAM, "invalid number string");
    }
    // make sure everything was parsed
    if(endptr != &temp.data[temp.len]){
        ORIG(E_PARAM, "invalid number string");
    }
    // return value
    *out = result;
    return E_OK;
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
                PROP( dstr_append(out, &sub) );
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
                PROP( dstr_append(out, &sub) );
            }
            // then append the replacement for the pattern
            PROP( dstr_append(out, &replace_strs->data[which_pattern]) );
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
    return E_OK;
}

// primary difference here is *in is not const
derr_t dstr_recode_stream(dstr_t* in,
                          dstr_t* out,
                          const LIST(dstr_t)* search_strs,
                          const LIST(dstr_t)* replace_strs,
                          bool ignore_partial,
                          size_t stop_str_index,
                          bool* found_stop){
    size_t consumed;
    PROP( do_dstr_recode(in, out, search_strs, replace_strs, ignore_partial,
                         stop_str_index, found_stop, &consumed) );
    dstr_leftshift(in, consumed);
    return E_OK;
}

// primary difference here is *in IS const
derr_t dstr_recode(const dstr_t* in,
                   dstr_t* out,
                   const LIST(dstr_t)* search_strs,
                   const LIST(dstr_t)* replace_strs,
                   bool append){
    if(append == false){
        out->len = 0;
    }
    PROP( do_dstr_recode(in, out, search_strs, replace_strs, false, 0, NULL,
                         NULL) );
    return E_OK;
}

size_t dstr_count(const dstr_t* text, const dstr_t* pattern){
    size_t count = 0;
    for(size_t i = 0; i + pattern->len <= text->len; i++){
        // we know from loop boundary there's always room for a full match
        // so now we can safely just check the whole pattern
        bool match = true;
        for(size_t j = 0; j < pattern->len; j++){
            if(text->data[i + j] != pattern->data[j]){
                // nope, not a match.  Continue search from i
                match = false;
                break;
            }
        }
        if(match == true){
            count++;
            // no need to search anymore until end of current pattern
            // (compensating for the fact that the loop adds 1 to i)
            i += pattern->len - 1;
        }
    }
    return count;
}

derr_t dstr_grow(dstr_t* ds, size_t min_size){
    if(ds->size < min_size){
        // we can't realloc if out is fixed-size
        if(ds->fixed_size){
            ORIG(E_FIXEDSIZE, "can't realloc a fixed-size dstr");
        }
        // upgrade buffer size by powers of 2
        size_t newsize = MAX(ds->size, 2);
        while(newsize < min_size){
            newsize *= 2;
        }
        void* new = realloc(ds->data, newsize);
        if(!new) ORIG(E_NOMEM, "unable to realloc dstr for grow");
        ds->size = newsize;
        ds->data = new;
    }
    return E_OK;
}

// append one dstr to another
derr_t dstr_append(dstr_t* dstr, const dstr_t* new_text){
    PROP( dstr_grow(dstr, dstr->len + new_text->len) );

    memcpy(dstr->data + dstr->len, new_text->data, new_text->len);
    dstr->len += new_text->len;

    return E_OK;
}

// copy one dstr to another
derr_t dstr_copy(const dstr_t* in, dstr_t* out){
    PROP( dstr_grow(out, in->len) );

    memcpy(out->data, in->data, in->len);
    out->len = in->len;

    return E_OK;
}

// the LIST(dstr_t) returned will have its elements point into the dstr_t text
// the LIST(dstr_t) should be allocated before this function
derr_t dstr_split(const dstr_t* text, const dstr_t* pattern, LIST(dstr_t)* out){
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
        if(word_end == 0){
            /* in this special case, we manually add an empty string, because
               dstr_sub will interpret the 0 specially */
            dstr_t new;
            new.data = text->data;
            new.len = 0;
            new.size = 0;
            new.fixed_size = true;
            PROP( LIST_APPEND(dstr_t, out, new) );
        }else{
            dstr_t new = dstr_sub(text, word_start, word_end);
            PROP( LIST_APPEND(dstr_t, out, new) );
        }

        // get ready for the next dstr_find()
        word_start = word_end + pattern->len;
    }

    return E_OK;
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

derr_t dstr_null_terminate(dstr_t* ds){
    // make sure that the ds is long enough to null-terminate
    PROP( dstr_grow(ds, ds->len + 1) );

    // add a null-terminating character
    ds->data[ds->len] = '\0';

    return E_OK;
}

derr_t list_append_with_mem(LIST(dstr_t)* list, dstr_t* mem, dstr_t in,
                            bool null_term){
    derr_t error;
    // remember how mem started
    size_t start = mem->len;
    char* oldp = mem->data;
    /* do the block memory allocation all at once (so there's only one chance
       for reallocation) */
    size_t to_grow = in.len + (null_term ? 1 : 0);
    PROP( dstr_grow(mem, mem->len + to_grow) );
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
    PROP_GO( LIST_APPEND(dstr_t, list, sub), fail_1);
    return E_OK;

fail_1:
    // if we failed in the LIST_APPEND, remove stuff from the mem
    mem->len = start;
    return error;
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
    // read() returns a signed size_t (ssize_t)
    ssize_t ar = 0;
    if(count == 0){
        // 0 means "try to fill buffer"
        count = buffer->size - buffer->len;
        // make sure the buffer isn't full
        if(count == 0){
            ORIG(E_FIXEDSIZE, "buffer is full");
        }
    }else{
        // grow buffer to fit
        PROP( dstr_grow(buffer, buffer->len + count) );
    }
    ar = read(fd, buffer->data + buffer->len, count);
    if(ar < 0){
        LOG_ERROR("%x: %x\n", FS("read"), FE(&errno));
        ORIG(E_OS, "error in read");
    }
    buffer->len += (size_t)ar;
    if(amnt_read) *amnt_read = (size_t)ar;
    return E_OK;
}

derr_t dstr_write(int fd, const dstr_t* buffer){
    // return early if buffer is empty
    if(buffer->len == 0) return E_OK;
    // repeatedly try to write until we write the whole buffer
    size_t total = 0;
    size_t zero_writes = 0;
    while(total < buffer->len){
        ssize_t amnt_written = write(fd, buffer->data + total, buffer->len - total);
        // writing zero bytes is a failure mode
        if(amnt_written < 0){
            LOG_ERROR("%x: %x\n", FS("write"), FE(&errno));
            ORIG(E_OS, "dstr_write failed");
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
                ORIG(E_OS, "too many zero writes");
            }
        }
        total += (size_t)amnt_written;
    }

    return E_OK;
}

derr_t dstr_fread(FILE* f, dstr_t* buffer, size_t count, size_t* amnt_read){
    size_t ar = 0;

    if(count == 0){
        // 0 means "try to fill buffer"
        count = buffer->size - buffer->len;
    }else{
        // grow buffer to fit
        PROP( dstr_grow(buffer, buffer->len + count) );
    }
    // make sure the buffer isn't full
    if(count == 0){
        ORIG(E_FIXEDSIZE, "buffer is full");
    }
    ar = fread(buffer->data + buffer->len, 1, count, f);
    if(ar > 0){
        buffer->len += ar;
    }else if(ferror(f)){
        LOG_ERROR("%x: %x\n", FS("fread"), FE(&errno));
        ORIG(E_OS, "error in fread");
    }
    if(amnt_read) *amnt_read = ar;
    return E_OK;
}

derr_t dstr_fwrite(FILE* f, const dstr_t* buffer){
    /* NOTE: this function does not use the error-handling macros, because is
       actually *called* in the error-handling macros, and any error results in
       an immediate recurse-until-segfault operation. */

    // return early if buffer is empty
    if(buffer->len == 0) return E_OK;

    size_t amnt_written = fwrite(buffer->data, 1, buffer->len, f);
    if(amnt_written < buffer->len){
        // we are in an error condition of some sort
        // because this function is called in error-handling macros,
        // we can't use error-handling macros or it quicky leads to problems
        return E_OS;
    }

    return E_OK;
}

derr_t dstr_read_file(const char* filename, dstr_t* buffer){
    derr_t error;
    int fd = open(filename, O_RDONLY);
    if(fd < 0){
        LOG_ERROR("%x: %x\n", FS(filename), FE(&errno));
        ORIG(E_OPEN, "unable to open file");
    }
    while(true){
        size_t amnt_read;
        PROP_GO( dstr_read(fd, buffer, 0, &amnt_read), cleanup);
        // if we read nothing then we're done
        if(amnt_read == 0){
            break;
        }
        /* if we filled the buffer with text we should force the buffer to
           reallocate and try again */
        if(buffer->len == buffer->size){
            PROP_GO( dstr_grow(buffer, buffer->size * 2), cleanup);
        }
    }
cleanup:
    close(fd);
    return error;
}

derr_t dstr_write_file(const char* filename, const dstr_t* buffer){
    derr_t error;
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if(fd < 0){
        LOG_ERROR("%x: %x\n", FS(filename), FE(&errno));
        ORIG(E_OPEN, "unable to open file");
    }
    PROP_GO( dstr_write(fd, buffer), cleanup);
cleanup:
    close(fd);
    return error;
}

derr_t dstr_fread_file(const char* filename, dstr_t* buffer){
    derr_t error;
    FILE* f = fopen(filename, "r");
    if(!f){
        LOG_ERROR("%x: %x\n", FS(filename), FE(&errno));
        ORIG(errno == ENOMEM ? E_NOMEM : E_OPEN, "unable to open file");
    }
    while(true){
        size_t amnt_read;
        PROP_GO( dstr_fread(f, buffer, 0, &amnt_read), cleanup);
        // if we read nothing then we're done
        if(amnt_read == 0){
            break;
        }
        /* if we filled the buffer with text we should force the buffer to
           reallocate and try again */
        if(buffer->len == buffer->size){
            PROP_GO( dstr_grow(buffer, buffer->size * 2), cleanup);
        }
    }
cleanup:
    fclose(f);
    return error;
}

derr_t dstr_fwrite_file(const char* filename, const dstr_t* buffer){
    derr_t error;
    FILE* f = fopen(filename, "w");
    if(!f){
        LOG_ERROR("%x: %x\n", FS(filename), FE(&errno));
        ORIG(errno == ENOMEM ? E_NOMEM : E_OPEN, "unable to open file");
    }
    PROP_GO( dstr_fwrite(f, buffer), cleanup);
cleanup:
    fclose(f);
    return error;
}

derr_t bin2b64(dstr_t* bin, dstr_t* b64, size_t line_width, bool force_end){
    unsigned char ch[3];
    memset(ch, 0, sizeof(ch));
    size_t ch_idx = 0;

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
            PROP( dstr_append(b64, &buff) );
            // reset chunk
            memset(ch, 0, sizeof(ch));
            ch_idx = 0;
            // check to append line break
            if(line_width > 0){
                if((i + 1) % chunk_size == 0 || i + 1 == bin->len){
                    PROP( dstr_append(b64, &line_break) );
                }
            }
        }
    }
    dstr_leftshift(bin, i);
    return E_OK;
}

derr_t b642bin(dstr_t* b64, dstr_t* bin){
    // build chunks of b64 characters to decode all-at-once
    unsigned char ch[4];
    memset(ch, 0, sizeof(ch));
    int ch_idx = 0;
    int skip = 0;
    size_t total_read = 0;

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
            PROP( dstr_append(bin, &buffer) );
            ch_idx = 0;
            total_read = i + 1;
            if(skip > 0) break;
        }
    }
    dstr_leftshift(b64, total_read);
    return E_OK;
}

derr_t bin2hex(const dstr_t* bin, dstr_t* hex){
    DSTR_VAR(temp, 3);
    temp.len = 2;
    for(size_t i = 0; i < bin->len; i++){
        int ret = snprintf(temp.data, temp.size, "%.2x", (unsigned char)bin->data[i]);
        if(ret != 2){
            ORIG(E_INTERNAL, "snprintf printed the wrong amount");
        }
        PROP( dstr_append(hex, &temp) );
    }
    return E_OK;
}

derr_t hex2bin(const dstr_t* hex, dstr_t* bin){
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
            ORIG(E_PARAM, "bad hex input");
        }
        shift = 1 - shift;
        *byte |= (unsigned char)(val << (4 * shift));
        if(!shift){
            // append byte to dstr
            PROP( dstr_append(bin, &one_char) );
            *byte = 0;
        }
    }
    return E_OK;
}

///////////////////////////////
// FMT()-related stuff below //
///////////////////////////////

static inline derr_t dstr_append_uint(dstr_t* dstr, unsigned int val){
    DSTR_VAR(buffer, 128);
    int len = snprintf(buffer.data, buffer.size, "%.3u", val);
    if(len < 0) ORIG(E_INTERNAL, "snprintf failed");
    buffer.len = (size_t)len;
    PROP( dstr_append(dstr, &buffer) );
    return E_OK;
}
static inline derr_t dstr_append_char(dstr_t* dstr, char val){
    PROP( dstr_grow(dstr, dstr->len + 1) );
    dstr->data[dstr->len++] = val;
    return E_OK;
}

#define SNPRINTF_WITH_RETRY(fmtstr, arg) \
    ret = snprintf(buf, left, fmtstr, arg); \
    if(ret < 0) ORIG(E_INTERNAL, "snprintf failed"); \
    sret = (size_t) ret; \
    if(sret + 1 > left){ \
        PROP( dstr_grow(out, out->len + sret + 1) ); \
        snprintf(buf, left, fmtstr, arg); \
    } \
    out->len += sret;

static inline derr_t fmt_arg(dstr_t* out, fmt_t arg){
    int ret;
    size_t sret;
    char* buf = out->data + out->len;
    size_t left = out->size - out->len;
    switch(arg.type){
        case FMT_UINT: SNPRINTF_WITH_RETRY("%ju", arg.data.u); break;
        case FMT_INT: SNPRINTF_WITH_RETRY("%jd", arg.data.i); break;
        case FMT_FLOAT: SNPRINTF_WITH_RETRY("%Lf", arg.data.f); break;
        case FMT_CHAR: SNPRINTF_WITH_RETRY("%c", arg.data.c); break;
        case FMT_CSTR: SNPRINTF_WITH_RETRY("%s", arg.data.cstr); break;
        case FMT_PTR: SNPRINTF_WITH_RETRY("%p", arg.data.ptr); break;
        case FMT_DSTR: PROP( dstr_append(out, arg.data.dstr) ); break;
        case FMT_EXT:
            PROP( arg.data.ext.hook(out, NULL, NULL, arg.data.ext.arg) );
            break;
    }
    return E_OK;
}

derr_t pvt_fmt(dstr_t* out, const char* fstr, const fmt_t* args, size_t nargs){
    // how far into the list of args we are, skip the dummy argument
    size_t idx = 1;
    // first parse through the fmt string looking for %
    const char *c = fstr;
    while(*c){
        if(*c != '%'){
            // copy this character over
            PROP( dstr_append_char(out, *c) );
            c += 1;
            continue;
        }
        // if we got a '%', check for the %x or %% patterns
        const char* cc = c + 1;
        if(*cc == 0){
            // oops, end of string, dump the '%'
            PROP( dstr_append_char(out, *c) );
            break;
        }
        if(*cc == '%'){
            // copy a literal '%' over
            PROP( dstr_append_char(out, '%') );
            c += 2;
            continue;
        }
        if(*cc != 'x'){
            // copy both characters over
            PROP( dstr_append_char(out, *c) );
            PROP( dstr_append_char(out, *cc) );
            c += 2;
            continue;
        }
        c += 2;
        // if it is "%x" dump another arg, unless we are already out of args
        if(idx >= nargs) continue;
        PROP( fmt_arg(out, args[idx++]) );
    }
    // now just print space-delineated arguments till we run out
    while(idx < nargs){
        PROP( fmt_arg(out, args[idx++]) );
    }
    // always null terminate
    PROP( dstr_null_terminate(out) );
    return E_OK;
}

#define FPRINTF_WITH_CHECK(fmtstr, arg) \
    ret = fprintf(f, fmtstr, arg); \
    if(ret < 0) return E_OS; \
    sret = (size_t)ret;

static inline derr_t ffmt_arg(FILE*f, size_t* written, fmt_t arg){
    // allow *written to be NULL
    size_t dummy = 0;
    if(written == NULL){
        written = &dummy;
    }
    int ret = 0;
    size_t sret = 0;
    derr_t error;
    switch(arg.type){
        case FMT_UINT: FPRINTF_WITH_CHECK("%ju", arg.data.u); break;
        case FMT_INT: FPRINTF_WITH_CHECK("%jd", arg.data.i); break;
        case FMT_FLOAT: FPRINTF_WITH_CHECK("%Lf", arg.data.f); break;
        case FMT_CHAR: FPRINTF_WITH_CHECK("%c", arg.data.c); break;
        case FMT_CSTR: FPRINTF_WITH_CHECK("%s", arg.data.cstr); break;
        case FMT_PTR: FPRINTF_WITH_CHECK("%p", arg.data.ptr); break;
        case FMT_DSTR:
            error = dstr_fwrite(f, arg.data.dstr);
            if(error) return error;
            sret = arg.data.dstr->len;
            break;
        case FMT_EXT:
            PROP( arg.data.ext.hook(NULL, f, written, arg.data.ext.arg) );
            break;
    }
    // add to *written
    *written += sret;
    return E_OK;
}

derr_t pvt_ffmt(FILE* f, size_t* written, const char* fstr, const fmt_t* args,
                size_t nargs){
    derr_t error;
    // allow *written to be NULL
    size_t dummy = 0;
    if(written == NULL){
        written = &dummy;
    }
    *written = 0;
    // how far into the list of args we are, skip the dummy argument
    size_t idx = 1;
    // first parse through the fmt string looking for %
    const char *c = fstr;
    while(*c){
        if(*c != '%'){
            // write this character out
            if( fputc(*c, f) == EOF) return E_OS; else *written += 1;
            c++;
            continue;
        }
        // if we got a '%', check for the %x pattern
        const char* cc = c + 1;
        if(*cc == 0){
            // oops, end of string, dump the '%'
            if( fputc(*c, f) == EOF) return E_OS; else *written += 1;
            break;
        }
        if(*cc != 'x'){
            // copy both characters over
            if( fputc(*c, f) == EOF) return E_OS; else *written += 1;
            if( fputc(*cc, f) == EOF) return E_OS; else *written += 1;
            c += 2;
            continue;
        }
        c += 2;
        // if it is "%x" dump another arg, unless we are already out of args
        if(idx >= nargs) continue;
        error = ffmt_arg(f, written, args[idx++]);
        if(error) return error;
    }
    // now just print space-delineated arguments till we run out
    while(idx < nargs){
        if( fputc(' ', f) == EOF) return E_OS; else *written += 1;
        error = ffmt_arg(f, written, args[idx++]);
        if(error) return error;
    }
    return E_OK;
}

/* the inputs define our behavior.  Possiblities are:
       out != NULL, f == NULL, written == NULL
           this is a append-to-dstr operation.  Ignore *written.
       out == NULL, f != NULL, written != NULL
           write to FILE*, set *written = (bytes written)
*/
derr_t fmthook_dstr_dbg(dstr_t* out, FILE* f, size_t* written, const void* arg){
    // cast the input
    const dstr_t* in = (const dstr_t*)arg;
    // some useful constants
    DSTR_STATIC(cr,"\\r");
    DSTR_STATIC(nl,"\\n");
    DSTR_STATIC(nc,"\\0");
    DSTR_STATIC(bs,"\\\\");
    DSTR_STATIC(pre,"\\x");
    DSTR_STATIC(tab,"\\t");
    if(out != NULL){
        // write to memory
        for(size_t i = 0; i < in->len; i++){
            char c = in->data[i];
            unsigned char u = (unsigned char)in->data[i];
            if     (c == '\r') PROP( dstr_append(out, &cr) )
            else if(c == '\n') PROP( dstr_append(out, &nl) )
            else if(c == '\0') PROP( dstr_append(out, &nc) )
            else if(c == '\t') PROP( dstr_append(out, &tab) )
            else if(c == '\\') PROP( dstr_append(out, &bs) )
            else if(u > 31 && u < 128) PROP( dstr_append_char(out, c) )
            else {
                PROP( dstr_append(out, &pre) );
                PROP( dstr_append_uint(out, u) );
            }
        }
    }else{
        // write to FILE*, can't use error macros
        for(size_t i = 0; i < in->len; i++){
            derr_t error;
            char c = in->data[i];
            unsigned char u = (unsigned char)in->data[i];
            if (c == '\r'){
                if((error = dstr_fwrite(f, &cr) ))
                    return error;
                else *written += 2;
            }else if(c == '\n'){
                if((error = dstr_fwrite(f, &nl) ))
                    return error;
                else *written += 2;
            }else if(c == '\0'){
                if((error = dstr_fwrite(f, &nc) ))
                    return error;
                else *written += 2;
            }else if(c == '\t'){
                if((error = dstr_fwrite(f, &tab)))
                    return error;
                else *written += 2;
            }else if(c == '\\'){
                if((error = dstr_fwrite(f, &bs) ))
                    return error;
                else *written += 2;
            }else if(u > 31 && u < 128){
                if(fputc(c, f) == EOF){
                    return E_OS;
                }
            }else{
                if((error = dstr_fwrite(f, &pre))) return error;
                *written += pre.len;
                DSTR_VAR(temp, 128);
                if((error = dstr_append_uint(&temp, u))) return error;
                if((error = dstr_fwrite(f, &temp))) return error;
                *written += temp.len;
            }
        }
    }
    return E_OK;
}

derr_t fmthook_strerror(dstr_t* out, FILE* f, size_t* written, const void* arg){
    // cast the input
    const int* err = (const int*)arg;
    /* a double copy makes the code way simpler, and is also required for
       Windows' shitty strerror_s implementation.  Also required no matter what
       for printing to a FILE* */
    // we'll just assume that 512 characters is quite long enough
    DSTR_VAR(temp, 512);
    strerror_r(*err, temp.data, temp.size);
    temp.len = strnlen(temp.data, temp.size);
    if(out != NULL){
        // write to memory, just append the error string to the output
        PROP( dstr_append(out, &temp) );
    }else{
        // write to FILE*, can't use error macros
        derr_t error;
        if((error = dstr_fwrite(f, &temp))) return error;
        written += temp.len;
    }
    return E_OK;
}

////////////////////////////////
// String Builder stuff below //
////////////////////////////////

derr_t sb_append_to_dstr(const string_builder_t* sb, const dstr_t* joiner, dstr_t* out){
    if(sb == NULL) return E_OK;
    if(sb->prev != NULL){
        PROP( sb_append_to_dstr(sb->prev, joiner, out) );
        if(joiner){
            PROP( dstr_append(out, joiner) );
        }
    }
    PROP( fmt_arg(out, sb->elem) );
    if(sb->next != NULL){
        if(joiner){
            PROP( dstr_append(out, joiner) );
        }
        PROP( sb_append_to_dstr(sb->next, joiner, out) );
    }
    return E_OK;
}

derr_t sb_to_dstr(const string_builder_t* sb, const dstr_t* joiner, dstr_t* out){
    out->len = 0;
    PROP( sb_append_to_dstr(sb, joiner, out) );
    return E_OK;
}

derr_t sb_fwrite(FILE* f, size_t* written, const string_builder_t* sb,
                 const dstr_t* joiner){
    if(sb == NULL) return E_OK;
    if(sb->prev != NULL){
        PROP( sb_fwrite(f, written, sb->prev, joiner) );
        if(joiner){
            PROP( dstr_fwrite(f, joiner) );
            *written += joiner->len;
        }
    }
    PROP( ffmt_arg(f, written, sb->elem) );
    if(sb->next != NULL){
        if(joiner){
            PROP( dstr_fwrite(f, joiner) );
            *written += joiner->len;
        }
        PROP( sb_fwrite(f, written, sb->prev, joiner) );
    }
    return E_OK;
}

derr_t sb_expand(const string_builder_t* sb, const dstr_t* joiner,
                 dstr_t* stack_dstr, dstr_t* heap_dstr, dstr_t** out){
    derr_t error = sb_to_dstr(sb, joiner, stack_dstr);
    CATCH(E_FIXEDSIZE){
        // we will need to allocate the heap_dstr to be bigger than stack_dstr
        PROP( dstr_new(heap_dstr, stack_dstr->size * 2) );
        PROP_GO( sb_to_dstr(sb, joiner, heap_dstr), fail);
        // path is contained in heap_dstr
        *out = heap_dstr;
    }else{
        // catch any other errors
        PROP(error);
        // if we had no errors, path is contained in stack_dstr
        *out = stack_dstr;
    }
    // this is often used for a path, so we're gonna need to null-terminate it
    PROP( dstr_null_terminate(*out) );
    return E_OK;

fail:
    dstr_free(heap_dstr);
    return error;
}

derr_t fmthook_sb(dstr_t* out, FILE* f, size_t* written, const void* arg){
    // cast the input
    const string_builder_format_t* sbf = (const string_builder_format_t*) arg;
    if(out != NULL){
        // write to memory, just append everything to the output
        PROP( sb_append_to_dstr(sbf->sb, sbf->joiner, out) );
    }else{
        // write to FILE*, can't use error macros
        derr_t error;
        if((error = sb_fwrite(f, written, sbf->sb, sbf->joiner))) return error;
    }
    return E_OK;
}
