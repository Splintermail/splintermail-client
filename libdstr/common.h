#undef bool
typedef _Bool bool;

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif
#ifndef ABS
#define ABS(a)   ((a) > 0 ? (a) : (-(a)))
#endif

/* DEF_CONTAINER_OF should be used right after struct definition to create an
   inline function for dereferencing a struct via a member.  "member_type" is
   required to avoid typeof, which windows doesn't have.  Also, unlike the
   linux kernel version, multi-token types ("struct xyz") are not supported. */
#define DEF_CONTAINER_OF(structure, member, member_type) \
    static inline structure *structure ## _ ## member ## _container_of( \
            const member_type *ptr){ \
        if(ptr == NULL) return NULL; \
        uintptr_t offset = offsetof(structure, member); \
        return (structure*)((uintptr_t)ptr - offset); \
    }

#define CONTAINER_OF(ptr, structure, member) \
    structure ## _ ## member ## _container_of(ptr)

// functional API for passing a pointer and erasing its original location
#define DEF_STEAL_STRUCT(type) \
    static inline type steal_##type(type *old){ \
        type temp = *old; \
        *old = (type){0}; \
        return temp; \
    }

#define DEF_STEAL_PTR(type) \
    static inline type *steal_##type(type **old){ \
        type *temp = *old; \
        *old = NULL; \
        return temp; \
    }

#define STEAL(type, ptr) steal_##type(ptr)

typedef struct {
    char* data;
    size_t size;
    size_t len;
    bool fixed_size;
} dstr_t;

/* Build an extensible error system, where every error type is a global
   pointer to a simple interface.  The system is extensible because the error
   handling macros do not need an exhaustive list of all errors in order to
   work. */

struct derr_type_t;
typedef const struct derr_type_t *derr_type_t;
struct derr_type_t {
    const dstr_t *name;
    const dstr_t *msg;
    bool (*matches)(derr_type_t self, derr_type_t other);
    // for simple closures, like with error groups
    void *data;
};

typedef struct {
    derr_type_t type;
    dstr_t msg;
} derr_t;

const dstr_t *error_to_dstr(derr_type_t type);
const dstr_t *error_to_msg(derr_type_t type);

