#include "libdstr/libdstr.h"

derr_t get_password(dstr_t* out);

derr_t get_string(dstr_t* out);

derr_t user_prompt(dstr_t prompt, dstr_t *resp, bool hide);

typedef derr_t (*user_prompt_fn)(dstr_t, dstr_t*, bool);

// multi-choice prompt, will enforce a valid response
/* the user_prompt_fn is a parameter so we can test prompt_one_of instead of
   mocking it */
derr_t prompt_one_of(
    user_prompt_fn upf, dstr_t prompt, const char* opts, size_t* ret
);
