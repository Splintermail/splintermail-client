# import gen
#
# g = gen.Grammar()
#
# EOL = g.token("EOL")
# NUM = g.token("NUM")
# PLUS = g.token("PLUS")
# MINUS = g.token("MINUS")
# MULT = g.token("MULT")
# DIV = g.token("DIV")
# LPAREN = g.token("LPAREN")
# RPAREN = g.token("RPAREN")
#
# # declare expression
# expr = g.sequence("expr")
#
# # factor = '(' exp0 ')' | num ;
# @g.branch
# def factor(branch):
#     with branch as e:
#         e.match(LPAREN)
#         e.match(expr)
#         e.match(RPAREN)
#     with branch as e:
#         n = e.match(NUM)
#
# # mult_op = * factor | - factor ;
# @g.branch
# def mult_op(branch):
#     with branch as e:
#         e.match(MULT)
#         e.match(factor)
#     with branch as e:
#         e.match(DIV)
#         e.match(factor)
#
# # term = factor mult_op* ;
# @g.seq
# def term(e):
#     e.match(factor)
#     e.zero_or_more(mult_op)
#
# # add_op = + term | - term ;
# @g.branch
# def add_op(branch):
#     with branch as e:
#         e.match(PLUS)
#         e.match(term)
#     with branch as e:
#         e.match(MINUS)
#         e.match(term)
#
# # define expression
# expr.match(term)
# expr.zero_or_more(add_op)
#
# g.gen()

##############

# mult_op:int = (
# | '*' {$$ = 1;}
# | '/' {$$ = 0;}
# );
#
# sum_op:int = (
# | '+' {$$ = +1;}
# | '-' {$$ = -1;}
# );
#
# factor:int = (
# | NUM { $$=$NUM; }
# | '(' expr ')' { $$=$expr; }
# );
#
# term:int = factor:f1 {$$ = $f1;}
#            *[ mult_op:m factor:f2 { $$ = $m ? $$ * $f2 : $$ / $f2; } ];
#
# expr:int = term:t1 { $$ = $t1}
#            *[ sum_op:s term:t2 { $$ += $s * $t2; } ];

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
            e.match(NUM)
            e.exec("$$ = $NUM;")
        with b.branch():
            e.match(LPAREN)
            e.match(expr)
            e.match(RPAREN)
            e.exec("$$ = $expr;")

# mult_op = ('*'|'/')
@g.expr
def mult_op(e):
    with e.branches() as b:
        with b.branch():
            e.match(MULT)
        with b.branch():
            e.match(DIV)

# term = factor { mult_op factor }
@g.expr
def term(e):
    e.match(factor, "f1")
    e.exec("$$ = $f1")
    with e.zero_or_more():
        e.match(mult_op, "m")
        e.match(factor, "f2")
        e.exec("$$ = $m ? $$ * $f1 : $$ / $f1;")

# sum_op = ('+'|'-')
@g.expr
def sum_op(e):
    with e.branches() as b:
        with b.branch():
            e.match(PLUS)
        with b.branch():
            e.match(MINUS)

# expr = term { sum_op term }
@expr
def expr(e):
    e.match(term)
    with e.zero_or_more():
        e.match(sum_op)
        e.match(term)
    # TODO: enforce that any top-level parser has a clear endpoint
    e.match(EOL)

gen.C().gen_file(g)
