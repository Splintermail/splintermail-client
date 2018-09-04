#ifndef CONSOLE_INPUT_C
#define CONSOLE_INPUT_C

#include "common.h"

// this is really only its own module to make testing the UI easier

derr_t get_password(dstr_t* out);

derr_t get_string(dstr_t* out);

#endif // CONSOLE_INPUT_C
