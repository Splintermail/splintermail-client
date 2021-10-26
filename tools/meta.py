import os
import sys
import textwrap

if len(sys.argv) < 2:
    print(
        "usage: %s /path/to/gen.py.in"%sys.argv[0],
        file=sys.stderr,
    )
    sys.exit(1)

INFILE = sys.argv[1]

# start with a clean gen.py, since we need to import it ourselves
with open(INFILE) as fin:
    with open("gen.py", "w") as fout:
        fout.write(fin.read())

# some environments don't import from . by default
if "" not in sys.path:
    sys.path = [""] + sys.path
import gen

# Example grammar:
#
# EOL;
# NUM;
# PLUS;
# MINUS;
# MULT;
# DIV;
# LPAREN;
# RPAREN;
#
# mult_op = MULT | DIV;
# sum_op = PLUS | MINUS;
# factor = NUM | LPAREN expr RPAREN;
# term = factor *(mult_op factor);
# expr = term *(sum_op term);
# line = < expr:e {print("$e")} ? {print("bad line")} > EOL;

# Meta grammar is:
#
# doc = *code 1*stmt *code;
# code = CODE [COLON TEXT:tag]
# stmt = directive | def;
# def = name (SEMI | EQ branches SEMI);
# name = TEXT:name [COLON TEXT:tag];
# branches =
# | 1*(PIPE seq)
# | seq *(PIPE seq)  # allows for the convert-to-seq case
# ;
# seq = *code 1*(term *code);
# term =
# | [NUM] ASTERISK [NUM] (TEXT | LPAREN branches RPAREN)  # disallow *thing:tag
#                                                         # since the :tag
#                                                         # would be useless
# | name
# | LPAREN branches RPAREN
# | LBRACKET branches RBRACKET
# | LANGLE branches QUESTION *code RANGLE
# ;
# directive = PERCENT (
#   | GENERATOR TEXT:generator
#   | KWARG [COLON TEXT:tag] TEXT:key TEXT:value  # TODO: support ATOM
#   | TYPE [COLON TEXT:tag] CODE:spec [CODE:destructor]
#   | ROOT [COLON TEXT:tag] TEXT:spec
#   | FALLBACK [COLON TEXT:tag] TEXT:to TEXT:from *(TEXT:from);
#   | PARAM [COLON TEXT:tag] TEXT:name [CODE:type];
#   | PREFIX [COLON TEXT:tag] TEXT:prefix;
# ) SEMI;
#
# priority of operators:
#  - parens, maybes, errors (unambiguous)
#  - multipliers
#  - sequences
#  - branches

g = gen.Grammar()

TEXT = g.token("TEXT")
NUM = g.token("NUM")
COLON = g.token("COLON")
EQ = g.token("EQ")
ASTERISK = g.token("ASTERISK")
QUESTION = g.token("QUESTION")
LPAREN = g.token("LPAREN")
RPAREN = g.token("RPAREN")
LBRACKET = g.token("LBRACKET")
RBRACKET = g.token("RBRACKET")
LANGLE = g.token("LANGLE")
RANGLE = g.token("RANGLE")
PIPE = g.token("PIPE")
PERCENT = g.token("PERCENT")
CODE = g.token("CODE")
SEMI = g.token("SEMI")
EOF = g.token("EOF")

GENERATOR = g.token("GENERATOR")
KWARG = g.token("KWARG")
TYPE = g.token("TYPE")
ROOT = g.token("ROOT")
FALLBACK = g.token("FALLBACK")
PARAM = g.token("PARAM")
PREFIX = g.token("PREFIX")

# Forward declaration.
branches = g.expr("branches")

@g.expr
def name(e):
    e.match(TEXT, "name")
    e.exec("$$ = ParsedName($name, @name)")
    with e.repeat(0, 1):
        e.match(COLON)
        e.match(TEXT, "tag")
        e.exec("$$.tag = $tag")
        e.exec("$$.loc = text_span($$.loc, @tag)")

@g.expr
def code(e):
    e.match(CODE, "text")
    e.exec("$$ = ParsedSnippet(textwrap.dedent($text).strip('\\n'), @text)")
    with e.repeat(0, 1):
        e.match(COLON)
        e.match(TEXT, "tag")
        e.exec("$$.tag = $tag")
        e.exec("$$.loc = text_span($$.loc, @tag)")

