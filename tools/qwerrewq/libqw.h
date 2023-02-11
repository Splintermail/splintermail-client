/*

The Qwerrewq Language: a templating language

    A template file may contain QWER...REWQ tags, and the tags and the
    qwerrewq-code snippet between them will be replaced by the evaluation of
    that snippet.

    Qwerrewq code is a functional language designed for concise string
    operations.

Execution Model:

    There is to be a config file, which represents immutable, global variables,
    that can be referenced by the snippets in template files.

    The frontent qwerrewq-code is compiled into a stack-based runtime for
    execution.

Basic expressions:

    - IDENT                a plain identifier references a key in the config
    - "text"               a string literal
    - [a b c]              a list
    - {a=b c=d e=f}        a dict
    - mydict.IDENT         dereference the IDENT key of an dict-type expression
    - func(a c=d -> a+b)   a function, with python-like args and kwargs
    - myfunc(a b=x)        call expr with python-like args and kwargs
    - if(a:b c:d e)        an if statement; if(a){b}elif(c){d}else{e}
    - switch(x a:b c:d e)  a switch statement; if(a==x){b}elif(c==x){d}else{e}
    - [for a=x b=y -> a+b] a for loop; [x0+y0 x1+y1 ...]
    - !expr                logical not
    - a%[b c d]            apply args (b,c,d) to fmt string a
    - a^[b c d]            join strings (b,c,d) with joiner a
    - a+b                  string concatenation
    - a/b                  string concatenation, but with "/" in between
    - a == b               string equality operator
    - a != b               string inequality operator
    - a && b               logical and
    - a || b               logical or
    - *expr                list-expander, allowed in lists and for loops
    - (expr)               parens can override normal order of operations

Normal order of operations:

    - (...)                order of operations parens
    - expr.IDENT           dereference
    - expr(...)            function call parens
    - !expr                logical not
    - a%b, a^b             fmt string, join string
    - a+b, a/b             concatenation operators
    - a==b, a!=b           equality-like operators
    - a && b               logical and
    - a || b               logical or
    - *expr                expand expr (only in list literals)

List objects:

    Example list code:

        [a *b c]

    Example instructions:

        LIST.N <a> <b> * <c>

    Note that this isn't a postfix operator, because there's not a good way to
    know how many values to pop from the stack at runtime, with the '*'
    operator.

Dict objects:

    A dict object is the combination of a compier-defined keymap, which maps
    string keys to integer indices, and a list of runtime values.

    Additionally, dict keys are normally evaluated lazily, unless they contain
    a reference to a non-global parameter.

    Example dict code:

        {a=f1("a") b=x}

    Example instructions:

        lazy.6 global.f1 "a" call.1.0 lazy.2 global.x dict.{a=0,b=1}

    Where "{a=0,b=1,c=2}" is the compile-time-defined keymap object.

    When an expression inside a lazy object is discovered by the compiler to be
    a reference to non-global parameter, that lazy object instruction and its
    arg get overwritten with noops:

    Example code:

        func(x -> {a="a" b={c="c" d={e="e" f=x}}} )

    Non-reference-aware instructions:

        lazy 1 "a" lazy 17
            lazy 1 "c" lazy 11
                lazy 1 "e" lazy 2 ref x dict {e=0,f=1}
            dict {c=0,d=1}
        dict {a=0,b=1}

    Actual compiled instructions:

        lazy 1 "a" noop noop
            lazy 1 "c" noop noop
                lazy 1 "e" noop noop ref x dict {e=0,f=1}
            dict {c=0,d=1}
        dict {a=0,b=1}

Functions:

    Example function code:

        func(a b c=d e=f -> z^[a b d f])

    Example instructions:

        <d> <f> func.ID.2.2.N.M a b c d <z^[a b d f]> binds...

    Where:
    - <...> means "instructions corresponding to ..."
    - and .ID is the unique scope id, determined by the compiler
    - and .2.2 means "two args, two kwargs"
    - and .N means "N instructions in the body"
    - and .M means "M binds after the body"
    - and "a b c d" are arg names, known at compile time

    Note that the z in the body would be replaced by a ref to this function's
    scope, not to the parent function's.  Also note that binds... would contain
    instructions for extracting z from the parent scope at the time that the
    func instruction is executed (which is before the function is called).

Function calls:

    Example call:

         myfunc(x, b=y, e=z)

    Example call instructions:

        <myfunc> <x> <y> <z> call.1.2 b e

    Where:
    - .2.1 means "one arg, two kwargs"
    - and "b e" are the kwarg names, known at compile time

If Statements:

    Example if-statement code:

        if(a:b c:d e)

    Example instructions:

        <a> ifjmp.N <b> jmp.N <c> ifjmp.N <d> jmp.N <e>

    Where:
    - ifjmp.N is a special instruction that pops a bool from the stack, and
      either proceeds if true or jumps N instructions forward if not.
    - jmp.N is intended to jump to the end of the whole control flow

Switch Statements:

    Example code:

        switch(x a:b c:d e)

    Example instructions:

        <x> swjmp.N <b> jmp.N <c> swjmp.N <d> jmp.N <e> swap drop

    Where:
    - swjmp.N is a special instruction that pops a val from the stack,
      peeks at the top of the stack, and if they are equal it proceeds,
      otherwise jumps N instructions forward.
    - jmp.N is intended to jump to the swap drop, which should always execute.

For loops:

    Example code:

        [for a=[1 2 3] b=[4 5 6] -> a+b]

    Example instructions:

        <[1 2 3]> <[4 5 6]> for.ID.2.N <a+b>

    Note that since there's no keyword-based calling like functions, there's no
    need to keep the names of the variables after compile time.
*/

#include "libdstr/libdstr.h"

#include <setjmp.h>

#define QWBLOCKSIZE 16384
#define QWMAXPARAMS 256
#define PTRSIZE sizeof(void*)

struct qw_engine_t;
typedef struct qw_engine_t qw_engine_t;

struct qw_comp_call_t;
typedef struct qw_comp_call_t qw_comp_call_t;

struct qw_comp_scope_t;
typedef struct qw_comp_scope_t qw_comp_scope_t;

// an instruction function takes only the engine and returns nothing
typedef void (*qw_instr_f)(qw_engine_t *engine);

#include "tools/qwerrewq/types.h"
#include "tools/qwerrewq/instr.h"
#include "tools/qwerrewq/compile.h"
#include <tools/qwerrewq/generated/parse.h>
#include "tools/qwerrewq/scan.h"
#include "tools/qwerrewq/engine.h"
#include "tools/qwerrewq/qw.h"
