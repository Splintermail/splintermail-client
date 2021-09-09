import gen

# Example grammar:
#
# EOL;
# NUM:i;
# PLUS;
# MINUS;
# MULT;
# DIV;
# LPAREN;
# RPAREN;
#
# mult_op =  ( '*' | "/" );
# sum_op =  ( '+' | '-' );
# factor =  ( NUM | '(' expr ')' );
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
expr = g.expr("expr")

@g.expr
def name(e):
    e.match(TEXT, "name")
    e.exec("$$ = [$name, None]")
    with e.maybe():
        e.match(COLON)
        e.match(TEXT, "extra")
        e.exec("$$[1] = $extra")

@g.expr
def branches(e):
    e.match(LPAREN)
    e.match(expr)
    e.match(PIPE)
    e.match(expr)
    with e.zero_or_more():
        e.match(PIPE)
        e.match(expr)
    e.match(RPAREN)

@g.expr
def group(e):
    # SYMBOL
    # SYMBOL*
    # [expr]
    # [expr]*
    # (expr | expr)
    # (expr | expr)*
    with e.branches() as b:
        with b.branch():
            e.match(name)
        with b.branch():
            e.match(LBRACKET)
            e.match(expr)
            e.match(RBRACKET)
        with b.branch():
            e.match(branches)
    with e.maybe():
        e.match(ASTERISK)
    with e.zero_or_more():
        e.match(CODE)

@g.expr
def recovery(e):
    e.match(LANGLE)
    e.match(expr)
    e.match(RANGLE)
    with e.zero_or_more():
        e.match(CODE)

@g.expr
def term(e):
    with e.branches() as b:
        with b.branch():
            e.match(group)
        with b.branch():
            e.match(recovery)

@expr
def expr(e):
    with e.zero_or_more():
        e.match(CODE)
    with e.zero_or_more():
        e.match(term)

@g.expr
def definition(e):
    e.match(name, "name")
    with e.branches() as b:
        with b.branch():
            # expression
            e.match(EQ)
            e.match(expr)
            e.match(SEMI)
        with b.branch():
            # token
            e.match(SEMI)

@g.expr
def doc(e):
    with e.zero_or_more():
        # pre-code
        e.match(CODE)

    e.match(definition)
    with e.zero_or_more():
        e.match(definition)

    with e.zero_or_more():
        # post-code
        e.match(CODE)

    e.match(EOF)

g.check()
gen.Python().gen_file(g, ("g",))
