#include <string.h>
#include <ctype.h>

#include "libdstr.h"

// terminology used in variable names:
// progname arg --something --something-else seval
//          ^     ^           ^              ^ this is a "value"
//          ^     ^           ^ this is an "option" that requires a value
//          ^     ^ this is an "option" that requires no value (ie, "flag")
//          ^ this is a "normal" argument (every token is an "argument")

static void to_end(int argc, char* argv[], int index, int count){
    // save old values
    char* one = argv[index];
    char* two = count == 2 ? argv[index + 1] : NULL;
    // left-shift remainder of array
    memmove(&argv[index], &argv[index + count],
            (size_t)(argc - index - count) * sizeof(*argv) );
    // restore old values to the end
    argv[argc - 1] = two;
    argv[argc - count] = one;
}

static derr_t maybe_cb(opt_spec_t *spec){
    derr_t e = E_OK;

    if(!spec->cb) return e;

    PROP(&e, spec->cb(spec->cb_data, spec->val) );

    return e;
}

static int max_found(opt_spec_t *spec[], size_t speclen){
    int max = 0;
    for(size_t i = 0; i < speclen; i++){
        if(spec[i]->found > max){
            max = spec[i]->found;
        }
    }
    return max;
}

// labels on the left like /*1*/ are labeled codepaths (to organize testing)

derr_t opt_parse_ex(
    int argc,
    char* argv[],
    opt_spec_t* spec[],
    size_t speclen,
    int* newargc,
    bool allow_unrecognized
){
    derr_t e = E_OK;

    int num_shifted = 0;
    int idx = 0;
    // start higher than the highest found order
    int found_order = max_found(spec, speclen) + 1;
    // look at each argument
    while(idx + num_shifted < argc){
        size_t len = strlen(argv[idx]);
        // if its an empty string or doesn't start with a '-', skip it
        if(len == 0 || argv[idx][0] != '-'){
            idx++;
/*1*/       continue;
        }
        // also skip a single '-'
        if(len == 1){
            idx++;
/*2*/       continue;
        }
        // now check for a long option
        if(argv[idx][1] == '-'){
            // well, if it's just "--" shift it to the end and quit
            if(len == 2){
                to_end(argc, argv, idx, 1);
                num_shifted++;
/*3*/           break;
            }
            // get a pointer to the long option
            char* optname = &argv[idx][2];
            // look for the matching entry in our spec
            for(size_t j = 0; j < speclen; j++){
                // make sure a long option is specified, and see if it matches
                if(spec[j]->olong && strcmp(optname, spec[j]->olong) == 0){
                    // found a a matching option
                    spec[j]->found = found_order++;
                    spec[j]->count++;
                    // check if it requires a value
                    if(spec[j]->val_req){
                        // make sure there is another argument
                        if(idx + num_shifted + 1 < argc){
                            // set spec.val to next argument
                            spec[j]->val = dstr_from_cstr(argv[idx+1]);
                            PROP(&e, maybe_cb(spec[j]) );
                            // shift both arguments to end of list
                            to_end(argc, argv, idx, 2);
                            num_shifted += 2;
/*4*/                       goto next_arg;
                        }
                        // throw missing value error if there isn't another arg
                        TRACE(&e,"Option --%x missing value\n", FS(optname));
/*5*/                   ORIG(&e, E_VALUE, "option missing value");
                    }
                    PROP(&e, maybe_cb(spec[j]) );
                    // or if the option doesn't need a value, just shift it:
                    to_end(argc, argv, idx, 1);
                    num_shifted++;
/*6*/               goto next_arg;
                }
            }
            // if we are here, we didn't find a match
            if(allow_unrecognized){
                idx++;
                goto next_arg;
            }
            TRACE(&e, "Unrecongnized option: --%x\n", FS(optname));
/*7*/       ORIG(&e, E_VALUE, "unrecognized option");
         }
        // if we are here, its a short option
        // parse through each character
        for(size_t cc = 1; cc < len; cc++){
            char c = argv[idx][cc];
            char opt_found = 0;
            // look for the matching entry in our spec
            for(size_t j = 0; j < speclen; j++){
                if(c == spec[j]->oshort){
                    // found our match
                    spec[j]->found = found_order++;
                    spec[j]->count++;
                    // check if it requires a value
                    if(spec[j]->val_req){
                        // if text remains in the arg, the value is attached
                        if(cc + 1 < len){
                            // set spec.val to remaining text
                            spec[j]->val = dstr_from_cstr(&argv[idx][cc+1]);
                            PROP(&e, maybe_cb(spec[j]) );
                            // shift argument
                            to_end(argc, argv, idx, 1);
                            num_shifted++;
/*8*/                       goto next_arg;
                        }
                        // if no text remains, the next argument is the value
                        if(idx + num_shifted + 1 < argc){
                            // set spec.val to next argument
                            spec[j]->val = dstr_from_cstr(argv[idx+1]);
                            PROP(&e, maybe_cb(spec[j]) );
                            // shift both arguments to end of list
                            to_end(argc, argv, idx, 2);
                            num_shifted += 2;
/*9*/                       goto next_arg;
                        }
                        // throw missing value error if there isn't another arg
                        TRACE(&e, "Option -%x missing value\n", FC(c));
/*A*/                   ORIG(&e, E_VALUE, "option missing value");
                    }
                    PROP(&e, maybe_cb(spec[j]) );
                    // if no value is required, continue with next character
                    opt_found = 1;
/*B*/               break;
                }
            }
            // make sure we found a match on the last character
            if(!opt_found){
                if(allow_unrecognized){
                    idx++;
                    goto next_arg;
                }
                TRACE(&e, "Unrecognized option: -%x\n", FC(c));
/*C*/           ORIG(&e, E_VALUE, "unrecognized option");
            }
        }
        // if we get here, it is after an arg with short options and no value
        // so shift just once
        to_end(argc, argv, idx, 1);
/*D*/   num_shifted++;

// this label is necessary for breaking out of certain double loops
// it also makes the control flow a a little clearer
next_arg:
        continue;
    }
    // before returning, set newargc
    *newargc = argc - num_shifted;
    return e;
}

