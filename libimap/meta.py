import textwrap

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
        e.exec("$$.precode.append($code)")

    e.match(definition, "def")
    e.exec("$$.defs.append($def)")
    with e.zero_or_more():
        e.match(definition, "def")
        e.exec("$$.defs.append($def)")

    with e.zero_or_more():
        # post-code
        e.match(CODE, "code")
        e.exec("$$.postcode.append($code)")

    e.match(EOF)

g.check()

print("import sys")
print("import textwrap")
print("")
print("import gen")
print("")
print(textwrap.dedent(r"""
class ParsedName:
    def __init__(self, name, tag):
        self.name = name
        self.tag = tag

    def process(self, g, e):
        e.match(g.get_name(self.name), self.tag)

class ParsedBranches:
    def __init__(self):
        self.branches = []

    def process(self, g, e):
        with e.branches() as b:
            for parsed_branch in self.branches:
                with b.branch():
                    parsed_branch.process(g, e)

class ParsedRecovery:
    def __init__(self, sub):
        self.sub = sub
        self.code = []

    def process(self, g, e):
        with e.recovery(None) as r:
            self.sub.process(g, e)
        for c in self.code:
            r.add_recovery_code(c)

class ParsedGroup:
    def __init__(self, sub):
        self.sub = sub
        self.m = "1"
        self.code = []

    def process(self, g, e):
        if self.m == "1":
            self.sub.process(g, e)
        elif self.m == "?":
            with e.maybe():
                self.sub.process(g, e)
        elif self.m == "*":
            with e.zero_or_more():
                self.sub.process(g, e)
        else:
            raise RuntimeError("unrecognized multiplier: " + str(self.m))
        for c in self.code:
            e.exec(c)

class ParsedSequence:
    def __init__(self):
        self.precode = []
        self.terms = []

    def process(self, g, e):
        for c in self.precode:
            e.exec(c)
        for term in self.terms:
            term.process(g, e)

class ParsedDoc:
    def __init__(self):
        self.precode = []
        self.defs = []
        self.postcode = []

    def generate(self, generator):
        g = gen.Grammar()

        # first declare all tokens and expressions
        for name, val in self.defs:
            if val is None:
                _ = g.token(name.name, name.tag)
            else:
                e = g.expr(name.name)
                e.type = name.tag

        # then define all the expressions
        for name, val in self.defs:
            if val is None:
                continue
            e = g.get_name(name.name)
            val.process(g, e)

        g.check()

        # then generate the code
        for c in self.precode:
            print(textwrap.dedent(c))

        generator.gen_file(g)

        for c in self.postcode:
            print(textwrap.dedent(c))
"""))

gen.Python().gen_file(g, fn="parse_doc")

print("")
print(textwrap.dedent(r"""
class CharStream:
    def __init__(self):
        self._lookahead = []

    def peek(self, index=0):
        while len(self._lookahead) <= index:
            temp = yield
            self._lookahead.append(temp)
        return self._lookahead[index]

    def __next__(self):
        if self._lookahead:
            return self._lookahead.pop()
        temp = yield
        return temp

    def empty(self):
        return not self._lookahead

class Discard:
    pass

class Tokenizer:
    def __init__(self):
        self.cs = CharStream()
        self.out = []

        def _gen():
            yield from self._tokenize_many()
        self.gen = _gen()
        next(self.gen)

    def feed(self, c):
        if self.gen is None:
            raise RuntimeError("tokenizer is already done!")

        if c is None:
            try:
                # This should finish the generator.
                self.gen.send(None)
                raise RuntimeError("tokenizer failed to exit after None")
            except StopIteration:
                return self.out

        self.gen.send(c)
        out = self.out
        self.out = []
        return out

    def _tokenize_many(self):
        while True:
            token = yield from self._tokenize()
            if token is None:
                break
            if isinstance(token, Discard):
                continue
            self.out.append(token)

    def _tokenize(self):
        c = yield from self.cs.peek()

        if c is None:
            return None

        if c in " \r\n\t":
            return (yield from self._ignore_whitespace())

        if c in "#":
            return (yield from self._ignore_comment())

        singles = {
            "*": ASTERISK,
            ":": COLON,
            "=": EQ,
            "<": LANGLE,
            "[": LBRACKET,
            "(": LPAREN,
            "|": PIPE,
            ">": RANGLE,
            "]": RBRACKET,
            ")": RPAREN,
            ";": SEMI,
        }
        if c in singles:
            yield from next(self.cs)
            return Token(singles[c], c)

        if c == "{":
            return (yield from self._tokenize_code())

        if c in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_":
            return (yield from self._tokenize_text())

        raise RuntimeError("invalid character in tokenizer" + str(ord(c)))

    def _ignore_whitespace(self):
        # discard the rest of the line
        while True:
            c = yield from self.cs.peek()
            if c is None or c not in " \r\n\t":
                break
            yield from next(self.cs)
        return Discard()

    def _ignore_comment(self):
        # discard the rest of the line
        while True:
            c = yield from self.cs.peek()
            if c is None or c in "\r\n":
                break
            yield from next(self.cs)
        return Discard()

    def _tokenize_code(self):
        # Some number of leading '{' chars which we discard...
        count = 0
        while True:
            c = yield from self.cs.peek()
            if c != "{":
                break
            yield from next(self.cs)
            count += 1

        # ... and code body, which we keep...
        text = ""
        while True:
            c = yield from self.cs.peek()
            if c is None:
                raise RuntimeError("unterminated code block:\n" + text)
            if c != "}":
                text += yield from next(self.cs)
                continue

            # ... and a matching number of closing '}' chars which we discard.
            matched = 1
            while matched < count:
                c = yield from self.cs.peek(matched)
                if c != "}":
                    break
                matched += 1

            if matched != count:
                # oops, these were actually part of the code body
                for _ in range(matched):
                    text += yield from next(self.cs)
                continue

            # discard these, they're not part of the code body
            for _ in range(matched):
                yield from next(self.cs)
            return Token(CODE, text)

    def _tokenize_text(self):
        text = ""
        while True:
            c = yield from self.cs.peek()
            if c is None:
                break
            if c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789":
                break
            text += yield from next(self.cs)
        return Token(TEXT, text)

    def iter(self, chars):
        def _chars(self):
            yield from chars
            yield None

        for c in chars:
            tokens = self.feed(c)
            if tokens is None:
                break
            yield from tokens
        yield Token(EOF, "")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: " + sys.argv[0] + " FILE > OUT", file=sys.stderr)
        sys.exit(1)

    with open(sys.argv[1]) as f:
        text = f.read()

    parser = Parser()

    for t in Tokenizer().iter(text):
        # the last time this is called it should return non-None
        parsed_doc = parser.feed(t)
    assert parsed_doc, "didn't get a doc!"

    g = parsed_doc.generate(gen.Python())
"""))
