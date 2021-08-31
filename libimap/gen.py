import re

# print("""
# struct imf_stack_t;
# typedef struct imf_stack_t imf_stack_t;
# struct imf_stack_t {
#     imf_stack_type_e t;
#     imf_stack_value_u v;
# };
#
# typedef call_t {
#     // a function to call
#     parse_fn_t fn;
#
#     // the point on the stack where we previously were
#     int prev;
#
#     // our position in the call
#     int state;
#
#     // a mask of all allowable tokens that could follow the expression
#     token_mask_t follow;
# };
#
# typedef union imf_stack_t {
#     call_t call;
# };
# """)

class FirstFirst(Exception):
    pass

class FirstFollow(Exception):
    pass

class Term:
    pass

class Match(Term):
    def __init__(self, expr):
        self.expr = expr

    @property
    def title(self):
        return "match(" + self.expr.title + ")"

    def gen(self, state, stack):
        print("    // match " + self.expr.title)
        print("    if(call->state == " + str(state) + "){")
        print("        call->state++;")
        print("        CALL(" + self.expr.parse_fn() + ");")
        print("    }")
        print("    if(call->state == " + str(state + 1) + "){")
        print("        // USER CODE")
        print("        call->state++;")
        print("    }")
        print("")
        return state + 2, stack

    def get_first(self, prev):
        return self.expr.get_first(prev)

    def get_disallowed_after(self, prev):
        return self.expr.get_disallowed_after(prev)

class Maybe(Term):
    def __init__(self, expr):
        self.expr = expr

    @property
    def title(self):
        return "maybe(" + self.expr.title + ")"

    def gen(self, state, stack):
        print("    // maybe match " + self.expr.title)
        print("    if(call->state == " + str(state) + "){")
        print("        AWAIT_TOKEN;")
        print("        if(" + self.expr.starts_with_fn() + "(*token){")
        print("            call->state += 1;")
        print("            return CALL(" + self.expr.parse_fn() + ")")
        print("        }else{")
        print("            call->state += 2;")
        print("        }")
        print("    }")
        print("    if(call->state == " + str(state + 1) + "){")
        print("        // USER CODE")
        print("        call->state++;")
        print("    }")
        print("")
        return state + 2, stack

    def get_first(self, prev):
        first = self.expr.get_first(prev)
        # TODO: is this necessary at all?
        assert None not in first, "invalid .maybe() on maybe-empty expression"
        return first.union({None})

    def get_disallowed_after(self, prev):
        disallowed = self.expr.get_disallowed_after(prev)
        assert None not in disallowed, "invalid .maybe() on maybe-empty expression"
        # since this might not exist, we disallow the this expression's first as well
        first = self.expr.get_first(prev)
        assert None not in first, "invalid .maybe() on maybe-empty expression"
        # always include None, since this term may be missing
        return disallowed.union(first).union({None})


class ZeroOrMore(Term):
    def __init__(self, expr, joiner=None):
        self.expr = expr
        self.joiner = joiner

    @property
    def title(self):
        return "zero_or_more(" + self.expr.title + ")"

    def gen(self, state, stack):
        print("    // match zero or more " + self.expr.title)
        print("    while(call->state < " + str(state + 2) + "){")
        print("        if(call->state == " + str(state) + "){")
        print("            AWAIT_TOKEN;")
        print("            if(" + self.expr.starts_with_fn() + "(*token){")
        print("                call->state += 1;")
        print("                return CALL(" + self.expr.parse_fn() + ")")
        print("            }else{")
        print("                call->state += 2;")
        print("            }")
        print("        }")
        print("        if(call->state == " + str(state + 1) + "){")
        print("            // USER CODE")
        print("            call->state--;")
        print("        }")
        print("    }")
        print("")
        return state + 2, stack

    def get_first(self, prev):
        first = self.expr.get_first(prev)
        assert None not in first, "invalid .zero_or_more() on maybe-empty expression: " + self.expr.title
        return first.union({None})

    def get_disallowed_after(self, prev):
        disallowed = self.expr.get_disallowed_after(prev)
        assert None not in disallowed, "invalid .maybe() on maybe-empty expression"
        # since this might not exist, we disallow the this expression's first as well
        first = self.expr.get_first(prev)
        assert None not in first, "invalid .maybe() on maybe-empty expression"
        # always include None, since this term may be missing
        return disallowed.union(first).union({None})


