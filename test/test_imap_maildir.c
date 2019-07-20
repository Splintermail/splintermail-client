#include <common.h>
#include <logger.h>
#include <imap_maildir.h>

#include "test_utils.h"

// path to where the test files can be found
const char* g_test_files;

// prints a leading space
static void print_mflag_list(ie_mflags_t mflags){
    if(mflags.noinferiors){ LOG_ERROR(" \\NoInferiors"); };
    switch(mflags.selectable){
        case IE_SELECTABLE_NONE: break;
        case IE_SELECTABLE_NOSELECT: LOG_ERROR(" \\Noselect"); break;
        case IE_SELECTABLE_MARKED:   LOG_ERROR(" \\Marked"); break;
        case IE_SELECTABLE_UNMARKED: LOG_ERROR(" \\Unmarked"); break;
    }
    for(ie_dstr_t *d = mflags.extensions; d != NULL; d = d->next){
        LOG_ERROR(" \\%x", FD(&d->dstr));
    }
}

static derr_t print_folders(imaildir_t *m, size_t indent){
    derr_t e = E_OK;
    DSTR_STATIC(spaces, "                                                   ");
    dstr_t pre = dstr_sub(&spaces, 0, indent);
    if(indent){
        PROP(&e, PFMT("%x%x", FD(&pre), FD(&m->name)) );
    }else{
        PROP(&e, PFMT("%x%x", FD(&m->name)) );
    }
    print_mflag_list(m->mflags);
    PROP(&e, PFMT("\n") );
    hashmap_iter_t i;
    for(i = hashmap_first(&m->children); i.more; hashmap_next(&i)){
        PROP(&e, print_folders((imaildir_t*)i.data, indent + 2) );
    }
    return e;
}

static derr_t test_imaildir_open(void){
    derr_t e = E_OK;
    string_builder_t files = sb_append(NULL, FS(g_test_files));
    string_builder_t path = sb_append(&files, FS("imap_maildir"));
    // allocate new imaildir
    imaildir_t *m;
    PROP(&e, imaildir_new(&m, &path, &DSTR_LIT("a")));

    PROP_GO(&e, print_folders(m, 0), cu_m);

cu_m:
    imaildir_free(m);
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, &g_test_files, LOG_LVL_WARN);

    PROP_GO(&e, test_imaildir_open(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
