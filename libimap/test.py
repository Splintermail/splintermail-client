import gen

# illegal: first term might start with X or be empty, second term starts with X
try:
    g = gen.Grammar()
    X = g.token("X")

    @g.seq
    def a(e):
        e.maybe(X)
        e.match(X)

    g.gen()
    1/0
except gen.FirstFollow as e:
    pass

# same thing, but with a term in the middle
try:
    g = gen.Grammar()
    X = g.token("X")
    Y = g.token("Y")

    @g.seq
    def a(e):
        e.maybe(X)
        e.zero_or_more(Y)
        e.match(X)

    g.gen()
    1/0
except gen.FirstFollow as e:
    pass

# same thing, but in different scopes
try:
    g = gen.Grammar()
    X = g.token("X")
    Y = g.token("Y")

    @g.seq
    def a(e):
        e.maybe(X)

    @g.seq
    def b(e):
        e.zero_or_more(Y)

    @g.seq
    def c(e):
        e.match(X)

    @g.seq
    def d(e):
        e.match(a)
        e.match(b)
        e.match(c)

    g.gen()
    1/0
except gen.FirstFollow as e:
    pass


# Unit tests
g = gen.Grammar()
W = g.token("W")
X = g.token("X")
Y = g.token("Y")
Z = g.token("Z")

@g.seq
def a(e):
    e.maybe(W)
    e.maybe(X)
    e.maybe(Y)
    e.maybe(Z)

@g.seq
def b(e):
    e.maybe(W)
    e.maybe(X)
    e.match(Y)
    e.maybe(Z)

assert a.get_first() == {"W", "X", "Y", "Z", None}
assert a.get_disallowed_after() == {"W", "X", "Y", "Z", None}

assert b.get_first() == {"W", "X", "Y"}
assert b.get_disallowed_after() == {"Z"}
