import sys
if "" not in sys.path:
    sys.path = [""] + sys.path
import gen


def sparse_enforce_policy(
    g,
    no_nested_recovery=False,
    no_params=False,
    no_extra_args=False,
    no_missing_args=False,
    no_untyped_params=False,
    no_arg_param_type_mismatch=False,
    no_arg_type_downgrade=False,
    no_arg_type_upgrade=False,
    no_fallback_first_follows=False,
):
    return g.enforce_policy(
        no_nested_recovery=no_nested_recovery,
        no_params=no_params,
        no_extra_args=no_extra_args,
        no_missing_args=no_missing_args,
        no_untyped_params=no_untyped_params,
        no_arg_param_type_mismatch=no_arg_param_type_mismatch,
        no_arg_type_downgrade=no_arg_type_downgrade,
        no_arg_type_upgrade=no_arg_type_upgrade,
        no_fallback_first_follows=no_fallback_first_follows,
    )

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

@g.expr
def c(e):
    with e.branches() as br:
        with br.branch():
            e.match(W)
            e.match(X)
        with br.branch():
            e.match(Y)
            with e.repeat(1, 2):
                e.match(Z)

g.compile()

assert a.seq.first_ex == {"W", "X", "Y", "Z", None}, a.seq.first_ex
assert a.seq.disallowed_after == {"W", "X", "Y", "Z"}, a.seq.disallowed_after

assert b.seq.first_ex == {"W", "X", "Y"}, b.seq.first_ex
assert b.seq.disallowed_after == {"Z"}, b.seq.disallowed_after

assert c.seq.first_ex == {"W", "Y"}, c.seq.first_ex
assert c.seq.disallowed_after == {"Z"}, c.seq.disallowed_after


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

# same thing, but where the repeat conflicts with itself
try:
    g = gen.Grammar()
    X = g.token("X")

    @g.expr
    def a(e):
        with e.repeat(0, None):
            e.match(X)
            with e.repeat(0, 1):
                e.match(X)

    g.check()
    1/0
except gen.FirstFollow as e:
    pass

# fixed-number repeats do not cause an issue
g = gen.Grammar()
X = g.token("X")

@g.expr
def a(e):
    with e.repeat(5, 5):
        e.match(X)
    e.match(X)

g.check()

# empty expressions: various legal forms
grammar_text = """
A; B;
a = %empty;  # explicit empty
b = B | %empty;  # in a branch, explicit
c = A (B | %empty);  # in a branch, explicit
"""
parsed_doc = gen.parse_doc(grammar_text)
g = parsed_doc.build_grammar(grammar_text)
g.check()
for key, first, maybe_empty, always_empty, disallowed_after in (
    ("a", set(), True, True, set()),
    ("b", {"B"}, True, False, {"B"}),
    ("c", {"A"}, False, False, {"B"}),
):
    got = g.exprs[key].seq.first
    assert got == first, (key, got)
    got = g.exprs[key].seq.maybe_empty
    assert got == maybe_empty, (key, got)
    got = g.exprs[key].seq.always_empty
    assert got == always_empty, (key, got)
    got = g.exprs[key].seq.disallowed_after
    assert got == disallowed_after, (key, got)

branches = g.exprs["b"].seq.terms[0]
assert len(branches.branches) == 1
assert branches.default is not None

# %return handling
grammar_text = """
A; B; C; D; X; Y; Z; ARG; COMMA;
a = [X] ([Y] %return | B) (Z %return | %empty);
b = ARG *(COMMA (ARG | %return));
c = A *(COMMA (A | X Y %return));
d = (A | B [C] %return | %empty) D;
e = A B %return;
f = (A | %return) [C];
g = (A | B %return) [C];
h = (A | B [D] %return) [C];
"""
parsed_doc = gen.parse_doc(grammar_text)
g = parsed_doc.build_grammar(grammar_text)
g.check()
for key, maybe_empty, disallowed_after in (
    ("a", True, {"X", "Y", "B", "Z"}),
    ("b", False, {"ARG", "COMMA"}),
    ("c", False, {"COMMA"}),
    ("d", False, {"C"}),
    ("e", False, set()),
    ("f", True, {"A", "C"}),
    ("g", False, {"C"}),
    ("h", False, {"D", "C"}),
):
    got = g.exprs[key].seq.disallowed_after
    assert got == disallowed_after, (key, got)