#define REGISTER_ERROR_TYPE(NAME, NAME_STRING, MSG_STRING) \
    DSTR_STATIC(NAME ## _name_dstr, NAME_STRING); \
    DSTR_STATIC(NAME ## _msg_dstr, MSG_STRING); \
    static bool NAME ## _matches(derr_type_t self, derr_type_t other){ \
        (void)self; \
        return other == NAME; \
    } \
    derr_type_t NAME = &(struct derr_type_t){ \
        .name = &NAME ## _name_dstr, \
        .msg = &NAME ## _msg_dstr, \
        .matches = NAME ## _matches, \
    }

#define REGISTER_STATIC_ERROR_TYPE(NAME, NAME_STRING, MSG_STRING) \
    DSTR_STATIC(NAME ## _name_dstr, NAME_STRING); \
    DSTR_STATIC(NAME ## _msg_dstr, MSG_STRING); \
    static bool NAME ## _matches(derr_type_t self, derr_type_t other){ \
        (void)self; \
        return other == NAME; \
    } \
    static derr_type_t NAME = &(struct derr_type_t){ \
        .name = &NAME ## _name_dstr, \
        .msg = &NAME ## _msg_dstr, \
        .matches = NAME ## _matches, \
    }

/* Error type groups are for matching matching against multiple error types.
   Error groups must never be thrown, because they do not support .to_string
   and they will not self-match. They are only for catching errors. */
struct derr_type_group_arg_t {
    derr_type_t *types;
    size_t ntypes;
};
bool derr_type_group_matches(derr_type_t self, derr_type_t other);
#define ERROR_GROUP(...) \
    &(struct derr_type_t){ \
        .name = NULL, \
        .msg = NULL, \
        .matches = derr_type_group_matches, \
        .data = &(struct derr_type_group_arg_t){ \
            .types = (derr_type_t[]){__VA_ARGS__}, \
            .ntypes = sizeof((derr_type_t[]){__VA_ARGS__}) / sizeof(derr_type_t), \
        } \
    }

#define E_NONE NULL
extern derr_type_t E_ANY;        /* For catching errors only; this value must
                                    never be thrown.  Will match against any
                                    non-E_NONE error. */

extern derr_type_t E_NOMEM;      // some memory allocation failed
extern derr_type_t E_SOCK;       // error in socket(), bind(), listen(), or accept()
extern derr_type_t E_CONN;       // error in connect() or send() or recv()
extern derr_type_t E_VALUE;      // delete this
extern derr_type_t E_FIXEDSIZE;  // an operation would result in a buffer overflow
extern derr_type_t E_OS;         // some system call failed
extern derr_type_t E_BADIDX;     // delete this
extern derr_type_t E_OPEN;       // an error in compat_open() # TODO: replace with E_FS
extern derr_type_t E_PARAM;      // invalid input parameter
extern derr_type_t E_INTERNAL;   // a never-should-happen failure occured
extern derr_type_t E_FS;         // a file system-related error
extern derr_type_t E_RESPONSE;   // invalid response from something external
extern derr_type_t E_USERMSG;    // an error with a user-facing message (don't TRACE before it)
extern derr_type_t E_CANCELED;   // operation was canceled by user

/* for backwards compatibility with a LOT of code, E_OK is not part of the
   derr_type_t enum but is actually a derr_t struct with an empty message. */
#define E_OK ((derr_t){.type = E_NONE})

/* take an E_USERMSG message, copy the user message substring with truncation
   and guaranteed null-termination to a buffer, and DROP_VAR the error.  buf
   should be a fixed-size buffer on the stack */
void consume_e_usermsg(derr_t *e, dstr_t *buf);

// wrap an empty char[] with in a dstr_t
#define DSTR_WRAP_ARRAY(dstr, buffer) do { \
    (dstr).data = (buffer); \
    (dstr).size = sizeof(buffer); \
    (dstr).len = 0; \
    (dstr).fixed_size = 1; \
} while(0)

// wrap a char* of known length in a dstr_t
#define DSTR_WRAP(dstr, buffer, length, null_terminated) do { \
    (dstr).data = (buffer); \
    (dstr).size = (length) + (null_terminated); \
    (dstr).len = (length); \
    (dstr).fixed_size = 1; \
} while(0)

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
derr_type_t dstr_new_quiet(dstr_t *ds, size_t size);
derr_t dstr_new(dstr_t* ds, size_t size);
/*  throws : E_NOMEM */

void dstr_free(dstr_t* ds);

