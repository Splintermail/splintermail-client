import sys
if "" not in sys.path:
    sys.path = [""] + sys.path
import gen

# illegal: first term might start with X or be empty, second term starts with X
try:
    g = gen.Grammar()
    X = g.token("X")

    @g.expr
    def a(e):
        with e.repeat(0, 1):
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
        with e.repeat(0, 1):
            e.match(X)
        with e.repeat(0, None):
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
        with e.repeat(0, 1):
            e.match(X)

    @g.expr
    def b(e):
        with e.repeat(0, None):
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

# same thing, but with a non-zero-rmin repeat
try:
    g = gen.Grammar()
    X = g.token("X")

    @g.expr
    def a(e):
        with e.repeat(4, 5):
            e.match(X)
        e.match(X)

    g.check()
    1/0
except gen.FirstFollow as e:
    pass

# fixed-number repeats do not cause an no issue
g = gen.Grammar()
X = g.token("X")

@g.expr
def a(e):
    with e.repeat(5, 5):
        e.match(X)
    e.match(X)

g.check()


# Unit tests
g = gen.Grammar()
W = g.token("W")
X = g.token("X")
Y = g.token("Y")
Z = g.token("Z")

@g.expr
def a(e):
    with e.repeat(0, 1):
        e.match(W)
    with e.repeat(0, 1):
        e.match(X)
    with e.repeat(0, 1):
        e.match(Y)
    with e.repeat(0, 1):
        e.match(Z)

@g.expr
def b(e):
    with e.repeat(0, 1):
        e.match(W)
    with e.repeat(0, 1):
        e.match(X)
    e.match(Y)
    with e.repeat(0, 1):
        e.match(Z)

assert a.seq.get_first() == {"W", "X", "Y", "Z", None}
assert a.seq.get_disallowed_after() == {"W", "X", "Y", "Z", None}

assert b.seq.get_first() == {"W", "X", "Y"}
assert b.seq.get_disallowed_after() == {"Z"}


# testing MetaTokenizer
text = r"{{{ hi {{hello}} bye }}}"
tokens = [t.type for t in gen.Tokenizer().iter(text)]
assert tokens == [gen.CODE, gen.EOF], tokens


# test extract_fallbacks (tree expansion)
grammar_text = """
%fallback LETTER A B C;
%fallback WORD LETTER;
asdf = WORD LETTER A B C;
"""
parsed_doc = gen.parse_doc(grammar_text)
g = parsed_doc.build_grammar(grammar_text)
exp = {
    "LETTER": {"A", "B", "C"},
    "WORD": {"LETTER", "A", "B", "C"},
    "A": set(), "B": set(), "C": set(),
}
fallbackmap = gen.extract_fallbacks(parsed_doc.fallbacks, g, None)
assert fallbackmap == exp, '\ngot: ' + str(got) + '\nexp: ' + str(exp)
fallbacks = gen.Fallbacks(fallbackmap)
got = fallbacks.fallback_set("WORD", ["WORD"])
assert got == {"LETTER", "A", "B", "C"}, got
got = fallbacks.fallback_set("WORD", ["WORD", "A"])
assert got == {"LETTER", "B", "C"}, got
got = fallbacks.fallback_set("A", ["WORD", "A"])
assert got == set(), got
got = fallbacks.fallback_set("WORD", ["WORD", "LETTER"])
assert got == set(), got
got = fallbacks.fallback_set("LETTER", ["WORD", "LETTER"])
assert got == {"A", "B", "C"}, got

