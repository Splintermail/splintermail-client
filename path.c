#include "common.h"
#include "logger.h"

//// I hated how the API for this turned out

/*#define PATH(...) (LIST(dstr_t)){.data=(dstr_t[]){__VA_ARGS__},\
                                   .size=sizeof((dstr_t[]){__VA_ARGS__}), \
                                   .len=sizeof((dstr_t[]){__VA_ARGS__})/sizeof(dstr_t), \
                                   .fixed_size=true} */

// static derr_t print_path(LIST(dstr_t) path){
//     for(size_t i = 0; i < path.len; i++){
//         PFMT("%x", FD(&path.data[i]));
//     }
//     PFMT("\n");
//     return E_OK;
// }

/* resulting API is way too verbose:

    PROP( print_path(PATH(DSTR_LIT("aaa"), DSTR_LIT("b"),
        DSTR_LIT("abc"),
        DSTR_LIT("b"))) );
*/

typedef enum {
    PATH_DSTR,
    PATH_CSTR,
} path_elem_type_t;

typedef union {
    const char* cstr;
    const dstr_t* dstr;
} path_elem_data_t;

typedef struct {
    path_elem_type_t type;
    path_elem_data_t data;
} path_elem_t;

/* While these inline functions version don't throw errors or valgrind errors,
   I think they are actually invalid according to C99 standard 6.5.2.5.
   Like I think as soon as you exit from the function, the automatic storage
   of the compound literal will go out of scope.

   But wait... if you are returning a struct, and the struct is exclusively
   pointers to external things... I think that should be totally valid.

*/

static inline path_elem_t PD(const dstr_t* elem){
    return (path_elem_t){.data.dstr = elem, .type=PATH_DSTR};
}

static inline path_elem_t PS(const char* elem){
    return (path_elem_t){.data.cstr = elem, .type=PATH_CSTR};
}

//#define PD(elem) (path_elem_t){.data.dstr = elem, .type=PATH_DSTR}
//#define PS(elem) (path_elem_t){.data.cstr = elem, .type=PATH_CSTR}

// outputs two args, a list and a length
#define PATH_ARGS(...) \
    (const path_elem_t[]){__VA_ARGS__}, \
    sizeof((const path_elem_t[]){__VA_ARGS__})/sizeof(path_elem_t)

#define PRINT_PATH2(...) \
    print_path2(PATH_ARGS(__VA_ARGS__))

/* this API was fine but the fact that the macro outputs two arguments means
   that you can't save the output of the macro to a variable */

static derr_t print_path2(const path_elem_t* elems, size_t len){
    for(size_t i = 0; i < len; i++){
        if(elems[i].type == PATH_CSTR){
            PFMT("%x", FS(elems[i].data.cstr));
        }else if(elems[i].type == PATH_DSTR){
            PFMT("%x", FD(elems[i].data.dstr));
        }
    }
    PFMT("\n");
    return E_OK;
}

typedef struct {
    const path_elem_t* elems;
    size_t len;
} path_list_t;

#define PATH_LIST(...) \
    (path_list_t){ .elems=(const path_elem_t[]){__VA_ARGS__}, \
                   .len=sizeof((const path_elem_t[]){__VA_ARGS__})/sizeof(path_elem_t) }

/* I like this API much better, and you could make either PRINT_PATH3 a macro
   or manually use the PATH macro.  Manually using the PATH macro would be
   more consistent with sometimes storing the path_list_t in a variable as well
   as controlling the order of arguments easier */

static derr_t print_path3(const path_list_t path){
    for(size_t i = 0; i < path.len; i++){
        if(path.elems[i].type == PATH_CSTR){
            PFMT("%x", FS(path.elems[i].data.cstr));
        }else if(path.elems[i].type == PATH_DSTR){
            PFMT("%x", FD(path.elems[i].data.dstr));
        }
    }
    PFMT("\n");
    return E_OK;
}

/* ACTUALLY:

   I think the best idea would be a reverse-linked list, so as you went down
   the stack you could append to it (without having to remove from it when
   you pop scope)

   */

/* well... now that I think of it that would only really be useful in rm_rf
   because in every other case that I open a file I know at compile time
   what the filename components are going to be */

/* well... let's just try and see how we like it */

/* update: I think this would be useful in quite a few situations, so I'm
   definitely going to do it. */

/*
   What if we made it so you could append or prepend at every step?

   Tree traversal example:

   a b c d e f

    a before:
        b before:
            f after:
                e after:
                    c before:
                        d
    traversal:
        a before -> print
        b before -> print
        f after -> push
            e after -> push
                c before -> print
                d -> print
            print
        print
   */

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

static derr_t sb_append_to_dstr(const string_builder_t* sb, dstr_t* out){
    if(sb->prev != NULL){
        PROP( sb_append_to_dstr(sb->prev, out) );
    }
    PROP( FMT(out, "%x", sb->elem) );
    if(sb->next != NULL){
        PROP( sb_append_to_dstr(sb->next, out) );
    }
    return E_OK;
}

static inline derr_t sb_to_dstr(const string_builder_t* sb, dstr_t* out){
    out->len = 0;
    PROP( sb_append_to_dstr(sb, out) );
    return E_OK;
}

int main(int argc, char** argv){
    (void)argc;
    (void)argv;

    DSTR_STATIC(dstat, "from_dstr");

    PRINT_PATH2(PS("asdf"), PS("zxcv"), PD(&dstat));

    print_path3(PATH_LIST(PS("---/"), PD(&dstat)));

    path_list_t pathvar = PATH_LIST(PS("path"),PS("var"));
    print_path3(pathvar);

    // openpath(PATH(PS(g_test_files), PS("asdf"), PD(&dval)), flags[, mode])
    // fopenpath(PATH(PS(g_test_files), PS("asdf"), PD(&dval)), mode)

    DSTR_VAR(out, 4096);
    string_builder_t sb = sb_append(NULL, FS("d"));
    string_builder_t sb1 = sb_append(&sb, FS("c"));
    string_builder_t sb2 = sb_prepend(&sb1, FS("b"));
    string_builder_t sb3 = sb_prepend(&sb2, FS("a"));
    string_builder_t sb4 = sb_append(&sb3, FS("e"));
    PROP( sb_to_dstr(&sb4, &out) );
    PFMT("out: %x\n", FD(&out));

    return 0;
}