@g.expr
def term(e):
    with e.branches() as b:
        with b.branch():
            e.exec("$$ = ParsedRepeat()")
            with e.repeat(0, 1):
                e.match(NUM, "n")
                e.exec("$$.rmin = $n")
                e.exec("$$.loc = @n")
            e.match(ASTERISK, "m")
            e.exec("$$.loc = $$.loc or @m")
            with e.repeat(0, 1):
                e.match(NUM, "n")
                e.exec("$$.rmax = $n")
            with e.branches() as b2:
                with b2.branch():
                    e.match(TEXT, "name")
                    e.exec("$$.term = ParsedName($name, None)")
                    e.exec("$$.loc = text_span(@m, @name)")
                with b2.branch():
                    e.match(LPAREN)
                    e.match(branches, "branches")
                    e.exec("$$.term = $branches")
                    e.exec("$$.loc = text_span(@m, @branches)")
                    e.match(RPAREN)
        with b.branch():
            e.match(name, "name")
            e.exec("$$ = $name")
        with b.branch():
            e.match(LPAREN)
            e.match(branches, "branches")
            e.exec("$$ = $branches")
            e.match(RPAREN)
        with b.branch():
            e.match(LBRACKET, "l")
            e.match(branches, "branches")
            e.exec("$$ = ParsedRepeat()")
            e.exec("$$.term = $branches")
            e.exec("$$.rmin = 0")
            e.exec("$$.rmax = 1")
            e.match(RBRACKET, "r")
            e.exec("$$.loc = text_span(@l, @r)")
        with b.branch():
            e.match(LANGLE, "l")
            e.match(branches, "branches")
            e.exec("$$ = ParsedRecovery($branches)")
            e.match(QUESTION)
            with e.repeat(0, None):
                e.match(code, "code")
                e.exec("$$.code.append($code)")
            e.match(RANGLE, "r")
            e.exec("$$.loc = text_span(@l, @r)")

@g.expr
def seq(e):
    e.exec("$$ = ParsedSequence()")
    with e.repeat(0, None):
        e.match(code, "precode")
        e.exec("$$.elems.append($precode)")
        e.exec("$$.loc = $$.loc or @precode")

    with e.repeat(1, None):
        e.match(term, "term")
        e.exec("$$.loc = $$.loc or @term")
        e.exec("$$.elems.append($term)")
        e.exec("$$.loc = text_span($$.loc, @term)")
        with e.repeat(0, None):
            e.match(code, "code")
            e.exec("$$.elems.append($code)")
            e.exec("$$.loc = text_span($$.loc, @code)")

@branches
def branches(e):
    e.exec("$$ = ParsedBranches()")
    with e.branches() as b:
        with b.branch():
            with e.repeat(1, None):
                e.match(PIPE,"p")
                e.exec("$$.loc = $$.loc or @p")
                e.match(seq, "seq")
                e.exec("$$.branches.append($seq)")
                e.exec("$$.loc = text_span($$.loc, @seq)")
        with b.branch():
            e.match(seq, "seq")
            e.exec("$$.branches.append($seq)")
            e.exec("$$.loc = @seq")
            with e.repeat(0, None):
                e.match(PIPE)
                e.match(seq, "seq")
                e.exec("$$.branches.append($seq)")
                e.exec("$$.loc = text_span($$.loc, @seq)")