# test all_fallbacks (tree expansion)
grammar_text = """
%fallback LETTER A B C;
%fallback NUM N1 N2 N3;
%fallback WORD LETTER NUM;
thing1 = LETTER | N2;
thing2 = NUM | A;
thing3 = WORD | C;
asdf = (thing1 | thing2 | thing3) B N1 N3;
"""
parsed_doc = gen.parse_doc(grammar_text)
g = parsed_doc.build_grammar(grammar_text)
fallbackmap = gen.extract_fallbacks(parsed_doc.fallbacks, g, None)
exp = {
    "LETTER": {"A", "B", "C"},
    "NUM": {"N1", "N2", "N3"},
    "WORD": {"LETTER", "A", "B", "C", "NUM", "N1", "N2", "N3"},
    "A": set(), "B": set(), "C": set(), "N1": set(), "N2": set(), "N3": set(),
}
assert fallbackmap == exp, fallbackmap
fallbacks = gen.Fallbacks(fallbackmap)
# first look at thing1, thing2, and thing3 directly
thing1 = g.exprs["thing1"].seq.get_first()
thing2 = g.exprs["thing2"].seq.get_first()
thing3 = g.exprs["thing3"].seq.get_first()
got = fallbacks.all_fallbacks(thing1)
assert got == {"A", "B", "C"}, got
got = fallbacks.all_fallbacks(thing2)
assert got == {"N1", "N2", "N3"}, got
got = fallbacks.all_fallbacks(thing3)
assert got == {"LETTER", "A", "B", "NUM", "N1", "N2", "N3"}, got
# now imagine we are looking at the branch statement in `asdf`
exclude = g.exprs["asdf"].seq.get_first()
got = fallbacks.all_fallbacks(thing1, exclude)
assert got == {"B"}, got
got = fallbacks.all_fallbacks(thing2, exclude)
assert got == {"N1", "N3"}, got
got = fallbacks.all_fallbacks(thing3, exclude)
assert got == set(), got

# test extract_fallbacks (multi-parent rejection)
grammar_text = """
%fallback LETTER A B C;
%fallback WORD LETTER C;
asdf = WORD LETTER A B C;
"""
parsed_doc = gen.parse_doc(grammar_text)
g = parsed_doc.build_grammar(grammar_text)
try:
    raise ValueError(gen.extract_fallbacks(parsed_doc.fallbacks, g, None))
except gen.RenderedError as e:
    assert "fallback to multiple other types" in str(e), e

# test extract_fallbacks (multi-parent rejection)
grammar_text = """
%fallback A B;
%fallback B C;
%fallback C A;
asdf = A B C;
"""
parsed_doc = gen.parse_doc(grammar_text)
g = parsed_doc.build_grammar(grammar_text)
try:
    raise ValueError(gen.extract_fallbacks(parsed_doc.fallbacks, g, None))
except gen.RenderedError as e:
    assert "detected circular %fallback" in str(e), e

# test conflicts caused by fallback
grammar_text = """
%fallback A B;
asdf = *A B;
"""
parsed_doc = gen.parse_doc(grammar_text)
g = parsed_doc.build_grammar(grammar_text)
g.check()
fallbackmap = gen.extract_fallbacks(parsed_doc.fallbacks, g, None)
fallbacks = gen.Fallbacks(fallbackmap)
try:
    g.check(fallbacks=fallbacks)
    1/0
except gen.FirstFollow:
    pass


# figure out a compiler to use
from subprocess import Popen, PIPE, DEVNULL
import os
def detect_on_path(cmd):
    try:
        Popen(cmd, stdout=DEVNULL, stderr=DEVNULL).wait()
        return True
    except FileNotFoundError:
        return False

if detect_on_path(["cl.exe"]):
    cc = "cl.exe"
else:
    assert detect_on_path(["gcc"]), "gcc not found"
    cc = "gcc"