# early %returns are invalid
grammar_text = """
A; B; C; D; E; F;
a = A B C 1*(D E %return) F;
b = A B C (D E %return | E %return) F;
"""
parsed_doc = gen.parse_doc(grammar_text)
g = parsed_doc.build_grammar(grammar_text)
g.compile()
assert g.exprs["a"].seq.terms[3].always_returns
assert g.exprs["b"].seq.terms[3].always_returns
try:
    g.check()
    1/0
except gen.ReturnNotLast:
    pass


# %returns can prevent first/follow conflicts on repeats
grammar_text = """
A; COMMA;
a = *(1*A (COMMA | %return));
b = 1*A (COMMA | %return);
"""
parsed_doc = gen.parse_doc(grammar_text)
g = parsed_doc.build_grammar(grammar_text)
g.compile()
g.check()
try:
    pass
except gen.ReturnNotLast:
    pass


# testing MetaTokenizer
text = r"{{{ hi {{hello}} bye }}}"
tokens = [t.type for t in gen.Tokenizer().iter(text)]
assert tokens == [gen.CODE, gen.EOF], tokens


# test extract_fallbacks (tree expansion)
grammar_text = """
A; B; C; WORD; LETTER;
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
A; B; C; WORD; LETTER; N1; N2; N3; NUM;
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
thing1 = g.exprs["thing1"].seq.first_ex
thing2 = g.exprs["thing2"].seq.first_ex
thing3 = g.exprs["thing3"].seq.first_ex
got = fallbacks.all_fallbacks(thing1)
assert got == {"A", "B", "C"}, got
got = fallbacks.all_fallbacks(thing2)
assert got == {"N1", "N2", "N3"}, got
got = fallbacks.all_fallbacks(thing3)
assert got == {"LETTER", "A", "B", "NUM", "N1", "N2", "N3"}, got
# now imagine we are looking at the branch statement in `asdf`
exclude = g.exprs["asdf"].seq.first_ex
got = fallbacks.all_fallbacks(thing1, exclude)
assert got == {"B"}, got
got = fallbacks.all_fallbacks(thing2, exclude)
assert got == {"N1", "N3"}, got
got = fallbacks.all_fallbacks(thing3, exclude)
assert got == set(), got

### This test would need to pass if we relaxed the single-parent requirement
### for %fallback.
# # test extract_fallbacks: a token can fallback to multiple types, but if both
# # to-types are used simultaneously, it causes a conflict.
# grammar_text = """
# %fallback HEX DIGIT A B C D E F;
# %fallback ALPHA A B C D E F G H I J K L M N O P Q R S T U V W X Y Z;
# token_decl = DIGIT A B C D E F G H I J K L M N O P Q R S T U V W X Y Z;
# alphahex = ALPHA | HEX;
# """
# parsed_doc = gen.parse_doc(grammar_text)
# g = parsed_doc.build_grammar(grammar_text)
# # should succeed:
# gen.extract_fallbacks(parsed_doc.fallbacks, g, None)
# try:
#     g.compile()
#     g.check()
#     1/0
# except gen.RenderedError as e:
#     assert "fallback to multiple other types" in str(e), e

# test extract_fallbacks (multi-parent rejection)
grammar_text = """
A; B; C;
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
A; B;
%fallback A B;
asdf = *A B;
"""
parsed_doc = gen.parse_doc(grammar_text)
g = parsed_doc.build_grammar(grammar_text)
fallbackmap = gen.extract_fallbacks(parsed_doc.fallbacks, g, None)
fallbacks = gen.Fallbacks(fallbackmap)
g.fallbacks = fallbacks
g.check()
try:
    sparse_enforce_policy(g, no_fallback_first_follows=True)
    1/0
