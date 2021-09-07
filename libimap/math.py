# a small grammar for basic math expressions

# mult_op =  ( '*' | "/" );            # branch syntax
# sum_op =  ( '+' | '-' );
# factor =  ( NUM | '(' expr ')' );
# term =  factor *[ mult_op factor];
# expr = term *[ sum_op term ];        # zero-or-more syntax
# line = < expr > EOL;                 # error handling syntax

### In-line code

# mult_op:i = (
# | '*' {$$ = 1;}
# | '/' {$$ = 0;}
# );
#
# sum_op:i = (
# | '+' {$$ = +1;}
# | '-' {$$ = -1;}
# );
#
# factor:i = (
# | NUM { $$=$NUM; }
# | '(' expr ')' { $$=$expr; }
# );
#
# term:i = factor:f1 {$$ = $f1;}
#          *[ mult_op:m factor:f2 { $$ = $m ? $$ * $f2 : $$ / $f2; } ];
#
# expr:i = term:t1 { $$ = $t1}
#          *[ sum_op:s term:t2 { $$ += $s * $t2; } ];
#
# line = <expr> { printf("bad expr\n"); } EOL;

import gen

g = gen.Grammar()

EOL = g.token("EOL")
NUM = g.token("NUM")
PLUS = g.token("PLUS")
MINUS = g.token("MINUS")
MULT = g.token("MULT")
DIV = g.token("DIV")
LPAREN = g.token("LPAREN")
RPAREN = g.token("RPAREN")

# Forward declaration.
expr = g.expr("expr")

# factor = ( NUM | '(' expr ')' )
@g.expr
def factor(e):
    with e.branches() as b:
        with b.branch():
            e.match(NUM, "n")
            e.exec("$$ = $n;")
        with b.branch():
            e.match(LPAREN)
            e.match(expr, "e")
            e.match(RPAREN)
            e.exec("$$ = $e;")

# mult_op = ('*'|'/')
@g.expr
def mult_op(e):
    with e.branches() as b:
        with b.branch():
            e.match(MULT)
            e.exec("$$ = 1;")
        with b.branch():
            e.match(DIV)
            e.exec("$$ = 0;")

# term = factor { mult_op factor }
@g.expr
def term(e):
    e.match(factor, "f1")
    e.exec("$$ = $f1;")
    with e.zero_or_more():
        e.match(mult_op, "m")
        e.match(factor, "f2")
        e.exec("$$ = $m ? $$ * $f2 : $$ / $f2;")

# sum_op = ('+'|'-')
@g.expr
def sum_op(e):
    with e.branches() as b:
        with b.branch():
            e.match(PLUS)
            e.exec("$$ = 1;")
        with b.branch():
            e.match(MINUS)
            e.exec("$$ = -1;")

# expr = term { sum_op term }
@expr
def expr(e):
    e.match(term, "t1")
    e.exec("$$ = $t1;")
    with e.zero_or_more():
        e.match(sum_op, "s")
        e.match(term, "t2")
        e.exec("$$ += $s * $t2;")

@g.expr
def line(e):
    with e.recovery(r'printf("bad expression\n");'):
        e.match(expr, "e")
        e.exec(r'printf("expr = %d\n", $e);')
    e.match(EOL)
    # TODO: enforce that any top-level parser has a clear endpoint

# Add semantic types (no idiomatic way yet)
NUM.type = "i"
factor.type = "i"
term.type = "i"
mult_op.type = "i"
sum_op.type = "i"
expr.type = "i"

gen.C().gen_file(g)
