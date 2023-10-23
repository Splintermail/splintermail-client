#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "console_input.h"
#include "libdstr/libdstr.h"

#ifdef _WIN32
#include <conio.h>
derr_t get_password(dstr_t* password){
    derr_t e = E_OK;
    DSTR_VAR(buf, 256);
    for(size_t i = 0; i < buf.size - 1; i++){
        // _getch() will not allow the character to be echoed
        // it will also not catch Ctrl-C characters
        // https://msdn.microsoft.com/en-us/library/078sfkak.aspx
        // also, this seems to totally fail on UTF-8/Unicode.  Ascii-only :(
        int c = _getch();
        // IDK why, but you have to call _getch() twice and throw away a zero:
        // WTF microsoft??
        _getch();
        // now we need to handle backspaces and newlines
        bool linebreak = false;
        switch(c){
            case '\b':
                if(i == 0){
                    // underflow should not cause problems here
                    i -= 1;
                }else{
                    // visually erase one asterisk
                    fprintf(stderr, "\b \b");
                    i -= 2;
                    buf.len -= 1;
                }
                break;
            case '\r':
                fprintf(stderr, "\n");
                linebreak = true;
                break;
            default:
                buf.data[i] = (char)c;
                buf.len += 1;
                fprintf(stderr, "*");
        }
        if(linebreak) break;
    }
    PROP(&e, dstr_copy(&buf, password) );
    PROP(&e, dstr_null_terminate(password) );
    return e;
}
#else
#include <termios.h>
derr_t get_password(dstr_t* password){
    derr_t e = E_OK;
    DSTR_VAR(buf, 256);
    int ret;
    tcflag_t old_lflag = 0; // CLANG wrongly thinks this might be uninitialized
    struct termios tios;
    bool ttymode = isatty(0);
    if(ttymode){
        // store terminal settings
        ret = tcgetattr(0, &tios);
        if(ret != 0){
            TRACE(&e, "%x: %x\n", FS("tcsetattr"), FE(errno));
            ORIG(&e, E_OS, "failed to get terminal settings");
        }
        // prepare to turn off echo
        old_lflag = tios.c_lflag;
        tios.c_lflag &= (tcflag_t)~ECHO;
        // turn off echo
        ret = tcsetattr(0, TCSANOW, &tios);
        if(ret != 0){
            TRACE(&e, "%x: %x\n", FS("tcsetattr"), FE(errno));
            ORIG(&e, E_OS, "failed to set terminal settings");
        }
    }

    // this seems logical, but I guess it is undefined behavior by C standard
    // fflush(stdin);

    // get password
    // this is a pretty safe cast
    char* s = fgets(buf.data, (int)buf.size, stdin);
    if(s != buf.data){
        ORIG_GO(&e, E_OS, "fgets failed", cu);
    }
    // remove the newline from the end of the password
    buf.len = strlen(buf.data) - 1;
    // print the newline that didn't get echoed
    PROP_GO(&e, FFMT(stderr, "\n"), cu);
    fflush(stderr);

    PROP_GO(&e, dstr_copy(&buf, password), cu);
    PROP_GO(&e, dstr_null_terminate(password), cu);

cu:
    if(ttymode){
        // reset terminal settings
        tios.c_lflag = old_lflag;
        ret = tcsetattr(0, TCSANOW, &tios);
        if(ret != 0){
            TRACE(&e, "%x: %x\n", FS("tcsetattr"), FE(errno));
            TRACE_ORIG(&e, E_OS, "failed to set terminal settings");
            DUMP(e);
            DROP_VAR(&e);
        }
    }
    return e;
}
#endif


derr_t get_string(dstr_t* out){
    derr_t e = E_OK;
    DSTR_VAR(buf, 1024);
    char* s = fgets(buf.data, (int)buf.size, stdin);
    if(s != buf.data){
        ORIG(&e, E_OS, "fgets failed");
    }
    buf.len = strnlen(buf.data, sizeof(buf));
    buf = dstr_rstrip_chars(buf, '\r', '\n');
    PROP(&e, dstr_copy(&buf, out) );
    PROP(&e, dstr_null_terminate(out) );
    return e;
}

derr_t user_prompt(dstr_t prompt, dstr_t *resp, bool hide){
    derr_t e = E_OK;

    PROP(&e, FFMT(stderr, "%x", FD(prompt)) );
    fflush(stderr);

    if(hide){
        PROP(&e, get_password(resp) );
    }else{
        PROP(&e, get_string(resp) );
    }

    return e;
}

// multi-choice prompt, will enforce a valid response
derr_t prompt_one_of(
    user_prompt_fn upf, dstr_t prompt, const char* opts, size_t* ret
){
    derr_t e = E_OK;

    size_t n = strlen(opts);

    DSTR_VAR(temp, 256);
    while(true){
        PROP(&e, upf(prompt, &temp, false) );
        if(temp.len == 1){
            // check if the character we got in response is a valid option
            char c = temp.data[0];
            for(size_t i = 0 ; i < n; i++){
                if(c != opts[i]) continue;
                *ret = i;
                return e;
            }
        }
        PROP(&e, FFMT(stderr, "Response must be one of [%x]\n", FS(opts)) );
    }
}
