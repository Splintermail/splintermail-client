#ifndef OPTPARSE_H
#define OPTPARSE_H

#include "common.h"

#define OPT_RETURN_INIT 0, {.data=NULL, .size=0, .len=0, .fixed_size=true}

/*
in: argc
    argv
    {h,  help,     no_arg,  OPT_RETURN_INIT}
    {p,  port,     yes_arg, OPT_RETURN_INIT}
    {\0, gpg-home, yes_arg, OPT_RETURN_INIT}

out: set values in the above list of structs
     rearrange argv
     return new argv and argc for just arguments
*/

typedef struct {
    char oshort;
    char* olong;
    char val_req; // boolean (0 or 1)
    int found;    /* value is 0 if not found, or >1 if found.  A higher number
                     means it was found later.  If found multiple times, the
                     value stored will be from the last instance of the option
                     in the list. */
    dstr_t val;
} opt_spec_t;

derr_t opt_parse(
        int argc, char* argv[], opt_spec_t* spec[], size_t speclen,
        int* newargc);

derr_t conf_parse(const dstr_t* text, opt_spec_t* spec[], size_t speclen);

derr_t opt_dump(opt_spec_t* spec[], size_t speclen, dstr_t* out);
derr_t opt_fdump(opt_spec_t* spec[], size_t speclen, FILE* f, size_t* len);

#endif // OPTPARSE_H
