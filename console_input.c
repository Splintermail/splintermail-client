#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "console_input.h"
#include "libdstr/libdstr.h"

#ifdef _WIN32
#include <conio.h>
derr_t get_password(dstr_t* password){
    derr_t e = E_OK;
    // empty the password
    password->len = 0;
    for(size_t i = 0; i < password->size - 1; i++){
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
                    password->len -= 1;
                }
                break;
            case '\r':
                fprintf(stderr, "\n");
                linebreak = true;
                break;
            default:
                password->data[i] = (char)c;
                password->len += 1;
                fprintf(stderr, "*");
        }
        if(linebreak) break;
    }
    PROP(& dstr_null_terminate(password) );
    return e;
}
#else
#include <termios.h>
derr_t get_password(dstr_t* password){
    derr_t e = E_OK;
    int ret;
    tcflag_t old_lflag = 0; // CLANG wrongly thinks this might be uninitialized
    struct termios tios;
    bool ttymode = isatty(0);
    if(ttymode){
        // store terminal settings
        ret = tcgetattr(0, &tios);
        if(ret != 0){
            TRACE(&e, "%x: %x\n", FS("tcsetattr"), FE(&errno));
            ORIG(&e, E_OS, "failed to get terminal settings");
        }
        // prepare to turn off echo
        old_lflag = tios.c_lflag;
        tios.c_lflag &= (tcflag_t)~ECHO;
        // turn off echo
        ret = tcsetattr(0, TCSANOW, &tios);
        if(ret != 0){
            TRACE(&e, "%x: %x\n", FS("tcsetattr"), FE(&errno));
            ORIG(&e, E_OS, "failed to set terminal settings");
        }
    }

    // this seems logical, but I guess it is undefined behavior by C standard
    // fflush(stdin);

    // get password
    // this is a pretty safe cast
    char* s = fgets(password->data, (int)password->size, stdin);
    if(s != password->data){
        ORIG(&e, E_OS, "fgets failed");
    }
    // remove the newline from the end of the password
    password->len = strlen(password->data) - 1;
    // print the newline that didn't get echoed
    PROP(&e, FFMT(stderr, NULL, "\n") );
    fflush(stderr);

    if(ttymode){
        // reset terminal settings
        tios.c_lflag = old_lflag;
        ret = tcsetattr(0, TCSANOW, &tios);
        if(ret != 0){
            TRACE(&e, "%x: %x\n", FS("tcsetattr"), FE(&errno));
            ORIG(&e, E_OS, "failed to set terminal settings");
        }
    }
    return e;
}
#endif


derr_t get_string(dstr_t* out){
    derr_t e = E_OK;
    char* s = fgets(out->data, (int)out->size, stdin);
    if(s != out->data){
        ORIG(&e, E_OS, "fgets failed");
    }
    out->len = strlen(out->data) - 1;
    return e;
}
