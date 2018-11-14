#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#undef bool
typedef _Bool bool;

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define ABS(a)   ((a) > 0 ? (a) : (-(a)))

typedef enum {              // GENERAL DEFINITIONS:
    E_OK        = 0,        // no error
    E_IO        = 1 << 0,   // delete this
    E_NOMEM     = 1 << 1,   // some memory allocation failed
    E_SOCK      = 1 << 2,   // error in socket(), bind(), listen(), or accept()
    E_CONN      = 1 << 3,   // error in connect() or send() or recv()
    E_VALUE     = 1 << 4,   // delete this
    E_FIXEDSIZE = 1 << 5,   // an operation would result in a buffer overflow
    E_OS        = 1 << 6,   // some system call failed
    E_BADIDX    = 1 << 7,   // delete this
    E_SSL       = 1 << 8,   // an encryption-related error
    E_SQL       = 1 << 9,   // an error in the SQL library
    E_NOT4ME    = 1 << 10,  // decryption failed due to missing key
    E_OPEN      = 1 << 11,  // an error in open() # TODO: replace with E_FS
    E_PARAM     = 1 << 12,  // invalid input parameter
    E_INTERNAL  = 1 << 13,  // a never-should-happen failure occured
    E_FS        = 1 << 14,  // a file system-related error
    E_RESPONSE  = 1 << 15,  // invalid response from something external
    E_NOKEYS    = 1 << 16,  // user has no keys, a non-critical error in encrypt_msg.c
    E_UV        = 1 << 17,  // an unidentified error from libuv
    E_ANY       = ~0, // for catching errors, never throw this error
} derr_t;

typedef struct {
    char* data;
    size_t size;
    size_t len;
    bool fixed_size;
} dstr_t;

// constant version
typedef struct {
    const char* data;
    size_t size;
    size_t len;
    bool fixed_size;
} dstr_const_t;

dstr_t* error_to_dstr(derr_t error);

// wrap an empty char[] with in a dstr_t
#define DSTR_WRAP_ARRAY(dstr, buffer){ \
    dstr.data = buffer; \
    dstr.size = sizeof(buffer); \
    dstr.len = 0; \
    dstr.fixed_size = 1; \
}

// wrap a char* of known length in a dstr_t
#define DSTR_WRAP(dstr, buffer, length, null_terminated){ \
    dstr.data = buffer; \
    dstr.size = length + null_terminated; \
    dstr.len = length; \
    dstr.fixed_size = 1; \
}