class Expression:
    def __init__(self, name):
        self.name = name

    @property
    def title(self):
        return self.name

    def parse_fn(self):
        return "_token_parse_" + self.name

    def starts_with_fn(self):
        return "_" + self.name + "_starts_with"

    def gen_header(self, end=";"):
        print("int " + self.parse_fn() + "(")
        print("    derr_t *e,")
        print("    imf_parser_t *p,")
        print("    int call_pos,")
        print("    imf_token_t *token")
        print(")" + end)


class Token(Expression):
    def __str__(self):
        return self.name

    def get_first(self, prev=None):
        return {self.name}

    def get_disallowed_after(self, prev=None):
        return set()

    def check(self):
        pass

    def gen(self):
        self.gen_header(end='{')
        print("    call_t *call = p->stack[call_pos].call")
        print("")
        print("    // match " + self.title)
        print("    AWAIT_TOKEN;")
        print("    if(*token != " + self.title + ") SYNTAX_ERROR;")
        print("    TAKE_TOKEN;")
        print("")
        print("    return call->prev;")
        print("}")
        print("")


class Sequence(Expression):
    def __init__(self, name):
        self.name = name
        self.terms = []
        # cacheable values
        self._first = None
        self._disallowed_after = None
        self._checked = False

    # grammar-building APIs

    def match(self, obj):
        term = Match(obj)
        self.terms.append(term)
        return term

    def zero_or_more(self, obj, *, joiner=None):
        term = ZeroOrMore(obj, joiner)
        self.terms.append(term)
        return term

    def maybe(self, obj):
        term = Maybe(obj)
        self.terms.append(term)
        return term

    # def exec(self, code):
    #     variables = re.findall("\\$[a-zA-Z_][a-zA-Z0-9_]*", code)
    #     for v in variables:
    #         name = v[1:]
    #         assert name in self.named_terms

    #     # TODO: sort by longest name first to avoid bad substitutions!
    #     code = code.replace("$$", "(stack)")
    #     for name, term in self.named_terms.items():
    #         code = code.replace("$" + name, "(" + str(term) + ")")

    #     print("        " + code)

    def gen(self):
        self.gen_header(end='{')
        print("    call_t *call = p->stack[call_pos].call")
        print("    int stack = call_pos;")
        print("")

        state = 0
        stack = 0
        for term in self.terms:
            state, stack = term.gen(state, stack)

        print("    return call->prev;")
        print("}")
        print("")

    def get_first(self, prev=None):
        """
        Get the list of all tokens that this expression could start with.

        Ex: at runtime, when are processing a .maybe(), we need to be able to
        look ahead one token and know if that token can be the first token
        of this expression.
        """
        # only calculate once
        if self._first is not None:
            return self._first

        if prev is None:
            prev = []
        assert self.name not in prev, "circular get_first() detected: " + self.name + " in " + str(prev)
        prev = list(prev) + [self.name]
        first = set()
        for term in self.terms:
            term_first = term.get_first(list(prev))
            # detect conflicts in .check(), not here.
            first = first.union(term_first)
            # if None is not in the set, we have our answer.
            if None not in first:
                break
            # if None is in the set, check the next term too.
            first.remove(None)
        else:
            # restore the None from the final term_first
            first.add(None)

        self._first = frozenset(first)
        return self._first

    def get_disallowed_after(self, prev=None):
        """
        Get the list of all tokens which would cause ambiguities after this
        expression.

        Ex. a maybe(EOL) term could not be followed by a match(EOL) term; it
        would be ambiguous if an EOL matched to the maybe() or the match().
        """
        if self._disallowed_after is not None:
            return self._disallowed_after

        if prev is None:
            prev = []
        assert self.name not in prev, "circular get_disallowed_after() detected: " + self.name + " in " + str(prev)
        prev = list(prev) + [self.name]

        disallowed = set()
        for term in reversed(self.terms):
            term_disallowed = term.get_disallowed_after(list(prev))
            disallowed = disallowed.union(term_disallowed)
            # if None is not in this term's disallowed, we have our answer
            if None not in disallowed:
                break
            # if None is in the set, check the next term too
            disallowed.remove(None)
        else:
            # restore the None from the final term_disallowed
            disallowed.add(None)

        self._disallowed_after = frozenset(disallowed)
        return self._disallowed_after

    def check(self):
        """
        At generation time, we need to ensure that for every Term in the
        expression, there is never a point where the lookahead token could
        possibly match the next token.
        """

        if self._checked:
            return
        self._checked = True

        prev = {self.name}

        disallowed = set()
        for i, term in enumerate(self.terms):
            term_first = term.get_first(list(prev))
            # Ignore None when checking for conflicts
            disallowed = disallowed.difference({None})
            conflicts = disallowed.intersection(term_first)
            if conflicts:
                raise FirstFollow(
                    "FIRST/FOLLOW conflicts:" + str(conflicts) + "\n"
                    "found while checking expression " + self.title + "\n"
                    "at least one token that starts " + term.title + "\n"
                    "is disallowed by that point."
                )
            term_disallowed = term.get_disallowed_after(list(prev))
            if None in term_disallowed:
                # disallowed should grow
                disallowed = disallowed.union(term_disallowed)
            else:
                # disallowed is reset
                disallowed = term_disallowed