@g.expr
def directive(e):
    e.match(PERCENT, "p")
    with e.branches() as b:
        with b.branch():
            e.match(GENERATOR)
            e.match(TEXT, "name")
            e.exec("$$ = ParsedGenerator($name)")
        with b.branch():
            e.match(KWARG)
            e.exec("$$ = ParsedKwarg()")
            with e.repeat(0, 1):
                e.match(COLON)
                e.match(TEXT, "tag")
                e.exec("$$.tag = $tag")
            e.match(TEXT, "key")
            e.exec("$$.key = $key")
            e.match(TEXT, "value")
            e.exec("$$.value = $value")
        with b.branch():
            e.match(TYPE)
            e.exec("$$ = ParsedType()")
            with e.repeat(0, 1):
                e.match(COLON)
                e.match(TEXT, "tag")
                e.exec("$$.tag = $tag")
            e.match(TEXT, "name")
            e.exec("$$.name = $name")
            e.match(CODE, "spec")
            e.exec("$$.spec = $spec.strip()")
            with e.repeat(0, 1):
                e.match(CODE, "destructor")
                # most other code snippets are lists with tags, so even though
                # there's only CODE block and no tag, we'll use the same format
                e.exec("""
                    $$.destructor = [
                        ParsedSnippet(
                            textwrap.dedent($destructor).strip('\\n'),
                            @destructor,
                        )
                    ]
                """)
        with b.branch():
            e.match(ROOT)
            e.exec("$$ = ParsedRoot()")
            with e.repeat(0, 1):
                e.match(COLON)
                e.match(TEXT, "tag")
                e.exec("$$.tag = $tag")
            e.match(TEXT, "name")
            e.exec("$$.name = $name")
        with b.branch():
            e.match(FALLBACK)
            e.exec("$$ = ParsedFallback()")
            with e.repeat(0, 1):
                e.match(COLON)
                e.match(TEXT, "tag")
                e.exec("$$.tag = $tag")
            e.match(TEXT, "to")
            e.exec("$$.to_type = $to")
            e.match(TEXT, "a")
            e.exec("$$.from_types.append($a)")
            with e.repeat(0, None):
                e.match(TEXT, "b")
                e.exec("$$.from_types.append($b)")
        with b.branch():
            e.match(PARAM)
            e.exec("$$ = ParsedParam()")
            with e.repeat(0, 1):
                e.match(COLON)
                e.match(TEXT, "tag")
                e.exec("$$.tag = $tag")
            e.match(TEXT, "name")
            e.exec("$$.name = $name")
            with e.repeat(0, 1):
                e.match(CODE, "type")
                e.exec("$$.type = $type")
        with b.branch():
            e.match(PREFIX)
            e.exec("$$ = ParsedPrefix()")
            with e.repeat(0, 1):
                e.match(COLON)
                e.match(TEXT, "tag")
                e.exec("$$.tag = $tag")
            e.match(TEXT, "prefix")
            e.exec("$$.prefix = $prefix")
    e.match(SEMI, 's')
    e.exec("$$.loc = text_span(@p, @s)")

@g.expr
def definition(e):
    e.match(name, "name")
    with e.branches() as b:
        with b.branch():
            # expression
            e.match(EQ)
            e.match(branches, "branches")
            e.match(SEMI)
            e.exec("""$$ = ($name, $branches)""")
        with b.branch():
            # explicit token declaration
            e.match(SEMI)
            e.exec("""$$ = ($name, None)""")

@g.expr
def stmt(e):
    with e.branches() as b:
        with b.branch():
            e.match(directive, "d")
            e.exec("$$ = $d")
        with b.branch():
            e.match(definition, "d")
            e.exec("$$ = $d")

@g.expr
def doc(e):
    e.exec("$$ = ParsedDoc()")
    with e.repeat(0, None):
        # pre-code
        e.match(code, "code")
        e.exec("$$.precode.append($code)")

    with e.repeat(1, None):
        e.match(stmt, "stmt")
        e.exec("$$.add_stmt($stmt)")

    with e.repeat(0, None):
        # post-code
        e.match(code, "code")
        e.exec("$$.postcode.append($code)")

    e.match(EOF)

g.check()


with open("gen.py", "w") as f:
    fallbackmap = {
        "TEXT": {
            "GENERATOR", "KWARG", "TYPE", "ROOT", "FALLBACK", "PARAM", "PREFIX"
        }
    }
    for token_name, _ in g.sorted_tokens():
        # no missing entries in fallbackmap
        fallbackmap.setdefault(token_name, set())
    fallbacks = gen.Fallbacks(fallbackmap)

    types = gen.Types([])

    with gen.read_template(INFILE, file=f) as dst_offset:

        ctx = gen.GeneratorContext(
            grammar=g,
            roots=["doc"],
            prefix="Meta",
            src=None,
            dst=None,
            dst_offset=dst_offset,
            file=f,
            precode=[],
            postcode=[],
            types=types,
            fallbacks=fallbacks,
            params=[],
        )

        p = gen.Python(ctx, span_fn="text_span", zero_loc_fn="text_zero_loc")
        p.gen_file()
    os.chmod("gen.py", 0o755)