except gen.FirstFollow:
    pass


def run_failure_test(exception, grammar_text):
    try:
        with open("test.c", "w") as f:
            gen.gen(grammar_text, 'c', f)
        1/0
    except exception:
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

def run_c_e2e_test(grammar_text):
    def assert_compile_success(p):
        if p.wait() == 0:
            return
        print("COMPILE STEP FAILED:")
        print("--------------------")
        with open("test.c") as f:
            print(f.read(), end="")
        print("--------------------")
        raise AssertionError("compile step failed")

    with open("test.c", "w") as f:
        gen.gen(grammar_text, 'c', f)

    if cc == "cl.exe":
        p = Popen([cc, "test.c"])
        assert_compile_success(p)

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
            assert_compile_success(p)

            p = Popen(["./a.out"])
            assert p.wait() == 0, p.wait()
        else:
            cflags=("-g",)
            p = Popen([cc, *cflags, "test.c"])
            assert_compile_success(p)

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

def run_py_e2e_test(grammar_text):
    with open("test.py", "w") as f:
        gen.gen(grammar_text, 'py', f)

    p = Popen((sys.executable, "test.py"))
    assert p.wait() == 0, p.wait()

    os.remove("test.py")

# function-like expressions: enforce arg/param match counts
grammar_text = """
A; B; C;
expr1 = A:a B:b expr2!(a, b);
expr2(a, b, c) = A B C;
"""
parsed_doc = gen.parse_doc(grammar_text)
g = parsed_doc.build_grammar(grammar_text)
errors = sparse_enforce_policy(g, no_missing_args=True)
if not errors:
    raise ValueError("errors expected but not found")
grammar_text = """
A; B; C;
expr1 = A:a B:b expr2!(a, b);
expr2(a) = A B C;
"""
parsed_doc = gen.parse_doc(grammar_text)
g = parsed_doc.build_grammar(grammar_text)
errors = sparse_enforce_policy(g, no_extra_args=True)
if not errors:
    raise ValueError("errors expected but not found")

# test empty branches in the C generator
run_c_e2e_test(r"""
%root maybe_num;
%kwarg debug true;
%type i {int};

NUM:i;
EOL;

maybe_num:i = (NUM:n {$$=$n;} | %empty {$$=7;}) EOL;

{{{

struct token {
    token_e tok;
    int val;
    int exp;
};

int do_test(struct token *tokens, size_t ntokens, int exp){
    ONSTACK_PARSER(p, 10, 10);

    status_e status = STATUS_OK;
    size_t i = 0;
    int out;
    while(!status && i < ntokens){
        status = parse_maybe_num(
            &p, tokens[i].tok, (val_u){.i=tokens[i].val}, &out, NULL
        );
        i++;
    }

    if(i != ntokens || status != STATUS_DONE){
        fprintf(stderr, "bad exit conditions %d\n", (int)status);
        return 1;
    }

    if(out != exp){
        fprintf(stderr, "bad out: %d != %d\n", out, exp);
        return 1;
    }

    return 0;
}

int main(int argc, char **argv){

    struct token t1[] = { {NUM, 11}, {EOL, 0}, };
    size_t nt1 = sizeof(t1)/sizeof(*t1);

    struct token t2[] = { {EOL, 0}, };
    size_t nt2 = sizeof(t2)/sizeof(*t2);


    int wrong = 0;
    wrong += do_test(t1, nt1, 11);
    wrong += do_test(t2, nt2, 7);

    return wrong;
}
}}}
""")

