# The Qwwq ("quick") Language: a templating language

Qwwq is a functional language designed for concise string operations.

Snippets of qwwq code can be embedded into template files between QW...WQ tags.

Then the `qwwq` command-line tool will read templates, evaluate snippets, and
embed the snippet results in the output.

## Execution Model

There is a config file, which represents immutable, global variables, that can
be referenced by the snippets in template files.

The frontend qwwq-code is compiled into a stack-based runtime for execution.

## Basic Expressions

| Expression             | Explanation                                       |
| ---------------------- | ------------------------------------------------- |
| `IDENT`                | a plain identifier references a key in the config |
| `"text"`               | a string literal                                  |
| `[a b c]`              | a list                                            |
| `{a=b c=d e=f}`        | a dict                                            |
| `mydict.IDENT`         | dereference the IDENT key of an dict object       |
| `func(a c=d -> a+b)`   | a function, with python-like args and kwargs      |
| `myfunc(a b=x)`        | call expr with python-like args and kwargs        |
| `if(a:b c:d e)`        | an if; if(a){b}elif(c){d}else{e}                  |
| `switch(x a:b c:d e)`  | a switch; if(a==x){b}elif(c==x){d}else{e}         |
| `[for a=x b=y -> a+b]` | a for loop; [x0+y0 x1+y1 ...]                     |
| `!expr`                | logical not                                       |
| `a%[b c d]`            | apply args (b,c,d) to fmt string a                |
| `a^[b c d]`            | join strings (b,c,d) with joiner a                |
| `a+b`                  | string concatenation                              |
| `a/b`                  | string concatenation, but with "/" in between     |
| `a == b`               | string equality operator                          |
| `a != b`               | string inequality operator                        |
| `a && b`               | logical and                                       |
| `a \|\| b`             | logical or                                        |
| `*expr`                | list-expander, allowed in lists and for loops     |
| `(expr)`               | parens can override normal order of operations    |
| `true`                 | literal boolean-type value                        |
| `false`                | literal boolean-type value                        |
| `null`                 | literal null-type value                           |
| `skip`                 | literal skip-type value (for lists and for loops) |
| `puke(expr)`           | stop execution with a reason given by `expr`      |

## Operator Precedence

From highest priority to lowest:

| Operator(s)    | Explanation                                           |
| -------------- | ----------------------------------------------------- |
| `(...)`        | order of operations parens                            |
| `expr.IDENT`   | dereference                                           |
| `expr(...)`    | function call parens (no space beween `expr` and `(`) |
| `!expr`        | logical not                                           |
| `a%b`, `a^b`   | fmt string, join string                               |
| `a+b`, `a/b`   | concatenation operators                               |
| `a==b`, `a!=b` | equality-like operators                               |
| `a && b`       | logical and                                           |
| `a \|\| b`     | logical or                                            |
| `*expr`        | expand expr (only in lists and for loops)             |

## Lists

Example list code:

    [a *b c]

Note that the expansion operator (`*`) will flatten its input value into the
list being generated.

Also note that expressions which evaluate to the special value `skip` will
not be included in the constructed list.

Example instructions:

    LIST.N <a> <b> * <c>

Note that this isn't a postfix operator because there's not a good way to
know how many values to pop from the stack at runtime, with the `*` operator.

## Dicts

A dict object is the combination of a compiler-defined keymap, which maps
string keys to integer indices, and a list of runtime values.

Additionally, dict keys are normally evaluated lazily unless they contain a
reference to a non-global parameter.

Example dict code:

    {a=f1("a") b=x}

Example instructions:

    lazy.6 global.f1 "a" call.1.0 lazy.2 global.x dict.{a=0,b=1}

Where "{a=0,b=1}" is the compile-time-defined keymap object.

When an expression inside a lazy object is discovered by the compiler to be a
reference to non-global parameter from outside the scope of the lazy, that lazy
object instruction and its arg get overwritten with noops:

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

### Keys with special characters

Dict keys may be string literals in the case that they are not valid
identifiers.

Example definition:

    {a="a" "my strange key"="strange"}

Example dereference:

    my_strange_dict."my strange key"

In case such a key exists in the root dict of the config, you can dereference
it by using the special builtin symbol `G` (for the "global" config variables)
so your dereference is not interpreted as a plain string literal:

    G."my strange root key"

## Functions

Example code:

    func(a b c=d e=f -> z^[a b d f])

Example instructions:

    <d> <f> func.ID.2.2.N.M a b c d <z^[a b d f]> binds...

Where:
- `<...>` means "instructions corresponding to `...`"
- and `.ID` is the unique scope id, determined by the compiler
- and `.2.2` means "two args, two kwargs"
- and `.N` means "N instructions in the body"
- and `.M` means "M binds after the body"
- and `a b c d` are arg names, known at compile time

Note that the `z` in the body would be replaced by a ref to this function's
scope, not to the parent function's.  Also note that `binds...` would contain
instructions for extracting `z` from the parent scope at the time that the func
instruction is executed (which is before the function is called).

