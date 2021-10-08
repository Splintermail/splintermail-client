import gen

# illegal: first term might start with X or be empty, second term starts with X
try:
    g = gen.Grammar()
    X = g.token("X")

    @g.expr
    def a(e):
        with e.maybe():
            e.match(X)
        e.match(X)

    g.check()
    1/0
except gen.FirstFollow as e:
    pass

# same thing, but with a term in the middle
try:
    g = gen.Grammar()
    X = g.token("X")
    Y = g.token("Y")

    @g.expr
    def a(e):
        with e.maybe():
            e.match(X)
        with e.zero_or_more():
            e.match(Y)
        e.match(X)

    g.check()
    1/0
except gen.FirstFollow as e:
    pass

# same thing, but in different scopes
try:
    g = gen.Grammar()
    X = g.token("X")
    Y = g.token("Y")

    @g.expr
    def a(e):
        with e.maybe():
            e.match(X)

    @g.expr
    def b(e):
        with e.zero_or_more():
            e.match(Y)

    @g.expr
    def c(e):
        e.match(X)

    @g.expr
    def d(e):
        e.match(a)
        e.match(b)
        e.match(c)

    g.check()
    1/0
except gen.FirstFollow as e:
    pass


# Unit tests
g = gen.Grammar()
W = g.token("W")
X = g.token("X")
Y = g.token("Y")
Z = g.token("Z")

@g.expr
def a(e):
    with e.maybe():
        e.match(W)
    with e.maybe():
        e.match(X)
    with e.maybe():
        e.match(Y)
    with e.maybe():
        e.match(Z)

@g.expr
def b(e):
    with e.maybe():
        e.match(W)
    with e.maybe():
        e.match(X)
    e.match(Y)
    with e.maybe():
        e.match(Z)

assert a.get_first() == {"W", "X", "Y", "Z", None}
assert a.get_disallowed_after() == {"W", "X", "Y", "Z", None}

assert b.get_first() == {"W", "X", "Y"}
assert b.get_disallowed_after() == {"Z"}

# testing MetaTokenizer
text = r"{{{ hi {{hello}} bye }}}"
tokens = [t.type for t in gen.Tokenizer().iter(text)]
assert tokens == [gen.CODE, gen.EOF], tokens


def run_e2e_test(grammar_text):
    with open("test.c", "w") as f:
        gen.gen(grammar_text, 'c', f)

    from subprocess import Popen, PIPE
    import os

    cc = "gcc"
    asan = True
    if asan:
        cflags=("-g", "-fsanitize=address", "-fno-omit-frame-pointer")
        p = Popen([cc, *cflags, "test.c"])
        assert p.wait() == 0, p.wait()

        p = Popen(["./a.out"])
        assert p.wait() == 0, p.wait()
        os.remove("a.out")
        os.remove("test.c")
    else:
        cflags=("-g",)
        p = Popen([cc, *cflags, "test.c"])
        assert p.wait() == 0, p.wait()

        cmd = (
            "valgrind",
            "--quiet",
            "--leak-check=full",
            "--show-leak-kinds=all",
            "--errors-for-leak-kinds=all",
            "--error-exitcode=255",
            "./a.out",
        )
        p = Popen(cmd)
        assert p.wait() == 0, p.wait()
        os.remove("test.c")
        os.remove("a.out")