# test empty branches in the Python generator
run_py_e2e_test(r"""
%root maybe_num;
%type i {int};

NUM:i;
EOL;

maybe_num:i = (NUM:n {$$=$n;} | %empty {$$=7;}) EOL;

{{
if __name__ == "__main__":
    tokens = [
        (Token(NUM, 11), None),
        (Token(EOL), 11),
        (Token(EOL), 7),
    ]
    p = maybe_numParser(repeat=True)
    for t, exp in tokens:
        expr = p.feed(t)
        assert (expr and expr.val) == exp, (t, exp, expr)
}}
""")

# test %return statements in the C generator
run_c_e2e_test(r"""
%root tuple;
%kwarg debug true;

ITEM; COMMA; LPAREN; RPAREN;
tuple_body = ITEM *(COMMA (ITEM | %return));
tuple = LPAREN [tuple_body] RPAREN;

{{{
int main(int argc, char **argv){
    ONSTACK_PARSER(p, 10, 10);
    status_e status = STATUS_OK;

    for(size_t n = 0; n < 5; n++){
        status = parse_tuple(&p, LPAREN, NULL);
        if(status != STATUS_OK){
            fprintf(stderr, "bad status after LPAREN: %d\n", (int)status);
            return 1;
        }

        for(size_t i = 0; i < n; i++){
            token_e token = i%2==0 ? ITEM : COMMA;
            status = parse_tuple(&p, token, NULL);
            if(status != STATUS_OK){
                fprintf(stderr,
                    "bad status after %s: %d\n",
                    i%2==0 ? "ITEM" : "COMMA",
                    (int)status
                );
                return 1;
            }
        }

        status = parse_tuple(&p, RPAREN, NULL);
        if(status != STATUS_DONE){
            fprintf(stderr, "bad status after RPAREN: %d\n", (int)status);
            return 1;
        }
    }


    // feed double commas
    status = parse_tuple(&p, LPAREN, NULL);
    if(status != STATUS_OK){
        fprintf(stderr, "bad status after LPAREN: %d\n", (int)status);
        return 1;
    }
    status = parse_tuple(&p, ITEM, NULL);
    if(status != STATUS_OK){
        fprintf(stderr, "bad status after ITEM: %d\n", (int)status);
        return 1;
    }
    status = parse_tuple(&p, COMMA, NULL);
    if(status != STATUS_OK){
        fprintf(stderr, "bad status after COMMA: %d\n", (int)status);
        return 1;
    }
    status = parse_tuple(&p, COMMA, NULL);
    if(status != STATUS_SYNTAX_ERROR){
        fprintf(stderr, "bad status after double-COMMA: %d\n", (int)status);
        return 1;
    }

    return 0;
}
}}}
""")


# test %return statements in the Python generator
run_py_e2e_test(r"""
%root tuple;

ITEM; COMMA; LPAREN; RPAREN;
tuple_body = ITEM *(COMMA (ITEM | %return));
tuple = LPAREN [tuple_body] RPAREN;

{{
if __name__ == "__main__":
    p = tupleParser(repeat=True)
    for n in range(5):
        p.feed(Token(LPAREN))
        for i in range(n):
            if i%2 == 0:
                p.feed(Token(ITEM))
            else:
                p.feed(Token(COMMA))
        p.feed(Token(RPAREN))

    # feed double commas
    p.feed(Token(LPAREN))
    p.feed(Token(ITEM))
    p.feed(Token(COMMA))
    try:
        p.feed(Token(COMMA))
        raise ValueError("expected SyntaxError")
    except SyntaxError:
        pass
}}
""")