## Function Calls

Example code:

     myfunc(x, b=y, e=z)

Example instructions:

    <myfunc> <x> <y> <z> call.1.2 b e

Where:
- `.2.1` means "one arg, two kwargs"
- and `b e` are the kwarg names, known at compile time

## If expressions

Example code:

    if(a:b c:d e)

Example instructions:

    <a> ifjmp.N <b> jmp.N <c> ifjmp.N <d> jmp.N <e>

Where:
- `ifjmp.N` is a special instruction that pops a bool from the stack, and
  either proceeds if true or jumps N instructions forward if not.
- `jmp.N` is intended to jump to the end of the whole control flow

## Switch expressions

Example code:

    switch(x a:b c:d e)

Example instructions:

    <x> swjmp.N <b> jmp.N <c> swjmp.N <d> jmp.N <e> swap drop

Where:
- `swjmp.N` is a special instruction that pops a val from the stack,
  peeks at the top of the stack, and if they are equal it proceeds,
  otherwise jumps N instructions forward.
- `jmp.N` is intended to jump to the swap drop, which should always execute.

## For loops

Example code:

    [for a=[1 2 3] b=[4 5 6] -> a+b]

Note that the for loop body can support the expansion operator (`*`) and
`skip` handling just like list expressions.

Example instructions:

    <[1 2 3]> <[4 5 6]> for.ID.2.N <a+b>

Note that since there's no keyword-based calling like functions, there's no
need to keep the names of the variables after compile time.

## Puke expressions

Example code:

    switch(os
        "windows":a
        "linux":b
        puke("os '%s' not supported"%[os])
    )

The puke statement is for stopping execution when a situation is detected that
the qwwq code is known not to handle.

## Builtin Functions

The qwwq language has a bunch of useful builtins, and its designed to be easy
to extended (at compile time) with additional custom bulitins written in C.

### `table(rows)`

`row` must be a list of lists of string cells.  `table` returns a
column-aligned representation of each row.  Notably, the individual cells may
contain multiple lines, but they should already be reflowed (perhaps with the
`STRING.wrap()` builtin).

### `relpath(path)`

Returns a `STRING` containing the `path` interpreted relative to the file being
processed.  If `path` is absolute, it will not be modified, but if it is
relative the result will be relative to the current working directory of the
`qwwq` command-line tool.

### `cat(path)`

Returns the unmodified contents of the file at `path`.  The `qwwq` command-line
tool interprets `path` according to its own current working directory.

### `exists(path)`

Returns a boolean if `path` exists.  The `qwwq` command-line tool interprets
`path` according to its own current working directory.

    // dict methods
    void qw_method_get(qw_env_t env, qw_dict_t *dict);

    // global builtins
    extern qw_func_t qw_builtin_table;
    extern qw_func_t qw_builtin_relpath;
    extern qw_func_t qw_builtin_cat;
    extern qw_func_t qw_builtin_exists;

## Builtin Methods

Many builtins act as methods on some type.

### `STRING.strip(chars="\r\n\t ")`

Returns the `STRING` but without any of the provided `chars` on the left or
right side.

### `STRING.lstrip(chars="\r\n\t ")`

Like `STRING.strip()` except it only strips chars from the left side.

### `STRING.rstrip(chars="\r\n\t ")`

Like `STRING.strip()` except it only strips chars from the right side.

### `STRING.upper()`

Returns the `STRING` but all upper-case.  Not utf8-smart.

### `STRING.lower()`

Returns the `STRING` but all lower-case.  Not utf8-smart.

### `STRING.wrap(width indent="" hang="")`

Returns the `STRING` but with paragraphs of text reflowed to match the provided
`width` (which is a string type in qwwq code, but is converted to an int in C
code).  The first line of each paragraph is indented by the `indent` string,
and each subsequent line of a paragraph is indented by the `hang` string.

### `STRING.pre(text split="\n" skip_empty=false)`

Returns the `STRING` but with each section prefixed with `text`.  Section
boundaries are defined by the `split` parameter.  If `skip_empty` is set to
`true` then empty sections are passed through untouched.

### `STRING.post(text split="\n" skip_empty=false)`

Like `pre()` but postfixes sections instead of prefixing them.

### `STRING.repl(find repl)`

Returns the `STRING` but with each instance of `find` replaced with `repl`.

### `STRING.lpad(width char=" ")`

Pad `STRING` with copies of `char` on the left until it is as at least `width`
characters long.  `char` must be a 1-character string.

### `STRING.rpad(width char=" ")`

Like `lpad` but pad on the right of the string.

### `DICT.get(key)`

Does a key lookup with a variable key, which the `.` operator does not support.
Missing keys will cause execution to fail.

## Builtin Symbols

### `G`

The symbol `G` is a reference to the config (the set of "global" variables).
This is only necessary for dereferencing special keys in the root of the config
dict:

    G."my strange key"
