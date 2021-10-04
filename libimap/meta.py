import textwrap

# start with a clean gen.py, since we need to import it ourselves
with open("gen.py.in") as fin:
    with open("gen.py", "w") as fout:
        fout.write(fin.read())

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
# doc = *code conf *conf *code;
# code = CODE [COLON TEXT:tag]
# conf = directive | def;
# def = name (SEMI | EQ branches SEMI);
# name = TEXT:name [COLON TEXT:tag];
# branches =
# | PIPE seq PIPE SEQ *(PIPE seq)
# | seq *(PIPE SEQ)
# ;
# seq = *code term *code *(term *code);
# term =
# | ASTERISK (TEXT | LPAREN branches RPAREN)  # disallow *thing:tag,
#                                             # since the :tag would be useless
# | name
# | LPAREN branches RPAREN
# | LBRACKET branches RBRACKET
# | LANGLE branches QUESTION *code RANGLE
# ;
# directive = PERCENT (
#   | GENERATOR TEXT:generator
#   | GENERATOR_ARG [COLON TEXT:tag] TEXT:key TEXT:value  # TODO: support ATOM
#   | DESTRUCTOR [COLON TEXT:tag] TEXT:type CODE
# );

g = gen.Grammar()

TEXT = g.token("TEXT")
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
DESTRUCTOR = g.token("DESTRUCTOR")

# Forward declaration.
branches = g.expr("branches")

@g.expr
def name(e):
    e.match(TEXT, "name")
    e.exec("$$ = ParsedName($name, None)")
    with e.maybe():
        e.match(COLON)
        e.match(TEXT, "tag")
        e.exec("$$.tag = $tag")

@g.expr
def code(e):
    e.match(CODE, "text")
    e.exec("$$ = ParsedSnippet(textwrap.dedent($text).strip('\\n'))")
    with e.maybe():
        e.match(COLON)
        e.match(TEXT, "tag")
        e.exec("$$.tag = $tag")

@g.expr
def term(e):
    with e.branches() as b:
        with b.branch():
            e.match(ASTERISK)
            with e.branches() as b2:
                with b2.branch():
                    e.match(TEXT, "name")
                    e.exec("$$ = ParsedMultiplier(ParsedName($name, None), '*')")
                with b2.branch():
                    e.match(LPAREN)
                    e.match(branches, "branches")
                    e.exec("$$ = ParsedMultiplier($branches, '*')")
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
            e.match(LBRACKET)
            e.match(branches, "branches")
            e.exec("$$ = ParsedMultiplier($branches, '?')")
            e.match(RBRACKET)
        with b.branch():
            e.match(LANGLE)
            e.match(branches, "branches")
            e.exec("$$ = ParsedRecovery($branches)")
            e.match(QUESTION)
            with e.zero_or_more():
                e.match(code, "code")
                e.exec("$$.code.append($code)")
            e.match(RANGLE)

@g.expr
def seq(e):
    e.exec("$$ = ParsedSequence()")
    with e.zero_or_more():
        e.match(code, "precode")
        e.exec("$$.elems.append($precode)")
    e.match(term, "term")
    e.exec("$$.elems.append($term)")
    with e.zero_or_more():
        e.match(code, "code")
        e.exec("$$.elems.append($code)")
    with e.zero_or_more():
        e.match(term, "term")
        e.exec("$$.elems.append($term)")
        with e.zero_or_more():
            e.match(code, "code")
            e.exec("$$.elems.append($code)")

@branches
def branches(e):
    e.exec("$$ = ParsedBranches()")
    with e.branches() as b:
        with b.branch():
            e.match(PIPE)
            e.match(seq, "seq")
            e.exec("$$.branches.append($seq)")
            e.match(PIPE)
            e.match(seq, "seq")
            e.exec("$$.branches.append($seq)")
            with e.zero_or_more():
                e.match(PIPE)
                e.match(seq, "seq")
                e.exec("$$.branches.append($seq)")
        with b.branch():
            e.match(seq, "seq")
            e.exec("$$.branches.append($seq)")
            with e.zero_or_more():
                e.match(PIPE)
                e.match(seq, "seq")
                e.exec("$$.branches.append($seq)")

@g.expr
def directive(e):
    e.match(PERCENT)
    with e.branches() as b:
        with b.branch():
            e.match(GENERATOR)
            e.match(TEXT, "name")
            e.exec("$$ = ParsedGenerator($name)")
        with b.branch():
            e.match(KWARG)
            e.exec("$$ = ParsedKwarg()")
            with e.maybe():
                e.match(COLON)
                e.match(TEXT, "tag")
                e.exec("$$.tag = $tag")
            e.match(TEXT, "key")
            e.exec("$$.key = $key")
            e.match(TEXT, "value")
            e.exec("$$.value = $value")
        with b.branch():
            e.match(DESTRUCTOR)
            e.exec("$$ = ParsedDestructor()")
            with e.maybe():
                e.match(COLON)
                e.match(TEXT, "tag")
                e.exec("$$.tag = $tag")
            e.match(TEXT, "type")
            e.exec("$$.type = $type")
            e.match(CODE, "text")
            e.exec("$$.snippet = ParsedSnippet(textwrap.dedent($text).strip('\\n'))")

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
def conf(e):
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
    with e.zero_or_more():
        # pre-code
        e.match(code, "code")
        e.exec("$$.precode.append($code)")

    e.match(conf, "conf")
    e.exec("$$.add_conf($conf)")
    with e.zero_or_more():
        e.match(conf, "conf")
        e.exec("$$.add_conf($conf)")

    with e.zero_or_more():
        # post-code
        e.match(code, "code")
        e.exec("$$.postcode.append($code)")

    e.match(EOF)

g.check()

with open("gen.py", "w") as f:
    with gen.read_template("gen.py.in", file=f):
        gen.Python(root="doc", prefix="Meta", span_fn="text_span").gen_file(g, file=f)
