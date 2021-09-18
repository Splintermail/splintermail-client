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

print("PASS")