# test %fallback matching in the C generator (valid)
run_c_e2e_test(r"""
LETTER;
A;
B;
DIGIT;
ONE;
TWO;
_EOF;
%fallback LETTER A B;
%fallback DIGIT ONE TWO;

# test %fallback in a sequence
%root tla;
tla = LETTER LETTER LETTER;

# test %fallback in a maybe repeat
%root maybe_letter;
maybe_letter = [LETTER] _EOF;

# test %fallback in a zero-or-more repeat
%root maybe_word;
maybe_word = *LETTER _EOF;

# test %fallback in a general repeat
%root three_to_five;
three_to_five = 3*5(LETTER) _EOF;

# test %fallback in a branch
%root token;
token = (LETTER | DIGIT);

# test %fallback propaging through a sequence->branch->expression->repeat
token_expr = (LETTER | DIGIT);
%root one_or_two_tokens;
one_or_two_tokens = token_expr [token_expr] _EOF;

{{{
int main(int argc, char **argv){
    ONSTACK_PARSER(p, 10, 10);

    int wrong = 0;

    #define RUN_TEST(name, ...) do { \
        token_e tokens[] = {__VA_ARGS__}; \
        size_t ntokens = sizeof(tokens)/sizeof(*tokens); \
        \
        status_e status = STATUS_OK; \
        size_t i = 0; \
        while(!status && i < ntokens){ \
            status = parse_##name(&p, tokens[i], NULL); \
            i++; \
        } \
        \
        if(i != ntokens || status != STATUS_DONE){ \
            fprintf( \
                stderr, \
                "bad exit conditions parsing " #name " %d\n", \
                (int)status \
            ); \
            wrong++; \
        } \
    } while(0)

    // tla = LETTER LETTER LETTER;
    RUN_TEST(tla, A, B, A);
    RUN_TEST(tla, B, A, B);
    RUN_TEST(tla, LETTER, A, B);
    RUN_TEST(tla, A, LETTER, B);

    // maybe_letter = [LETTER] _EOF;
    RUN_TEST(maybe_letter, A, _EOF);
    RUN_TEST(maybe_letter, B, _EOF);
    RUN_TEST(maybe_letter, LETTER, _EOF);
    RUN_TEST(maybe_letter, _EOF);

    // maybe_word = *LETTER _EOF;
    RUN_TEST(maybe_word, A, B, A, _EOF);
    RUN_TEST(maybe_word, B, A, B, _EOF);
    RUN_TEST(maybe_word, A, _EOF);
    RUN_TEST(maybe_word, B, _EOF);
    RUN_TEST(maybe_word, LETTER, LETTER, _EOF);
    RUN_TEST(maybe_word, LETTER, _EOF);
    RUN_TEST(maybe_word, _EOF);

    // three_to_five = 3*5(LETTER) _EOF;
    RUN_TEST(three_to_five, A, B, A, _EOF);
    RUN_TEST(three_to_five, B, A, B, A, B, _EOF);

    // token = (LETTER | DIGIT);
    RUN_TEST(token, A);
    RUN_TEST(token, B);
    RUN_TEST(token, LETTER);
    RUN_TEST(token, ONE);
    RUN_TEST(token, TWO);
    RUN_TEST(token, DIGIT);

    // one_or_two_tokens = token_expr [token_expr] _EOF;
    RUN_TEST(one_or_two_tokens, A, _EOF);
    RUN_TEST(one_or_two_tokens, B, _EOF);
    RUN_TEST(one_or_two_tokens, LETTER, _EOF);
    RUN_TEST(one_or_two_tokens, ONE, _EOF);
    RUN_TEST(one_or_two_tokens, TWO, _EOF);
    RUN_TEST(one_or_two_tokens, DIGIT, _EOF);
    RUN_TEST(one_or_two_tokens, A, A, _EOF);
    RUN_TEST(one_or_two_tokens, B, B, _EOF);
    RUN_TEST(one_or_two_tokens, LETTER, LETTER, _EOF);
    RUN_TEST(one_or_two_tokens, ONE, ONE, _EOF);
    RUN_TEST(one_or_two_tokens, TWO, TWO, _EOF);
    RUN_TEST(one_or_two_tokens, DIGIT, DIGIT, _EOF);

    return wrong;
}
}}}
""")