def run_e2e_test(grammar_text):
    with open("test.c", "w") as f:
        gen.gen(grammar_text, 'c', f)

    if cc == "cl.exe":
        p = Popen([cc, "test.c"])
        assert p.wait() == 0, p.wait()

        cmd = ("test.exe",)
        p = Popen(cmd)
        assert p.wait() == 0, p.wait()

        os.remove("test.c")
        os.remove("test.obj")
        os.remove("test.exe")

    else:
        assert cc == "gcc", cc
        asan = True
        if asan:
            cflags=("-g", "-fsanitize=address", "-fno-omit-frame-pointer")
            p = Popen([cc, *cflags, "test.c"])
            assert p.wait() == 0, p.wait()

            p = Popen(["./a.out"])
            assert p.wait() == 0, p.wait()
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
    status_e status = STATUS_OK;
    size_t i = 0;
    while(!status && i < ntokens){
        // printf("feeding %s (%s)\n", token_name(tokens[i].tok), tokens[i].str);
        status = parse_sum(
            &p, tokens[i].tok, (val_u){.str=tokens[i].str}, &out
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
    status_e status = STATUS_OK;
    size_t i = 0;
    while(!status && i < ntokens){
        // printf("feeding %s (%s)\n", token_name(tokens[i].tok), tokens[i].str);
        status = parse_sum(
            &p, tokens[i].tok, (val_u){.str=tokens[i].str}, &out
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


# test the C generator's location tracking
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

typedef struct {
    int start;
    int end;
} loc_t;

loc_t span(loc_t start, loc_t end){
    return (loc_t){start.start, end.end};
}

loc_t zero_loc(const loc_t *prev){
    int val = prev ? prev->end + 1 : 0;
    return (loc_t){val, val};
}

void check_location(char *val, loc_t loc);

}}

%type str {char*} {/*printf("freeing %s\n", $$);*/ free($$);};
%root sum;
%kwarg semloc_type loc_t;
%kwarg span_fn span;
%kwarg zero_loc_fn zero_loc;

STR:str;

sum:str =
    diff:a {check_location($a, @a); $$ = steal(&$a);}
    *( PLUS diff:b {check_location($b, @b); $$ = myappend($$, '+', steal(&$b));} )
    _EOF
;

diff:str =
    STR:a {check_location($a, @a); $$ = steal(&$a);}
    *( MINUS STR:b {check_location($b, @b); $$ = myappend($$, '-', steal(&$b));} )
;

{{{
int main(int argc, char **argv){
    ONSTACK_PARSER(p, 2, 7);

    struct {int tok; loc_t loc; char *str;} tokens[] = {
        {STR, (loc_t){1,1}, strdup("QWER")},
        {PLUS, (loc_t){2,2}},
        {STR, (loc_t){3,3}, strdup("ASDF")},
        {PLUS, (loc_t){4,4}},
        {STR, (loc_t){5,5}, strdup("ZXVC")},
        {MINUS, (loc_t){6,6}},
        {STR, (loc_t){7,7}, strdup("qwer")},
        {MINUS, (loc_t){8,8}},
        {STR, (loc_t){9,9}, strdup("asdf")},
        {MINUS, (loc_t){10,10}},
        {STR, (loc_t){10,10}, strdup("zxvc")},
        {_EOF, (loc_t){11,11}},
    };
    size_t ntokens = sizeof(tokens)/sizeof(*tokens);

    char *out;
    loc_t loc;
    status_e status = STATUS_OK;
    size_t i = 0;
    while(!status && i < ntokens){
        // printf("feeding %s (%s)\n", token_name(tokens[i].tok), tokens[i].str);
        status = parse_sum(
            &p, tokens[i].tok, (val_u){.str=tokens[i].str}, tokens[i].loc, &out, &loc
        );
        i++;
    }

    if(i != ntokens || status != STATUS_DONE){
        fprintf(stderr, "bad exit conditions %d\n", (int)status);
        return 1;
    }

    check_location(out, loc);
    char *exp = "QWER+ASDF+ZXVC-qwer-asdf-zxvc";
    int ret = !!strcmp(out, exp);
    if(ret){
        fprintf(stderr, "expected: %s\n", exp);
        fprintf(stderr, "but got : %s\n", out);
    }
    free(out);

    return ret;
}

void check_location(char *val, loc_t loc){
    char locstr[32];
    sprintf(locstr, "%d:%d", loc.start, loc.end);
    // printf("loc = %s, text = %s\n", locstr, val);

    #define CHECKLOC(_val, _loc) do {\
        if(val && strcmp(val, _val) == 0 && strcmp(locstr, _loc) != 0) { \
            fprintf(stderr, "for string %s:\n", _val); \
            fprintf(stderr, "    expected: %s\n", _loc); \
            fprintf(stderr, "    but got : %s\n", locstr); \
            exit(1); \
        } \
    } while(0)

    CHECKLOC("QWER", "1:1");
    CHECKLOC("ASDF", "3:3");
    CHECKLOC("ZXCV", "5:5");
    CHECKLOC("qwer", "7:7");
    CHECKLOC("asdf", "9:9");
    CHECKLOC("zxcv", "11:11");
    CHECKLOC("ZXVC-qwer-asdf-zxvc", "5:10");
    CHECKLOC("QWER+ASDF+ZXVC-qwer-asdf-zxvc", "1:11");

    #undef CHECKLOC
}

}}}
""")


# test the C generator's counted repeats
# also test custom error handlers
run_e2e_test(r"""
{{
// extra-forward declarations, to get around ordering issues
struct parser_t;
typedef struct parser_t parser_t;
struct sem_t;
typedef struct sem_t sem_t;

static void handle_error(
    parser_t *p,
    int token,
    sem_t sem,
    const unsigned char *expected_mask,
    const char *loc_summary
);
}}

%root short;
%root perfect;
%root runon;
%kwarg error_fn handle_error;
short = *4WORD DOT;
perfect = 5*5WORD DOT;
runon = 5*WORD DOT;

{{
#include <stdbool.h>

static void handle_error(
    parser_t *p,
    int token,
    sem_t sem,
    const unsigned char *expected_mask,
    const char *loc_summary
){
    char maskbuf[1024];
    snprint_mask(maskbuf, sizeof(maskbuf), expected_mask, "|");
    fprintf(stderr,
        "syntax error @(%s): expected one of (%s) but got %s\n",
        loc_summary,
        maskbuf,
        token_name(token)
    );
}

int main(int argc, char **argv){
    ONSTACK_PARSER(p, 1, 4);

    char *out;
    status_e status = STATUS_OK;
    size_t i = 0;

    // test the short parser
    for(size_t n = 0; n <= 5; n++){
        parser_reset(&p);
        bool finish = true;
        for(size_t i = 0; i < n; i++){
            status = parse_short(&p, WORD);
            // first four tokens always work
            if(i < 4 && status != STATUS_OK){
                fprintf(stderr, "short failed on %zu/%zu: %d\n", i+1, n, (int)status);
                return 1;
            }
            // after 4 tokens, always fails
            if(i >= 4){
                if(status != STATUS_SYNTAX_ERROR){
                    fprintf(stderr, "short allowed %zu/%zu\n", i+1, n);
                    return 1;
                }
                finish = false;
                break;
            }
        }
        if(finish){
            status = parse_short(&p, DOT);
            if(status != STATUS_DONE){
                fprintf(stderr, "short did not finish %zu\n", n);
                return 1;
            }
        }
    }

    // test the perfect parser
    for(size_t n = 0; n <= 6; n++){
        parser_reset(&p);
        bool finish = true;
        for(size_t i = 0; i < n; i++){
            status = parse_perfect(&p, WORD);
            // first five tokens always work
            if(i < 5 && status != STATUS_OK){
                fprintf(stderr, "perfect failed on %zu/%zu: %d\n", i+1, n, (int)status);
                return 1;
            }
            // after 5 tokens, always fails
            if(i >= 5){
                if(status != STATUS_SYNTAX_ERROR){
                    fprintf(stderr, "perfect allowed %zu/%zu\n", i+1, n);
                    return 1;
                }
                finish = false;
                break;
            }
        }
        if(finish){
            status = parse_perfect(&p, DOT);
            if(n == 5){
                if(status != STATUS_DONE){
                    fprintf(stderr, "perfect did not finish %zu\n", n);
                    return 1;
                }
            }else{
                if(status != STATUS_SYNTAX_ERROR){
                    fprintf(stderr, "perfect finished %zu\n", n);
                    return 1;
                }
            }
        }
    }

    // test the runon parser
    for(size_t n = 0; n <= 20; n++){
        parser_reset(&p);
        for(size_t i = 0; i < n; i++){
            status = parse_runon(&p, WORD);
            // WORD tokens are always accepted
            if(status != STATUS_OK){
                fprintf(stderr, "runon failed on %zu/%zu: %d\n", i+1, n, (int)status);
                return 1;
            }
        }
        status = parse_runon(&p, DOT);
        if(n < 5){
            if(status != STATUS_SYNTAX_ERROR){
                fprintf(stderr, "runon finished %zu\n", n);
                return 1;
            }
        }else{
            if(status != STATUS_DONE){
                fprintf(stderr, "runon did not finish %zu\n", n);
                return 1;
            }
        }
    }

    return 0;
}
}}
""")

print("PASS")
