/*
in: argc
    argv
    {h,  help,     false }
    {p,  port,     true  }
    {\0, gpg-home, true  }

out: set values in the above list of structs
     rearrange argv
     return new argv and argc for just positional arguments
*/

// if val_req == false, val will be (dstr_t){0}
typedef derr_t (*opt_spec_cb)(void *cb_data, dstr_t val);

typedef struct {
    char oshort;
    char* olong;
    bool val_req;
    opt_spec_cb cb;
    void *cb_data;
    /* found will be 0 if the option is not detected, or >1 if it was.  A
       higher number means it was found later.  If an option was provided
       multiple times, the value stored will be from the last appearance. */
    int found;
    // a count of the number of times an option was seen
    int count;
    dstr_t val;
} opt_spec_t;

derr_t opt_parse_ex(
    int argc,
    char* argv[],
    opt_spec_t* spec[],
    size_t speclen,
    int* newargc,
    bool allow_unrecognized
);

derr_t opt_parse(
    int argc, char* argv[], opt_spec_t* spec[], size_t speclen, int* newargc
);

derr_t opt_parse_soft(
    int argc, char* argv[], opt_spec_t* spec[], size_t speclen, int* newargc
);

derr_t conf_parse(const dstr_t* text, opt_spec_t* spec[], size_t speclen);