// declares an empty, fixed-size dstr_t on the stack
#define DSTR_VAR(name, size) char name ## _buffer[size]; \
                             dstr_t name = { name ## _buffer, size, 0, 1 }

// this wraps a static string into a static, fixed-sized dstr_t
#define DSTR_STATIC(name, string) \
    static char name ## _buffer[] = string; \
    static dstr_t name = {name ## _buffer, \
                          sizeof(name ## _buffer), \
                          sizeof(name ## _buffer) - 1, \
                          1}

// like DSTR_STATIC just not static
#define DSTR_PRESET(name, string) \
     char name ## _buffer[] = string; \
     dstr_t name = {name ## _buffer, \
                    sizeof(name ## _buffer), \
                    sizeof(name ## _buffer) - 1, \
                    1}

// for right-hand side of declaration/definitions
#define DSTR_LIT(str) ((dstr_t){.data=str, \
                                .size=sizeof(str), \
                                .len=sizeof(str) - 1, \
                                .fixed_size=true})

// this allocates a new dstr on the heap
derr_t dstr_new(dstr_t* ds, size_t size);
/*  throws : E_NOMEM */

void dstr_free(dstr_t* ds);

// function "templates"
#define LIST(type) list_ ## type
#define LIST_NEW(type, list, num_items) list_ ## type ## _new(list, num_items)
/*  throws : E_NOMEM */

#define LIST_GROW(type, list, num_items) list_ ## type ## _grow(list, num_items)
/*  throws : E_NOMEM
             E_FIXEDSIZE */

#define LIST_APPEND(type, list, elem) list_ ## type ## _append(list, elem)
/*  throws : E_NOMEM
             E_FIXEDSIZE */

#define LIST_DELETE(type, list, idx) list_ ## type ## _delete(list, idx)
#define LIST_FREE(type, list) list_ ## type ## _free(list)

// various useful ways to declare (and initilize) variables
#define LIST_VAR(type, name, num_items) \
            type name ## _buffer[num_items]; \
            LIST(type) name = {name ## _buffer, \
                               sizeof(name ## _buffer), \
                               0, \
                               true}

#define LIST_STATIC(type, name, ...) \
            static type name ## _buffer[] = {__VA_ARGS__}; \
            static LIST(type) name = {name ## _buffer, \
                                      sizeof(name ## _buffer), \
                                      sizeof(name ## _buffer) / sizeof(type), \
                                      true}

#define LIST_PRESET(type, name, ...) \
            type name ## _buffer[] = {__VA_ARGS__}; \
            LIST(type) name = {name ## _buffer, \
                               sizeof(name ## _buffer), \
                               sizeof(name ## _buffer) / sizeof(type), \
                               true}

#define LIST_HEADERS(type) \
typedef struct { \
    type* data; \
    /* size is in bytes */ \
    size_t size; \
    /* len is number of elements */ \
    size_t len; \
    bool fixed_size; \
} LIST(type); \
derr_t list_ ## type ## _new(LIST(type)* list, size_t num_items); \
derr_t list_ ## type ## _grow(LIST(type)* list, size_t num_items); \
derr_t list_ ## type ## _append(LIST(type)* list, type element); \
void list_ ## type ## _delete(LIST(type)* list, size_t index); \
void list_ ## type ## _free(LIST(type)* list);

// wrap an empty T[] with a LIST(T) (doesn't actually need to know T)
#define LIST_WRAP_ARRAY(list, buffer){ \
    list.data = buffer; \
    list.size = sizeof(buffer); \
    list.len = 0; \
    list.fixed_size = 1; \
}

// wrap a T* of known length with a LIST(T) (doesn't actually need to know T)
#define LIST_WRAP(list, buffer, length){ \
    list.data = buffer; \
    list.size = length * sizeof(*buffer); \
    list.len = length; \
    list.fixed_size = 1; \
}


LIST_HEADERS(dstr_t)
LIST_HEADERS(size_t)
LIST_HEADERS(bool)


#define LIST_FUNCTIONS(type) \
derr_t list_ ## type ## _new(LIST(type)* list, size_t num_items){ \
    /* only malloc in power of 2 */ \
    size_t min_size = num_items * sizeof(type); \
    size_t newsize = 2; \
    while(newsize < min_size){ \
        newsize *= 2; \
    } \
    list->data = (type*)malloc(newsize); \
    if(!list->data){ \
        ORIG(E_NOMEM, "unable to malloc list"); \
    } \
    list->size = newsize; \
    list->len = 0; \
    list->fixed_size = false; \
    return E_OK; \
} \
derr_t list_ ## type ## _grow(LIST(type)* list, size_t num_items){ \
    /* check for reallocation */ \
    size_t min_size = num_items * sizeof(type); \
    if(list->size < min_size){ \
        /* don't try to realloc on a fixed-size list */ \
        if(list->fixed_size){ \
            ORIG(E_FIXEDSIZE, "unable to grow a fixed-size list"); \
        } \
        size_t newsize = MIN(list->size, 2); \
        while(newsize < min_size){ \
            newsize *= 2; \
        } \
        void* new = realloc(list->data, newsize); \
        if(!new) ORIG(E_NOMEM, "unable to realloc list"); \
        list->data = new; \
        list->size = newsize; \
    } \
    return E_OK; \
} \
derr_t list_ ## type ## _append(LIST(type)* list, type element){ \
    /* grow the list */ \
    PROP( list_ ## type ## _grow(list, list->len + 1) ); \
    /* append the element to the list */ \
    list->data[list->len++] = element; \
    return E_OK; \
} \
void list_ ## type ## _delete(LIST(type)* list, size_t index){ \
    /* move everything to the right of our pointer to the left */ \
    if(list->len - 1 == index){ \
        list->len--; \
    }else if(list->len - 1 > index){ \
        size_t num_bytes = (list->len - index - 1) * sizeof(type); \
        memmove(&(list->data[index]), &(list->data[index + 1]), num_bytes); \
        list->len--; \
    } \
} \
void list_ ## type ## _free(LIST(type)* list){ \
    if(list->data){ \
        free(list->data); \
        list->data = NULL; \
        list->len = 0; \
        list->size = 0; \
    } \
}

derr_t dstr_grow(dstr_t* ds, size_t min_size);
/* throws: E_FIXEDSIZE
           E_NOMEM */

// in:    full string
// start: byte offset to start of substring
// end:   byte offset to end of substring (0 means copy all)
//          (note that end=8 will include data[7] but not data[8])
// out:   output substring, will point into in->data
// note: it IS safe to do "a = dstr_sub(a, x, y)"
dstr_t dstr_sub(const dstr_t* in, size_t start, size_t end);

// result is <0, =0, or >0 if a is <b, =b, or >b, respectively
// Its just a byte comparison, its not UTF8-smart
// because there's multiple ways to encode the same accent in UTF8
int dstr_cmp(const dstr_t* a, const dstr_t* b);

// Not UTF8-smart at all
void dstr_upper(dstr_t* text);
void dstr_lower(dstr_t* text);

/* dstr_toi and friends have stricter type enforcement than atoi or strtol.
   Additionally, they result in an error if any part of *in is not part of the
   parsed number.  The idea is that you should know exactly what part of your
   string is a number and you should know what sort of number it is. */
derr_t dstr_toi(const dstr_t* in, int* out, int base);
derr_t dstr_tou(const dstr_t* in, unsigned int* out, int base);
derr_t dstr_tol(const dstr_t* in, long* out, int base);
derr_t dstr_toul(const dstr_t* in, unsigned long* out, int base);
derr_t dstr_toll(const dstr_t* in, long long* out, int base);
derr_t dstr_toull(const dstr_t* in, unsigned long long* out, int base);
derr_t dstr_tof(const dstr_t* in, float* out);
derr_t dstr_tod(const dstr_t* in, double* out);
derr_t dstr_told(const dstr_t* in, long double* out);
/*  throws : E_PARAM (for bad number strings) */

/*
   (inputs)
text:               the text to be searched
start:              byte offset to start search
end:                byte offset to end search (0 means search the whole text)
patterns:           a LIST(dstr_t) of patterns to check against
   (outputs)
position:           points to start of pattern (NULL for no match)
which_pattern:      which pattern got matched (0 if position == NULL)
partial_match_len:  length of partial match at end (0 if position != NULL)
*/
char* dstr_find(const dstr_t* text, const LIST(dstr_t)* patterns,
                size_t* which_pattern, size_t* partial_match_len);

/* this is meant to be a generic function that can handle various types of
   encoding/decoding situations.  It is safe for encoding/decoding operations
   on blocks of data from a stream, even if block boundaries land in the middle
   of a search pattern.

   As an overview: text from *in will be removed from *in and appended to *out,
   but with anything in search_patterns converted to the corresponding
   replace_patterns (although you can choose not to consume *in by setting
   stream=false).

   Note that not all text will be necessarily be encoded/decoded.  If the end
   of *in contains a partial match to one of the strings in search_strs, the
   resulting *in string will contain only that partial match.  This is
   desirable in the case where you are encoding/decoding a stream in small,
   fixed-size chunks, because the split between chunks should not cause
   encoding/decoding errors.  The parameter force_end can force a partial match
   at the end of *in to be moved to *out, for the last chunk of a stream.

   Note also that if desired, a *found_stop can be defined, in which case the
   the encoding/decoding will stop at a particular point in the stream.  The
   point is defined by when the string at stop_str_index is found.  This
   behavior is useful if the stream contains several delineated messages (each
   of which might span several fixed-size chunks) and you only want to pull out
   the first one.  If the pattern at stop_str_index is found, *found_end will
   be set to true.  If the search_strs at stop_str_index is found, it will be
   removed from *in and its replacement will be appended to *out as the final
   step.  Setting *found_stop = NULL will disable this behavior.

   In cases such as POP3 encoding/decoding, it would be desirable to write
   wrapper functions that call this function but hide irrelevant details.

   It might seem like ignore_partial and stream should always be set together,
   but ignore_partial is important during the known-last block of a stream.
   However, I can't think of any reason why you would set stream=false and
   ignore_partial=false...

   Finally, it is undefined behavior if one of the search strings is a
   substring of another search string.

   dstr_t* in:                       text to be translated
   dstr_t* out:                      where text goes after translation
   const LIST(dstr_t)* search_strs:  list of search strings
   const LIST(dstr_t)* replace_strs: list of replacement strings (equal length)
   bool ignore_partial:              ignore partial matches at the end of *in
   int stop_str_index:               specify which search string means "stop"
   bool* found_stop:                 if search_strs[stop_str_index] was found
   bool stream:                      whether or not to consume *in
   */
derr_t dstr_recode_stream(dstr_t* in,
                          dstr_t* out,
                          const LIST(dstr_t)* search_strs,
                          const LIST(dstr_t)* replace_strs,
                          bool ignore_partial,
                          size_t stop_str_index,
                          bool* found_stop);
/*  throws : E_NOMEM
             E_FIXEDSIZE */

// non-stream version has "append" parameter and const *in
derr_t dstr_recode(const dstr_t* in,
                   dstr_t* out,
                   const LIST(dstr_t)* search_strs,
                   const LIST(dstr_t)* replace_strs,
                   bool append);
/*  throws : E_NOMEM
             E_FIXEDSIZE */

size_t dstr_count(const dstr_t* text, const dstr_t* pattern);

/*
in:     dstr with text to copy
out:    dstr to be copied/appended to
*/
derr_t dstr_copy(const dstr_t* in, dstr_t* out);
/* throws: E_FIXEDSIZE
           E_NOMEM */
derr_t dstr_append(dstr_t* dstr, const dstr_t* new_text);
/* throws: E_FIXEDSIZE
           E_NOMEM */

/* splits a dstr_t into a LIST(dstr_t) based on a single pattern.  The resulting
   dstr_t's in the output will all point into the original text */
derr_t dstr_split(const dstr_t* text, const dstr_t* pattern,
                    LIST(dstr_t)* out);
/*  throws : E_NOMEM
             E_FIXEDSIZE */

// remove bytes from the left side of the string
// this will do memcpy or memmove appropriately
void dstr_leftshift(dstr_t* buffer, size_t count);

/* will append a '\0' to the end of a dstr without changing its length, or
   raise an error if it can't.  It will work always work on dstr_t's created
   with DSTR_STATIC(). */
derr_t dstr_null_terminate(dstr_t* ds);
/*  throws : E_NOMEM
             E_FIXEDSIZE */

/* this is a utility function for the common pattern where you have a
   LIST(dstr_t) *list and a corresponding dstr_t *mem of backing memory, and
   where all pointers in *list point into *mem.  It accepts an new
   dstr_t *elem, appends it to the *block, and if a reallocation is detected,
   it fixes the pointers in each element of *list.  A similar function is not
   necessary for other LIST(T) types because they generally are not pointers
   into other memory.  The null_term parameter specifies if a '\0' should be
   appended to *mem after *in, for if *in will be treated like a c-string. */
derr_t list_append_with_mem(LIST(dstr_t)* list, dstr_t* mem, dstr_t in,
                            bool null_term);
/*  throws : E_NOMEM
             E_FIXEDSIZE */

// a correct but currently inefficient lookup mechanism
bool in_list(const dstr_t* val, const LIST(dstr_t)* list, size_t* idx);

/* wraps the unistd.h read() function to read from a file descriptor and append
   it directly into a dstr_t buffer.  Specifying count=0 means try to fill the
   buffer.  dstr_read() will grow the buffer before it tries to read if
   necessary. */
derr_t dstr_read(int fd, dstr_t* buffer, size_t count, size_t* amnt_read);
/*  throws : E_NOMEM
             E_FIXEDSIZE
             E_OS */

derr_t dstr_write(int fd, const dstr_t* buffer);
/*  throws : E_OS */

// similar but wraps fread() and fwrite()
derr_t dstr_fread(FILE* f, dstr_t* buffer, size_t count, size_t* amnt_read);
/*  throws : E_NOMEM
             E_FIXEDSIZE
             E_OS */

derr_t dstr_fwrite(FILE* f, const dstr_t* buffer);
/*  throws : E_OS */

// read/write an entire file to/from memory
derr_t dstr_read_file(const char* filename, dstr_t* buffer);
/*  throws : E_NOMEM (reading into *buffer)
             E_FIXEDSIZE (reading into *buffer)
             E_OS (reading)
             E_OPEN */

derr_t dstr_write_file(const char* filename, const dstr_t* buffer);
/*  throws : E_OS
             E_OPEN */

derr_t dstr_fread_file(const char* filename, dstr_t* buffer);
/*  throws : E_NOMEM (might be *buffer or might be the FILE* allocation)
             E_FIXEDSIZE (reading into *buffer)
             E_OS (reading)
             E_OPEN */

derr_t dstr_fwrite_file(const char* filename, const dstr_t* buffer);
/*  throws : E_NOMEM (the FILE* allocation)
             E_OS
             E_OPEN */

derr_t bin2b64(dstr_t* bin, dstr_t* b64, size_t line_width, bool force_end);
/* throws: E_FIXEDSIZE (on output)
           E_NOMEM     (on output) */

derr_t b642bin(dstr_t* b64, dstr_t* bin);
/* throws: E_FIXEDSIZE (on output)
           E_NOMEM     (on output) */

// these are not streaming functions; that is, they don't consume the input
// however they only append to the output
derr_t bin2hex(const dstr_t* bin, dstr_t* hex);
/* throws: E_INTERNAL (snprintf)
           E_FIXEDSIZE (for *hex)
           E_NOMEM     (for *hex) */

derr_t hex2bin(const dstr_t* hex, dstr_t* bin);
/* throws: E_PARAM (bad hex input)
           E_FIXEDSIZE (for *bin)
           E_NOMEM     (for *bin) */

/* casting to an signed type when the original value does not fit in the bounds
   of the new type is undefined behavior.  Therefore, to guarantee proper
   behavior we will have a function that manually does the conversion how we
   want it done */
static inline char uchar_to_char(unsigned char u){
#if CHAR_MIN == 0
    // char is unsigned, cast is safe
    return (char)u;
#else
    // char is signed, cast is not safe
    // convert to int
    int i = u;
    // if i will not fit in the positive half of a signed char, subtract 256
    return (i > CHAR_MAX) ? (char)(i - (UCHAR_MAX + 1)) : (char)i;
#endif
}

// String Formatting functions below //

typedef enum {
    FMT_UINT,
    FMT_INT,
    FMT_FLOAT,
    FMT_CHAR,
    FMT_CSTR,
    FMT_PTR,
    FMT_DSTR,
    FMT_EXT,
} fmt_type_t;

typedef struct {
    const void* arg;
    derr_t (*hook)(dstr_t* out, FILE* f, size_t* written, const void* arg);
} fmt_data_ext_t;

typedef union {
    uintmax_t u;
    intmax_t i;
    long double f;
    char c;
    const char* cstr;
    const void* ptr;
    const dstr_t* dstr;
    fmt_data_ext_t ext;
} fmt_data_t;

typedef struct {
    fmt_type_t type;
    fmt_data_t data;
} fmt_t;

/* NOTE: this following function does not use the error-handling macros,
   because the it is *called* in the error-handling macros, and any error
   results in an immediate recurse-until-segfault operation. */
// Also note: this function is meant to be called by the FFMT macro
derr_t pvt_ffmt(FILE* f, size_t* written, const char* fstr, const fmt_t* args,
                size_t nargs);
/*  throws : E_OS (on the write part)
             If you use an extension, this could throw additional errors */

// Note: this function is meant to be called by the FMT macro
derr_t pvt_fmt(dstr_t* out, const char* fstr, const fmt_t* args, size_t nargs);
/*  throws : E_NOMEM
             E_FIXEDSIZE
             E_INTERNAL
             If you use an extension, this could throw additional errors */

/* the following macros CANNOT be inline functions because the compound
   literals they create have automatic storage that falls out of scope as soon
   as the compound literal falls out of scope (meaning you can't safely return
   the compound literal or it will be useless) */

// FMT is like sprintf
#define FMT(out, fstr, ...) \
    pvt_fmt(out, \
          fstr, \
          (const fmt_t[]){FI(1), __VA_ARGS__}, \
          sizeof((const fmt_t[]){FI(1), __VA_ARGS__})/sizeof(fmt_t))

// FFMT is like fprintf
#define FFMT(f, written, fstr, ...) \
    pvt_ffmt(f, \
           written, \
           fstr, \
           (const fmt_t[]){FI(1), __VA_ARGS__}, \
           sizeof((const fmt_t[]){FI(1), __VA_ARGS__})/sizeof(fmt_t))

// PFMT is like printf
#define PFMT(fstr, ...) FFMT(stdout, NULL, fstr, __VA_ARGS__)

/* The following functions can be inline functions because they return structs
   which only contain literal values or pointers to outside objects */
static inline fmt_t FU(uintmax_t arg){ return (fmt_t){FMT_UINT, {.u = arg} }; }
static inline fmt_t FI(intmax_t arg){ return (fmt_t){FMT_INT, {.i = arg} }; }
static inline fmt_t FF(long double arg){ return (fmt_t){FMT_FLOAT, {.f = arg} }; }
static inline fmt_t FC(char arg){ return (fmt_t){FMT_CHAR, {.c = arg} }; }
static inline fmt_t FS(const char* arg){ return (fmt_t){FMT_CSTR, {.cstr = arg} }; }
static inline fmt_t FD(const dstr_t* arg){ return (fmt_t){FMT_DSTR, {.dstr = arg} }; }
static inline fmt_t FP(const void* arg){ return (fmt_t){FMT_PTR, {.ptr = arg} }; }

derr_t fmthook_dstr_dbg(dstr_t* out, FILE* f, size_t* written, const void* arg);

static inline fmt_t FD_DBG(const dstr_t* arg){
    return (fmt_t){FMT_EXT, {.ext = {.arg = (const void*)arg,
                                     .hook = fmthook_dstr_dbg} } };
}

// the FMT()-ready replacement of perror():
derr_t fmthook_strerror(dstr_t* out, FILE* f, size_t* written, const void* arg);

static inline fmt_t FE(int* err){
    return (fmt_t){FMT_EXT, {.ext = {.arg = (const void*)err,
                                     .hook = fmthook_strerror} } };
}

// String Builder stuff below //

typedef struct string_builder_t {
    const struct string_builder_t* prev;
    const struct string_builder_t* next;
    fmt_t elem;
} string_builder_t;

static inline string_builder_t sb_append(const string_builder_t* sb, fmt_t elem){
    return (string_builder_t){.prev=sb, .next=NULL, .elem=elem};
}

static inline string_builder_t sb_prepend(const string_builder_t* sb, fmt_t elem){
    return (string_builder_t){.prev=NULL, .next=sb, .elem=elem};
}

// shortcut for literal string builder objects
#define SB(fmt) ((string_builder_t){.prev=NULL, .next=NULL, .elem=fmt})

derr_t sb_append_to_dstr(const string_builder_t* sb, const dstr_t* joiner, dstr_t* out);
derr_t sb_to_dstr(const string_builder_t* sb, const dstr_t* joiner, dstr_t* out);
derr_t sb_fwrite(FILE* f, size_t* written, const string_builder_t* sb,
                 const dstr_t* joiner);

/* this will attempt to expand the string_builder into a stack buffer, and then
   into a heap buffer if that fails.  The "out" parameter is a pointer to
   whichever buffer was successfully written to.  The result is always
   null-terminated. */
derr_t sb_expand(const string_builder_t* sb, const dstr_t* joiner,
                 dstr_t* stack_dstr, dstr_t* heap_dstr, dstr_t** out);

// FMT() support for string_builder
derr_t fmthook_sb(dstr_t* out, FILE* f, size_t* written, const void* arg);

// this is just for passing two args in one fmthook
typedef struct {
    const string_builder_t* sb;
    const dstr_t* joiner;
} string_builder_format_t;

/* This needs to be a macro to put the intermediate struct in the proper scope.
   Treat it as if it were the following prototype:

    fmt_t FSB(const string_builder_t* arg, const dstr_t* joiner);
*/
#define FSB(_sb, _joiner) ((fmt_t){ \
     FMT_EXT, { \
        .ext = { \
            .hook = fmthook_sb, \
            .arg = (const void*)(&(const string_builder_format_t){ \
                .sb = _sb, \
                .joiner = _joiner, \
            })\
        }}})


#endif //COMMON_H