// I thought of this but didn't implement it, because it is only safe to allow
// this sort of auto-allocate usage if you zeroize the input parameters, so you
// are not really getting rid of any "gotchas" at all.
// /* A clean api for implementing things like sb_append which would have will
//    have to make multiple calls to dstr_append.  If you are passed an
//    unallocated output, and you fail several calls in, you want to ensure that
//    you don't leave the output in a possibly-allocated state (which would be a
//    pain in the ass for the caller).  However, you don't want to always allocate
//    a local dstr_t because that would not work transparently with fixed-size
//    dstr_t's.  So you call this at the beginning of your function, and then in
//    error cases you free free_on_error, which will conveniently be NULL in cases
//    where freeing the output is not necessary */
// derr_t dstr_maybe_alloc_quiet(
//     dstr_t *maybe_needs_alloc,
//     dstr_t** free_on_error,
// );

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
    derr_t e = E_OK; \
    /* only malloc in power of 2 */ \
    size_t min_size = num_items * sizeof(type); \
    size_t newsize = 2; \
    while(newsize < min_size){ \
        newsize *= 2; \
    } \
    list->data = (type*)malloc(newsize); \
    if(!list->data){ \
        ORIG(&e, E_NOMEM, "unable to malloc list"); \
    } \
    list->size = newsize; \
    list->len = 0; \
    list->fixed_size = false; \
    return e; \
} \
derr_t list_ ## type ## _grow(LIST(type)* list, size_t num_items){ \
    derr_t e = E_OK; \
    /* check for reallocation */ \
    size_t min_size = num_items * sizeof(type); \
    if(list->size < min_size){ \
        /* don't try to realloc on a fixed-size list */ \
        if(list->fixed_size){ \
            ORIG(&e, E_FIXEDSIZE, "unable to grow a fixed-size list"); \
        } \
        size_t newsize = MIN(list->size, 2); \
        while(newsize < min_size){ \
            newsize *= 2; \
        } \
        void* new = realloc(list->data, newsize); \
        if(!new) ORIG(&e, E_NOMEM, "unable to realloc list"); \
        list->data = new; \
        list->size = newsize; \
    } \
    return e; \
} \
derr_t list_ ## type ## _append(LIST(type)* list, type element){ \
    derr_t e = E_OK; \
    /* grow the list */ \
    PROP(&e, list_ ## type ## _grow(list, list->len + 1) ); \
    /* append the element to the list */ \
    list->data[list->len++] = element; \
    return e; \
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

derr_type_t dstr_grow_quiet(dstr_t *ds, size_t min_size);
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

// this is the right api, use SIZE_MAX instead for 0 for end
dstr_t dstr_sub2(const dstr_t in, size_t start, size_t end);

// get a zero-len dstr_t that, when written to, would fill `in`
dstr_t dstr_empty_space(const dstr_t in);

dstr_t _dstr_lstrip_chars(const dstr_t in, const char *chars, size_t n);
#define dstr_lstrip_chars(in, ...) \
    _dstr_lstrip_chars( \
        (in), \
        &(const char[]){'\0', __VA_ARGS__}[1], \
        sizeof((char[]){'\0', __VA_ARGS__}) / sizeof(char) - 1 \
    )

dstr_t _dstr_rstrip_chars(const dstr_t in, const char *chars, size_t n);
#define dstr_rstrip_chars(in, ...) \
    _dstr_rstrip_chars( \
        (in), \
        &(const char[]){'\0', __VA_ARGS__}[1], \
        sizeof((char[]){'\0', __VA_ARGS__}) / sizeof(char) - 1 \
    )

dstr_t _dstr_strip_chars(const dstr_t in, const char *chars, size_t n);
#define dstr_strip_chars(in, ...) \
    _dstr_strip_chars( \
        (in), \
        &(const char[]){'\0', __VA_ARGS__}[1], \
        sizeof((char[]){'\0', __VA_ARGS__}) / sizeof(char) - 1 \
    )


// result is <0, =0, or >0 if a is <b, =b, or >b, respectively
// Its just a byte comparison, its not UTF8-smart
// because there's multiple ways to encode the same accent in UTF8
int dstr_cmp(const dstr_t *a, const dstr_t *b);
int dstr_cmp2(const dstr_t a, const dstr_t b);
bool dstr_eq(const dstr_t a, const dstr_t b);

// case-insensitive
int dstr_icmp(const dstr_t *a, const dstr_t *b);
int dstr_icmp2(const dstr_t a, const dstr_t b);
bool dstr_ieq(const dstr_t a, const dstr_t b);

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
derr_t dstr_tou64(const dstr_t* in, uint64_t* out, int base);
derr_t dstr_tof(const dstr_t* in, float* out);
derr_t dstr_tod(const dstr_t* in, double* out);
derr_t dstr_told(const dstr_t* in, long double* out);
derr_t dstr_tosize(const dstr_t* in, size_t* out, int base);
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
size_t dstr_count2(const dstr_t text, const dstr_t pattern);

// case-insensitive
size_t dstr_icount2(const dstr_t text, const dstr_t pattern);

bool dstr_beginswith(const dstr_t *str, const dstr_t *pattern);
bool dstr_endswith(const dstr_t *str, const dstr_t *pattern);

/*
in:     dstr with text to copy
out:    dstr to be copied/appended to
*/
derr_t dstr_copy(const dstr_t* in, dstr_t* out);
/* throws: E_FIXEDSIZE
           E_NOMEM */
derr_type_t dstr_append_quiet(dstr_t *dstr, const dstr_t *new_text);
derr_t dstr_append(dstr_t* dstr, const dstr_t* new_text);
/* throws: E_FIXEDSIZE
           E_NOMEM */

// like dupstr
derr_t dstr_dupstr(const dstr_t in, char** out);

/* splits a dstr_t into a LIST(dstr_t) based on a single pattern.  The
   resulting dstr_t's in the output will all point into the original text */
derr_t dstr_split(const dstr_t* text, const dstr_t* pattern,
                    LIST(dstr_t)* out);
/*  throws : E_NOMEM
             E_FIXEDSIZE */

/* like dstr_split, execpt in the case of E_FIXEDSIZE, it drops the error and
   sets the final token to be the full remaining length of the string, so
   if you split a string on '.' into 3 tokens, you could get something like:

       "a.b.c.d.e.f" -> {"a", "b", "c.d.e.f"}
*/
derr_t dstr_split_soft(const dstr_t *text, const dstr_t *pattern,
        LIST(dstr_t)* out);
/* throws : E_NOMEM */

derr_t _dstr_split2(
    const dstr_t text,
    const dstr_t pattern,
    size_t *len,
    dstr_t **outs,
    size_t nouts
);
#define dstr_split2(text, pattern, len, ...) \
    _dstr_split2( \
        (text), \
        (pattern), \
        (len), \
        &(dstr_t*[]){&(dstr_t){0}, __VA_ARGS__}[1], \
        sizeof((dstr_t*[]){&(dstr_t){0}, __VA_ARGS__}) / sizeof(dstr_t*) - 1 \
    )

void _dstr_split2_soft(
    const dstr_t text,
    const dstr_t pattern,
    size_t *len,
    dstr_t **outs,
    size_t nouts
);
#define dstr_split2_soft(text, pattern, len, ...) \
    _dstr_split2_soft( \
        (text), \
        (pattern), \
        (len), \
        &(dstr_t*[]){&(dstr_t){0}, __VA_ARGS__}[1], \
        sizeof((dstr_t*[]){&(dstr_t){0}, __VA_ARGS__}) / sizeof(dstr_t*) - 1 \
    )

// remove bytes from the left side of the string
// this will do memcpy or memmove appropriately
void dstr_leftshift(dstr_t* buffer, size_t count);

/* will append a '\0' to the end of a dstr without changing its length, or
   raise an error if it can't.  It will work always work on dstr_t's created
   with DSTR_STATIC(). */
derr_type_t dstr_null_terminate_quiet(dstr_t* ds);
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

/* wraps the unistd.h compat_read() function to read from a file descriptor and append
   it directly into a dstr_t buffer.  Specifying count=0 means try to fill the
   buffer.  dstr_read() will grow the buffer before it tries to read if
   necessary. */
derr_t dstr_read(int fd, dstr_t* buffer, size_t count, size_t* amnt_read);
/*  throws : E_NOMEM
             E_FIXEDSIZE
             E_OS */

derr_t dstr_read_all(int fd, dstr_t *out);
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

derr_type_t dstr_fwrite_quiet(FILE* f, const dstr_t* buffer);
derr_t dstr_fwrite(FILE* f, const dstr_t* buffer);
/*  throws : E_OS */

derr_type_t bin2b64_quiet(
    const dstr_t* bin,
    dstr_t* b64,
    size_t line_width,
    bool force_end,
    size_t *consumed
);
/* returns: E_FIXEDSIZE
            E_NOMEM */

derr_type_t b642bin_quiet(const dstr_t* b64, dstr_t* bin, size_t *consumed);
/* returns: E_FIXEDSIZE
            E_NOMEM */

derr_t bin2b64(const dstr_t *bin, dstr_t *b64);
derr_t bin2b64_stream(
    dstr_t* bin, dstr_t* b64, size_t line_width, bool force_end
);

derr_t b642bin(const dstr_t *b64, dstr_t *bin);
derr_t b642bin_stream(dstr_t* b64, dstr_t* bin);

// simple math helpers
size_t b642bin_output_len(size_t in);
size_t bin2b64_output_len(size_t in);

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

/* fmt_dstr_append_quiet is just like dstr_append_quiet except when it fails
   due to a size limit it will try to fill the buffer as much as possible
   before returning the error */
derr_type_t fmt_dstr_append_quiet(dstr_t *dstr, const dstr_t *new_text);

typedef enum {
    FMT_UINT,
    FMT_INT,
    FMT_FLOAT,
    FMT_CHAR,
    FMT_CSTR,
    FMT_PTR,
    FMT_DSTR,
    FMT_BOOL,
    FMT_EXT,
    FMT_EXT_NOCONST,
} fmt_type_t;

typedef struct {
    const void* arg;
    derr_type_t (*hook)(dstr_t* out, const void* arg);
} fmt_data_ext_t;

typedef struct {
    void* arg;
    derr_type_t (*hook)(dstr_t* out, void* arg);
} fmt_data_ext_noconst_t;

typedef union {
    uintmax_t u;
    intmax_t i;
    long double f;
    char c;
    const char* cstr;
    const void* ptr;
    const dstr_t* dstr;
    bool boolean;
    fmt_data_ext_t ext;
    fmt_data_ext_noconst_t ext_noconst;
} fmt_data_t;

typedef struct {
    fmt_type_t type;
    fmt_data_t data;
} fmt_t;

/* NOTE: this following function does not use the error-handling macros,
   because the it is *called* in the error-handling macros, and any error
   results in an immediate recurse-until-segfault operation. */
// Also note: this function is meant to be called by the FFMT macro
derr_type_t pvt_ffmt_quiet(
        FILE* f, size_t* written, const char* fstr, const fmt_t* args,
        size_t nargs);
derr_t pvt_ffmt(
        FILE* f, size_t* written, const char* fstr, const fmt_t* args,
        size_t nargs);
/*  throws : E_OS (on the write part)
             If you use an extension, this could throw additional errors */

// Note: this function is meant to be called by the FMT macro
derr_type_t pvt_fmt_quiet(
    dstr_t* out, const char* fstr, const fmt_t* args, size_t nargs
);
derr_t pvt_fmt(
    dstr_t* out, const char* fstr, const fmt_t* args, size_t nargs
);
/*  throws : E_NOMEM
             E_FIXEDSIZE
             E_INTERNAL
             If you use an extension, this could throw additional errors */

/* the following macros CANNOT be inline functions because the compound
   literals they create have automatic storage that falls out of scope as soon
   as the compound literal falls out of scope (meaning you can't safely return
   the compound literal or it will be useless) */

// FMT is like sprintf, without the null-termination guarantee in error cases
#define FMT(out, fstr, ...) \
    pvt_fmt(out, \
        fstr, \
        &(const fmt_t[]){FI(1), __VA_ARGS__}[1], \
        sizeof((const fmt_t[]){FI(1), __VA_ARGS__})/sizeof(fmt_t) - 1 \
    )
#define FMT_QUIET(out, fstr, ...) \
    pvt_fmt_quiet(out, \
        fstr, \
        &(const fmt_t[]){FI(1), __VA_ARGS__}[1], \
        sizeof((const fmt_t[]){FI(1), __VA_ARGS__})/sizeof(fmt_t) - 1 \
    )

// FFMT is like fprintf
#define FFMT(f, written, fstr, ...) \
    pvt_ffmt(f, \
        written, \
        fstr, \
        &(const fmt_t[]){FI(1), __VA_ARGS__}[1], \
        sizeof((const fmt_t[]){FI(1), __VA_ARGS__})/sizeof(fmt_t) - 1 \
    )

#define FFMT_QUIET(f, written, fstr, ...) \
    pvt_ffmt_quiet(f, \
        written, \
        fstr, \
        &(const fmt_t[]){FI(1), __VA_ARGS__}[1], \
        sizeof((const fmt_t[]){FI(1), __VA_ARGS__})/sizeof(fmt_t) - 1 \
    )

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
static inline fmt_t FB(bool arg){ return (fmt_t){FMT_BOOL, {.boolean = arg} }; }

derr_type_t fmthook_dstr_dbg(dstr_t* out, const void* arg);

static inline fmt_t FD_DBG(const dstr_t* arg){
    return (fmt_t){FMT_EXT, {.ext = {.arg = (const void*)arg,
                                     .hook = fmthook_dstr_dbg} } };
}

// the FMT()-ready replacement of perror():
derr_type_t fmthook_strerror(dstr_t* out, const void* arg);

static inline fmt_t FE(int* err){
    return (fmt_t){FMT_EXT, {.ext = {.arg = (const void*)err,
                                     .hook = fmthook_strerror} } };
}

// hex-encoded dstr
derr_type_t fmthook_bin2hex(dstr_t* out, const void* arg);

static inline fmt_t FX(const dstr_t *arg){
    return (fmt_t){FMT_EXT, {.ext = {.arg = (const void*)arg,
                                     .hook = fmthook_bin2hex} } };
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
derr_type_t fmthook_sb(dstr_t* out, const void* arg);

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