class Branch(Expression):
    def __init__(self, name):
        self.name = name
        self.branches = []
        self.branch = None
        # cacheable values
        self._first = None
        self._disallowed_after = None
        self._checked = False

    def __enter__(self):
        assert self.branch is None, "can't nest"
        self.branch = Sequence(self.name + "." + str(len(self.branches)))
        return self.branch

    def __exit__(self, *arg):
        self.branches.append(self.branch)
        self.branch = None

    def gen(self):
        for branch in self.branches:
            branch.gen()

        self.gen_header(end='{')
        print(    "    call_t *call = p->stack[call_pos].call")
        print(    "    if(call->state == 0){")
        print(    "        AWAIT_TOKEN;")
        print(    "        call->state++;")
        for branch in self.branches:
            print("        if(" + branch.starts_with_fn() + "(*token)){")
            print("            CALL(" + branch.parse_fn() + ");")
            print("        }")
        print(    "        // token did not match")
        print(    "        call->state++;")
        print(    "    }")
        print(    "    if(call->state == 1){")
        print(    "        // USER CODE")
        print(    "    }")
        print(    "")
        print(    "    return call->prev;")
        print(    "}")
        print(    "")

    def get_first(self, prev=None):
        # only calculate once
        if self._first is not None:
            return self._first

        if prev is None:
            prev = []
        assert self.name not in prev, "circular get_first() detected: " + self.name + " in " + str(prev)

        prev = list(prev) + [self.name]
        first = set()
        for branch in self.branches:
            first = first.union(branch.get_first(list(prev)))

        self._first = frozenset(first)
        return self._first

    def get_disallowed_after(self, prev):
        # only calculate once
        if self._disallowed_after is not None:
            return self._disallowed_after

        if prev is None:
            prev = []
        assert self.name not in prev, "circular get_disallowed_after() detected: " + self.name + " in " + str(prev)

        prev = list(prev) + [self.name]
        disallowed = set()
        for branch in self.branches:
            disallowed = disallowed.union(branch.get_disallowed_after(list(prev)))

        self._disallowed_after = frozenset(disallowed)
        return self._disallowed_after

    def check(self):
        first = set()
        for branch in self.branches:
            branch.check()

            # no possibly-empty branches
            branch_first = branch.get_first()
            assert None not in branch_first

            # Ensure that no branches begin with the same tokens
            conflicts = first.intersection(branch_first)
            assert not conflicts, "FIRST/FIRST conflicts for tokens " + str(conflicts)

            first = first.union(branch_first)

class Grammar:
    def __init__(self):
        self.exprs = {}

    def sequence(self, name):
        assert name not in self.exprs
        e = Sequence(name)
        self.exprs[name] = e
        return e

    def seq(self, fn):
        name = fn.__name__
        assert name not in self.exprs
        e = Sequence(fn.__name__)
        self.exprs[fn.__name__] = e
        fn(e)
        return e

    def branch(self, fn):
        name = fn.__name__
        assert name not in self.exprs
        e = Branch(fn.__name__)
        self.exprs[fn.__name__] = e
        fn(e)
        return e

    def token(self, name):
        assert name not in self.exprs
        e = Token(name)
        self.exprs[name] = e
        return e

    def gen(self):
        names = sorted(self.exprs)

        # check all of the expressions
        for name in names:
            expr = self.exprs[name]
            expr.check()

        # generate all of the expressions
        for name in names:
            expr = self.exprs[name]
            expr.gen()
