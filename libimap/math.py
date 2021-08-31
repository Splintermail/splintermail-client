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

# declare expression
expr = g.sequence("expr")

# factor = '(' exp0 ')' | num ;
@g.branch
def factor(branch):
    with branch as e:
        e.match(LPAREN)
        e.match(expr)
        e.match(RPAREN)
    with branch as e:
        n = e.match(NUM)

# mult_op = * factor | - factor ;
@g.branch
def mult_op(branch):
    with branch as e:
        e.match(MULT)
        e.match(factor)
    with branch as e:
        e.match(DIV)
        e.match(factor)

# term = factor mult_op* ;
@g.seq
def term(e):
    e.match(factor)
    e.zero_or_more(mult_op)

# add_op = + term | - term ;
@g.branch
def add_op(branch):
    with branch as e:
        e.match(PLUS)
        e.match(term)
    with branch as e:
        e.match(MINUS)
        e.match(term)

# define expression
expr.match(term)
expr.zero_or_more(add_op)

g.gen()

##############

factor = g.sequence("factor")
factor.match()


sum = g.sequence("sum")
sum.match(factor)
with sum.one_of().tag('op') as switch:
    switch.case(PLUS)
    switch.case(MINUS)


with sum.switch():
    with sum.case(PLUS):
        sum.match(factor)
        sum.exec()
    with sum.case(MINUS):
        sum.match(factor)
        sum.exec()

with sum.zero_or_more():
    sum.one_of(PLUS, MINUS)
    sum.match(factor)
    sum.exec()

with sum.maybe():
    sum.one_of(PLUS, MINUS)
    sum.match(factor)
    sum.exec()

expr = g.sequence("expr")
expr.match(sum)
