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

#ifndef _WIN32
// unix
#define SM_NORETURN(fn) fn __attribute__((noreturn))
#else
// windows
#define SM_NORETURN(fn) __declspec(noreturn) fn
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
        return (structure*)((uintptr_t)ptr - (uintptr_t)offset); \
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
DEF_STEAL_STRUCT(dstr_t)

/* Build an extensible error system, where every error type is a global
   pointer to a simple interface.  The system is extensible because the error
   handling macros do not need an exhaustive list of all errors in order to
   work. */

struct derr_type_t;
typedef const struct derr_type_t *derr_type_t;
struct derr_type_t {
    const dstr_t *name;
    const dstr_t *msg;
};

typedef struct {
    derr_type_t type;
    dstr_t msg;
} derr_t;

dstr_t error_to_dstr(derr_type_t type);
dstr_t error_to_msg(derr_type_t type);

#define REGISTER_ERROR_TYPE(NAME, NAME_STRING, MSG_STRING) \
    DSTR_STATIC(NAME ## _name_dstr, NAME_STRING); \
    DSTR_STATIC(NAME ## _msg_dstr, MSG_STRING); \
    derr_type_t NAME = &(struct derr_type_t){ \
        .name = &NAME ## _name_dstr, \
        .msg = &NAME ## _msg_dstr, \
    }

#define REGISTER_STATIC_ERROR_TYPE(NAME, NAME_STRING, MSG_STRING) \
    DSTR_STATIC(NAME ## _name_dstr, NAME_STRING); \
    DSTR_STATIC(NAME ## _msg_dstr, MSG_STRING); \
    static derr_type_t NAME = &(struct derr_type_t){ \
        .name = &NAME ## _name_dstr, \
        .msg = &NAME ## _msg_dstr, \
    }

#define E_NONE NULL

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
extern derr_type_t E_BUSY;       // resource in use

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

// for global variables, where msvc is offended by the leading (dstr_t)
#define DSTR_GLOBAL(str) {.data=str, \
                          .size=sizeof(str), \
                          .len=sizeof(str) - 1, \
                          .fixed_size=true}

// for right-hand side of declaration/definitions
#define DSTR_LIT(str) ((dstr_t)DSTR_GLOBAL(str))

// this allocates a new dstr on the heap
derr_type_t dstr_new_quiet(dstr_t *ds, size_t size);
derr_t dstr_new(dstr_t* ds, size_t size);
/*  throws : E_NOMEM */

// zeroize the whole contents of a buffer, such as to erase key material
/* Note that dstr_grow() and friends could realloc the buffer, so sensitive
   dstr_t's should prefer dstr_grow0 and dstr_append0 */
void dstr_zeroize(dstr_t *ds);

void dstr_free(dstr_t* ds);
// same as dstr_free, but zeroize first
void dstr_free0(dstr_t* ds);

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
    memset((char*)list->data, 0, newsize); \
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
        size_t newsize = MAX(list->size, 2); \
        while(newsize < min_size){ \
            newsize *= 2; \
        } \
        void* new = realloc(list->data, newsize); \
        if(!new) ORIG(&e, E_NOMEM, "unable to realloc list"); \
        memset((char*)new + list->size, 0, newsize - list->size); \
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

dstr_t dstr_from_cstr(char *cstr);
dstr_t dstr_from_cstrn(char *cstr, size_t n, bool null_terminated);

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

   Finally, it is undefined behavior if one of the search strings is a
   substring of another search string.

   dstr_t* in:                       text to be translated
   dstr_t* out:                      where text goes after translation
   const LIST(dstr_t)* search_strs:  list of search strings
   const LIST(dstr_t)* replace_strs: list of replacement strings (equal length)
   bool force_end:                   copy partial matches at the end of *in
   int stop_str_index:               specify which search string means "stop"
   bool* found_stop:                 if search_strs[stop_str_index] was found
   */
derr_t dstr_recode_stream(dstr_t* in,
                          dstr_t* out,
                          const LIST(dstr_t)* search_strs,
                          const LIST(dstr_t)* replace_strs,
                          bool force_end,
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

// like dstr_count but exists after the first occurance
bool dstr_contains(const dstr_t text, const dstr_t pattern);
bool dstr_icontains(const dstr_t text, const dstr_t pattern);

bool dstr_beginswith(const dstr_t *str, const dstr_t *pattern);
bool dstr_beginswith2(const dstr_t str, const dstr_t pattern);
bool dstr_ibeginswith2(const dstr_t str, const dstr_t pattern);

bool dstr_endswith(const dstr_t *str, const dstr_t *pattern);
bool dstr_endswith2(const dstr_t str, const dstr_t pattern);
bool dstr_iendswith2(const dstr_t str, const dstr_t pattern);

derr_type_t dstr_grow_quiet(dstr_t *ds, size_t min_size);
derr_t dstr_grow(dstr_t* ds, size_t min_size);
/* throws: E_FIXEDSIZE
           E_NOMEM */

// same as dstr_grow, but zeroize the old buffer
derr_type_t dstr_grow0_quiet(dstr_t *ds, size_t min_size);
derr_t dstr_grow0(dstr_t* ds, size_t min_size);

derr_type_t dstr_append_quiet(dstr_t *dstr, const dstr_t *new_text);
derr_t dstr_append(dstr_t* dstr, const dstr_t* new_text);
/* throws: E_FIXEDSIZE
           E_NOMEM */
/*
in:     dstr with text to copy
out:    dstr to be copied/appended to
*/
derr_t dstr_copystrn(const char *in, size_t n, dstr_t *out);
derr_t dstr_copystr(const char *in, dstr_t *out);
derr_t dstr_copy(const dstr_t* in, dstr_t* out);
derr_t dstr_copy2(const dstr_t in, dstr_t* out);
/* throws: E_FIXEDSIZE
           E_NOMEM */

// same as dstr_append
derr_type_t dstr_append_quiet(dstr_t *dstr, const dstr_t *new_text);
derr_t dstr_append(dstr_t* dstr, const dstr_t* new_text);

// same as dstr_append but zeroize old buffers
derr_type_t dstr_append0_quiet(dstr_t *dstr, const dstr_t *new_text);
derr_t dstr_append0(dstr_t* dstr, const dstr_t* new_text);

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

derr_t dstr_fread_all(FILE* f, dstr_t* buffer);

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

// no error checking
char hex_high_nibble(unsigned char u);
char hex_low_nibble(unsigned char u);

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

derr_type_t dstr_append_char(dstr_t* dstr, char val);
// append the same char multiple times, such as for padding a string
derr_type_t dstr_append_char_n(dstr_t* dstr, char val, size_t n);