# test the C generator; make sure everything is always freed, with a grammar
# whose side effects never free anything
run_e2e_test(r"""
{{
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

char *mydup(char *in, char *tag){
    char *out = malloc(strlen(in) + 1 + strlen(tag) + 1);
    // sprintf(out, "%s.%s", in, tag);
    sprintf(out, "%s", in);
    // printf("expect to free '%s'\n", out);
    return out;
}

char *myappend(char *a, char joiner, char *b, char *tag){
    // printf("freeing %s (by realloc)\n", a);
    a = realloc(a, strlen(a) + 1 + strlen(b) + 1 + strlen(tag) + 1);
    // sprintf(a + strlen(a), "%c%s.%s", joiner, b, tag);
    sprintf(a + strlen(a), "%c%s", joiner, b);
    // printf("expect to free '%s'\n", a);
    return a;
}
}}

%type str {char*} {/*printf("freeing %s\n", $$);*/ free($$);};
%root sum;

STR:str;

sum:str =
    diff:a {$$ = mydup($a, "1");}
    *( PLUS diff:b { $$ = myappend($$, '+', $b, "2"); } )
    _EOF
;

diff:str =
    STR:a {$$ = mydup($a, "3");}
    *( MINUS STR:b { $$ = myappend($$, '-', $b, "4"); } )
;

{{
int main(int argc, char **argv){
    ONSTACK_PARSER(p, 2, 7);

    struct {int tok; char *str;} tokens[] = {
        {STR, mydup("QWER", "a")},
        {PLUS},
        {STR, mydup("ASDF", "b")},
        {PLUS},
        {STR, mydup("ZXVC", "c")},
        {MINUS},
        {STR, mydup("qwer", "d")},
        {MINUS},
        {STR, mydup("asdf", "e")},
        {MINUS},
        {STR, mydup("zxvc", "f")},
        {_EOF, NULL},
    };
    size_t ntokens = sizeof(tokens)/sizeof(*tokens);

    char *out;
    status_t status = STATUS_OK;
    size_t i = 0;
    while(!status && i < ntokens){
        // printf("feeding %s (%s)\n", token_name(tokens[i].tok), tokens[i].str);
        status = parse_sum(
            &p, tokens[i].tok, (val_t){.str=tokens[i].str}, &out
        );
        i++;
    }

    if(i != ntokens || status != STATUS_DONE){
        fprintf(stderr, "bad exit conditions %d\n", (int)status);
        return 1;
    }

    char *exp = "QWER+ASDF+ZXVC-qwer-asdf-zxvc";
    int ret = !!strcmp(out, exp);
    if(ret){
        fprintf(stderr, "expected: %s\n", exp);
        fprintf(stderr, "but got : %s\n", out);
    }
    free(out);

    return ret;
}
}}
""")

# Run a similar test, only this time the side-effects always free semantic
# values, so we're checking for double-frees.
run_e2e_test(r"""
{{
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

char *steal(char **in){
    char *out = *in;
    *in = NULL;
    return out;
}

char *myappend(char *a, char joiner, char *b){
    a = realloc(a, strlen(a) + 1 + strlen(b) + 1);
    sprintf(a + strlen(a), "%c%s", joiner, b);
    free(b);
    return a;
}
}}

%type str {char*} {free($$);};
%root sum;

STR:str;

sum:str =
    diff:a {$$ = steal(&$a);}
    *( PLUS diff:b { $$ = myappend($$, '+', steal(&$b)); } )
    _EOF
;

diff:str =
    STR:a {$$ = steal(&$a);}
    *( MINUS STR:b { $$ = myappend($$, '-', steal(&$b)); } )
;

{{
int main(int argc, char **argv){
    ONSTACK_PARSER(p, 2, 7);

    struct {int tok; char *str;} tokens[] = {
        {STR, strdup("QWER")},
        {PLUS},
        {STR, strdup("ASDF")},
        {PLUS},
        {STR, strdup("ZXVC")},
        {MINUS},
        {STR, strdup("qwer")},
        {MINUS},
        {STR, strdup("asdf")},
        {MINUS},
        {STR, strdup("zxvc")},
        {_EOF, NULL},
    };
    size_t ntokens = sizeof(tokens)/sizeof(*tokens);

    char *out;
    status_t status = STATUS_OK;
    size_t i = 0;
    while(!status && i < ntokens){
        // printf("feeding %s (%s)\n", token_name(tokens[i].tok), tokens[i].str);
        status = parse_sum(
            &p, tokens[i].tok, (val_t){.str=tokens[i].str}, &out
        );
        i++;
    }

    if(i != ntokens || status != STATUS_DONE){
        fprintf(stderr, "bad exit conditions %d\n", (int)status);
        return 1;
    }

    char *exp = "QWER+ASDF+ZXVC-qwer-asdf-zxvc";
    int ret = !!strcmp(out, exp);
    if(ret){
        fprintf(stderr, "expected: %s\n", exp);
        fprintf(stderr, "but got : %s\n", out);
    }
    free(out);

    return ret;
}
}}
""")

print("PASS")