# test function-like expressions (valid)
run_c_e2e_test(r"""
{{
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

static int saved = 0;
static int passed = 0;
static int finalized = 0;
loc_t numloc = {0};
}}

%root num;
%type i {int};
%kwarg semloc_type loc_t;
%kwarg span_fn span;
%kwarg zero_loc_fn zero_loc;
%kwarg debug true;

NUM:i;
JUNK;

locate_num(x) = JUNK { numloc = @x; };
save_num(x:i) = locate_num!(x) { saved = $x++; };
pass_num(p:i) = { passed=$p; fprintf(stderr, "set passed\n"); } save_num!(p);
set_output(x:i) = %empty { $x = 9; };
num:i = NUM:n pass_num!(n) set_output!($) { finalized = $n; };

{{{
int main(int argc, char **argv){
    ONSTACK_PARSER(p, 10, 10);

    struct {int tok; int val; loc_t loc;} tokens[] = {
        {NUM, 7, {1,1}},
        {JUNK, 0, {2,2}},
    };
    size_t ntokens = sizeof(tokens)/sizeof(*tokens);

    char *out;
    int numout = 0;
    status_e status = STATUS_OK;
    size_t i = 0;
    while(!status && i < ntokens){
        status = parse_num(
            &p,
            tokens[i].tok,
            (val_u){.i=tokens[i].val},
            tokens[i].loc,
            &numout,
            NULL,
            NULL
        );
        i++;
    }

    if(i != ntokens || status != STATUS_DONE){
        fprintf(stderr, "bad exit conditions %d\n", (int)status);
        return 1;
    }

    int wrong = 0;
    if(saved != 7){
        fprintf(stderr, "saved: %d, not 7\n", saved);
        wrong++;
    }
    if(passed != 7){
        fprintf(stderr, "passed: %d, not 7\n", passed);
        wrong++;
    }
    if(finalized != 8){
        fprintf(stderr, "finalized: %d, not 8\n", finalized);
        wrong++;
    }
    if(numloc.start != 1){
        fprintf(stderr, "numloc.start: %d, not 1\n", numloc.start);
        wrong++;
    }
    if(numloc.end != 1){
        fprintf(stderr, "numloc.end: %d, not 1\n", numloc.end);
        wrong++;
    }
    if(numout != 9){
        fprintf(stderr, "numout: %d, not 9\n", numout);
        wrong++;
    }

    return wrong;
}
}}}
""")

