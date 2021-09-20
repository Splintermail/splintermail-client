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
# mult_op =  ( MULT | DIV );
# sum_op =  ( PLUS | MINUS );
# factor =  ( NUM | LPAREN expr RPAREN );
# term =  factor [mult_op factor]*;
# expr = term [sum_op term]*;
# line = < expr > EOL;

g = gen.Grammar()

TEXT = g.token("TEXT")
COLON = g.token("COLON")
EQ = g.token("EQ")
ASTERISK = g.token("ASTERISK")
LPAREN = g.token("LPAREN")
RPAREN = g.token("RPAREN")
LBRACKET = g.token("LBRACKET")
RBRACKET = g.token("RBRACKET")
LANGLE = g.token("LANGLE")
RANGLE = g.token("RANGLE")
PIPE = g.token("PIPE")
CODE = g.token("CODE")
SEMI = g.token("SEMI")
EOF = g.token("EOF")

# Forward declaration.
seq = g.expr("seq")

@g.expr
def name(e):
    e.match(TEXT, "name")
    e.exec("$$ = ParsedName($name, None)")
    with e.maybe():
        e.match(COLON)
        e.match(TEXT, "extra")
        e.exec("$$.tag = $extra")

@g.expr
def branches(e):
    e.exec("$$ = ParsedBranches()")
    e.match(LPAREN)
    with e.maybe():
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
    e.match(RPAREN)

@g.expr
def group(e):
    # SYMBOL [*] [CODE]
    # '[' seq ']' [*] [CODE]
    # '(' seq '|' seq ')' [*] [CODE]
    with e.branches() as b:
        with b.branch():
            e.match(name, "name")
            e.exec("$$ = ParsedGroup($name)")
        with b.branch():
            e.match(LBRACKET)
            e.match(seq, "seq")
            e.exec("$$ = ParsedGroup($seq)")
            e.match(RBRACKET)
        with b.branch():
            e.match(branches, "branches")
            e.exec("$$ = ParsedGroup($branches)")
    with e.maybe():
        e.match(ASTERISK)
        e.exec("$$.m = '*'")
    with e.zero_or_more():
        e.match(CODE, "code")
        e.exec("$$.code.append($code)")

@g.expr
def recovery(e):
    e.match(LANGLE)
    e.match(seq, "seq")
    e.exec("$$ = ParsedRecovery($seq)")
    e.match(RANGLE)
    with e.zero_or_more():
        e.match(CODE, "code")
        e.exec("$$.code.append($code)")

@g.expr
def term(e):
    with e.branches() as b:
        with b.branch():
            e.match(group, "group")
            e.exec("$$ = $group")
        with b.branch():
            e.match(recovery, "recovery")
            e.exec("$$ = $recovery")

@seq
def seq(e):
    e.exec("$$ = ParsedSequence()")
    with e.zero_or_more():
        e.match(CODE, "code")
        e.exec("$$.precode.append($code)")
    e.match(term, "term")
    e.exec("$$.terms.append($term)")
    with e.zero_or_more():
        e.match(term, "term")
        e.exec("$$.terms.append($term)")

@g.expr
def definition(e):
    e.match(name, "name")
    with e.branches() as b:
        with b.branch():
            # expression
            e.match(EQ)
            e.match(seq, "seq")
            e.match(SEMI)
            e.exec("""$$ = ($name, $seq)""")
        with b.branch():
            # explicit token declaration
            e.match(SEMI)
            e.exec("""$$ = ($name, None)""")

@g.expr
def doc(e):
    e.exec("$$ = ParsedDoc()")
    with e.zero_or_more():
        # pre-code
        e.match(CODE, "code")
        e.exec("$$.precode.append(Snippet($code))")

    e.match(definition, "def")
    e.exec("$$.defs.append($def)")
    with e.zero_or_more():
        e.match(definition, "def")
        e.exec("$$.defs.append($def)")

    with e.zero_or_more():
        # post-code
        e.match(CODE, "code")
        e.exec("$$.postcode.append(Snippet($code))")

    e.match(EOF)

g.check()

with open("gen.py", "w") as f:
    with gen.read_template("gen.py.in", file=f):
        gen.Python(prefix="Meta").gen_file(g, "doc", file=f)