derr_t opt_parse(
    int argc, char* argv[], opt_spec_t* spec[], size_t speclen, int* newargc
){
    return opt_parse_ex(argc, argv, spec, speclen, newargc, false);
}

derr_t opt_parse_soft(
    int argc, char* argv[], opt_spec_t* spec[], size_t speclen, int* newargc
){
    return opt_parse_ex(argc, argv, spec, speclen, newargc, true);
}


derr_t conf_parse(const dstr_t* text, opt_spec_t* spec[], size_t speclen){
    derr_t e = E_OK;
    // the first non-space character
    bool have_first_nsp = false;
    char* first_nsp = NULL; // init this so clang doesn't complain falsely
    // the most recent non-space character
    char* last_nsp = NULL; // init this so clang doesn't complain falsely
    bool in_comment = false;
    // have we found the name of the option yet
    char* opt = NULL;  // init this so clang doesn't complain falsely
    size_t optlen = 0; // init this so clang doesn't complain falsely
    bool have_opt = false;
    int found_order = max_found(spec, speclen) + 1;

    for(size_t i = 0; i < text->len; i++){
        // check if the character is a space
        bool issp = (isspace(text->data[i]) != 0);

        // check for non-space
        if(issp == false){
            // start of a comment?
            if(
                text->data[i] == '#'
                && have_first_nsp == false
                && have_opt == false
            ){
                in_comment = true;
                continue;
            }
            // first character?
            else if(have_first_nsp == false){
                first_nsp = &text->data[i];
                have_first_nsp = true;
            }
            last_nsp = &text->data[i];
            continue;
        }

        // check for the end of the option name
        if(issp == true && have_first_nsp == true && have_opt == false){
            opt = first_nsp;
            optlen = (size_t)(last_nsp - first_nsp) + 1;
            have_opt = true;
            have_first_nsp = false;
        }

        // check for end of line
        if(text->data[i] == '\n' || text->data[i] == '\r'){
            if(in_comment){
                in_comment = false;
            }else{
                // handle the interpretation of the line

                // first, ignore empty lines
                if(have_opt == false){
                    continue;
                }

                // wrap the option we found in a dstr_t
                dstr_t dopt;
                DSTR_WRAP(dopt, opt, optlen, false);

                // then, make sure the option we found is a real option
                opt_spec_t* s = NULL;
                for(size_t j = 0; j < speclen; j++){
                    if(dstr_eq(dstr_from_cstr(spec[j]->olong), dopt)){
                        s = spec[j];
                        break;
                    }
                }
                if(s == NULL){
                    TRACE(&e, "Unrecongnized option: %x\n", FD(dopt));
/*1*/               ORIG(&e, E_VALUE, "error in config file");
                }

                if(have_first_nsp == true){
                    // option with a value
                    if(s->val_req == false){
                        TRACE(&e, "Option \"%x\" does not require a value\n",
                              FD(dopt));
/*2*/                   ORIG(&e, E_VALUE, "error in config file");
                    }
                    s->found = found_order++;
                    s->count++;
                    size_t vallen = (size_t)(last_nsp - first_nsp) + 1;
                    DSTR_WRAP(s->val, first_nsp, vallen, false);
                    PROP(&e, maybe_cb(s) );
                }else if(have_first_nsp == false){
                    // option with no value
                    if(s->val_req == true){
                        TRACE(&e, "Option \"%x\" requires a value\n",
                              FD(dopt));
/*3*/                   ORIG(&e, E_VALUE, "error in config file");
                    }
                    s->found = found_order++;
                    s->count++;
                    PROP(&e, maybe_cb(s) );
                }
            }
            have_opt = false;
            have_first_nsp = false;
            continue;
        }
    }
    return e;
}