# test the C generator; make sure everything is always freed, with a grammar
# whose side effects never free anything
run_c_e2e_test(r"""
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
%kwarg debug true;

STR:str;
MINUS;
PLUS;
_EOF;

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
            &p, tokens[i].tok, (val_u){.str=tokens[i].str}, &out, NULL
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
run_c_e2e_test(r"""
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
%kwarg debug true;

STR:str;
MINUS;
PLUS;
_EOF;

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
            &p, tokens[i].tok, (val_u){.str=tokens[i].str}, &out, NULL
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
run_c_e2e_test(r"""
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
%kwarg debug true;

STR:str;
MINUS;
PLUS;
_EOF;

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
            &p,
            tokens[i].tok,
            (val_u){.str=tokens[i].str},
            tokens[i].loc,
            &out,
            &loc,
            NULL
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
run_c_e2e_test(r"""
{{
// extra-forward declarations, to get around ordering issues
struct parser_t;
typedef struct parser_t parser_t;
struct sem_t;
typedef struct sem_t sem_t;

static void handle_error(
    parser_t *p,
    unsigned int token,
    sem_t sem,
    const unsigned char *expected_mask,
    const char *loc_summary
);
}}

WORD;
DOT;

%root short;
%root perfect;
%root runon;
%kwarg error_fn handle_error;
%kwarg debug true;
short = *4WORD DOT;
perfect = 5*5WORD DOT;
runon = 5*WORD DOT;

{{
#include <stdbool.h>

static void handle_error(
    parser_t *p,
    token_e token,
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


# try cover as much code as possible to exercise the memcheck.
# simultaneously, try even harder to trigger a memory leak.
run_c_e2e_test(r"""
{{
#include <stdlib.h>
#include <string.h>

// extra-forward declarations, to get around ordering issues
struct parser_t;
typedef struct parser_t parser_t;
struct sem_t;
typedef struct sem_t sem_t;

static void handle_error(
    parser_t *p,
    unsigned int token,
    sem_t sem,
    const unsigned char *expected_mask,
    const char *loc_summary
);
}}

%type str {char*} {fprintf(stderr, "freeing %s\n", $$); free($$);};
%kwarg error_fn handle_error;
%kwarg debug true;
%root line;

WORD:str;
X:str;
Y:str;
COMMA;
LPAREN;
RPAREN;
EOL;

start:str = %empty {$$=strdup("hi");};
arglist(s:str):str = {$$=strdup("hi");} WORD 3*(COMMA (WORD | %return));
expr(s:str):str = {$$=strdup("hi");} WORD LPAREN arglist!(s) RPAREN;
line:str = {$$=strdup("hi");} start:s (
    expr!(s) | [X] start start [Y] start
) EOL %return;

{{{
static void handle_error(
    parser_t *p,
    token_e token,
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

int do_test(parser_t *p,  token_e *tokens, size_t ntokens){
    int wrong = 0;
    for(size_t n = 1; n <= ntokens; n++){
        for(size_t i = 0; i < n; i++){
            char *strval = NULL;
            if(tokens[i]==WORD) strval = strdup("WORD");
            if(tokens[i]==X) strval = strdup("X");
            if(tokens[i]==Y) strval = strdup("Y");
            // fprintf(stderr, "feeding %s\n", token_name(tokens[i]));
            val_u val = { .str = strval };
            char *out = NULL;
            status_e status = parse_line(p, tokens[i], val, &out);
            status_e expected = i+1 == ntokens ? STATUS_DONE : STATUS_OK;
            if(status == STATUS_DONE){ free(out); }
            if(status != expected){
                fprintf(stderr,
                    "(i=%zu, n=%zu) bad status after %s: %d, expected %d\n",
                    i,
                    n,
                    token_name(tokens[i]),
                    (int)status,
                    (int)expected
                );
                wrong += 1;
                break;
            }
        }
        // fprintf(stderr, "reset!\n");
        parser_reset(p);
    }
    return wrong;
}

int main(int argc, char **argv){
    ONSTACK_PARSER(p, LINE_MAX_CALLSTACK, LINE_MAX_SEMSTACK);
    int wrong = 0;
    {
        token_e tokens[] = {
            WORD,
            LPAREN,
            WORD, COMMA,
            WORD, COMMA,
            WORD, COMMA,
            WORD, COMMA,
            WORD, COMMA,
            RPAREN,
            EOL,
        };
        size_t ntokens = sizeof(tokens) / sizeof(*tokens);
        wrong += do_test(&p, tokens, ntokens);

        // remove the last comma to hit the %return
        tokens[ntokens-3] = tokens[ntokens-2];
        tokens[ntokens-2] = tokens[ntokens-1];
        ntokens--;
        wrong += do_test(&p, tokens, ntokens);
    }

    // Pass through the default branch in different ways
    {
        token_e tokens[] = {X, Y, EOL};
        size_t ntokens = sizeof(tokens) / sizeof(*tokens);
        wrong += do_test(&p, tokens, ntokens);
    }
    {
        token_e tokens[] = {X, EOL};
        size_t ntokens = sizeof(tokens) / sizeof(*tokens);
        wrong += do_test(&p, tokens, ntokens);
    }
    {
        token_e tokens[] = {Y, EOL};
        size_t ntokens = sizeof(tokens) / sizeof(*tokens);
        wrong += do_test(&p, tokens, ntokens);
    }
    {
        token_e tokens[] = {EOL};
        size_t ntokens = sizeof(tokens) / sizeof(*tokens);
        wrong += do_test(&p, tokens, ntokens);
    }


    return wrong;
}
}}}
""")

print("PASS")
