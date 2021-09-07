import abc
import contextlib
import textwrap


class FirstFirst(Exception):
    pass

class FirstFollow(Exception):
    pass

class InfiniteRecursion(Exception):
    pass

def add_to_prev(prev, name, call):
    if prev is None:
        prev = []
    if name in prev:
        raise InfiniteRecursion(
            "circular " + call + "() detected in " + name + ": " + str(prev)
        )
    return list(prev) + [name]

def cacheable(fn):
    cache = None
    prop = "_cached_" + fn.__name__
    def _fn(self, *args, **kwargs):
        if not hasattr(self, prop):
            setattr(self, prop, fn(self, *args, **kwargs))
        return getattr(self, prop)
    return _fn


class Parsable(metaclass=abc.ABCMeta):
    @abc.abstractmethod
    def title(self):
        pass

    @abc.abstractmethod
    def get_first(self, prev=None):
        pass

    @abc.abstractmethod
    def get_disallowed_after(self, prev=None):
        pass

    @abc.abstractmethod
    def check(self):
        pass


class Token(Parsable):
    def __init__(self, name, typ=None):
        self.name = name
        self.type = typ

    @property
    def title(self):
        return self.name

    @cacheable
    def get_first(self, prev=None):
        return {self.name}

    @cacheable
    def get_disallowed_after(self, prev=None):
        return set()

    @cacheable
    def check(self):
        pass


class Maybe(Parsable):
    def __init__(self, name):
        self.name = name
        self.seq = Sequential(self.name + ".seq")

    @property
    def title(self):
        return "maybe(" + self.name + ")"

    def add_term(self, term, tag=None):
        self.seq.add_term(term, tag)

    def add_code(self, code):
        self.seq.add_code(code)

    @cacheable
    def get_first(self, prev=None):
        prev = add_to_prev(prev, self.name, "get_first")
        first = self.seq.get_first(prev)
        # TODO: is this necessary at all?
        assert None not in first, "invalid .maybe() on maybe-empty expression"
        return first.union({None})

    @cacheable
    def get_disallowed_after(self, prev):
        prev = add_to_prev(prev, self.name, "get_disallowed_after")
        disallowed = self.seq.get_disallowed_after(prev)
        assert None not in disallowed, "invalid .maybe() on maybe-empty expression"
        # since this might not exist, we disallow the this expression's first as well
        first = self.seq.get_first()
        assert None not in first, "invalid .maybe() on maybe-empty expression"
        # always include None, since this term may be missing
        return disallowed.union(first).union({None})

    @cacheable
    def check(self):
        self.seq.check()



class ZeroOrMore(Parsable):
    def __init__(self, name):
        self.name = name
        self.seq = Sequence(self.name + '.seq')

    def add_term(self, term, tag=None):
        self.seq.add_term(term, tag)

    def add_code(self, code):
        self.seq.add_code(code)

    @property
    def title(self):
        return "zero_or_more(" + self.name + ")"

    @cacheable
    def get_first(self, prev):
        prev = add_to_prev(prev, self.name, "get_first")
        first = self.seq.get_first(prev)
        assert None not in first, "invalid .zero_or_more() on maybe-empty expression: " + self.seq.title
        return first.union({None})

    @cacheable
    def get_disallowed_after(self, prev):
        prev = add_to_prev(prev, self.name, "get_disallowed_after")
        disallowed = self.seq.get_disallowed_after(prev)
        assert None not in disallowed, "invalid .maybe() on maybe-empty expression"
        # since this might not exist, we disallow the this expression's first as well
        first = self.seq.get_first(prev)
        assert None not in first, "invalid .maybe() on maybe-empty expression"
        # always include None, since this term may be missing
        return disallowed.union(first).union({None})

    @cacheable
    def check(self):
        self.seq.check()


class Branches(Parsable):
    def __init__(self, name):
        self.name = name
        self.branches = []
        self._branch = None

    @property
    def title(self):
        return self.name

    def add_term(self, term, tag=None):
        assert self._branch is not None, (
            "inside a branches() context, but not any branch() subcontext!"
        )
        self._branch.add_term(term, tag)

    def add_code(self, code):
        assert self._branch is not None, (
            "inside a branches() context, but not any branch() subcontext!"
        )
        self._branch.add_code(code)

    @contextlib.contextmanager
    def branch(self):
        assert self._branch is None, "can't nest calls to branch"
        self._branch = Sequence(self.name + '[' + str(len(self.branches)) + ']')
        yield
        self.branches.append(self._branch)
        self._branch = None

    @cacheable
    def get_first(self, prev=None):
        prev = add_to_prev(prev, self.name, "get_first")
        first = set()
        for branch in self.branches:
            first = first.union(branch.get_first(list(prev)))

        return frozenset(first)

    @cacheable
    def get_disallowed_after(self, prev):
        prev = add_to_prev(prev, self.name, "get_disallowed_after")
        prev = list(prev) + [self.name]
        disallowed = set()
        for branch in self.branches:
            disallowed = disallowed.union(branch.get_disallowed_after(list(prev)))

        return frozenset(disallowed)

    @cacheable
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


class Recovery(Parsable):
    def __init__(self, name, code):
        self.name = name
        self.code = code
        self.seq = Sequence(self.name + '.seq')
        self.after = None

    def add_term(self, term, tag=None):
        self.seq.add_term(term, tag)

    def add_code(self, code):
        self.seq.add_code(code)

    def set_after(self, after):
        assert self.after is None, "can't call set_after() twice!"
        self.after = after

    @property
    def title(self):
        return "recovery(" + self.name + ")"

    @cacheable
    def get_first(self, prev):
        return self.seq.get_first(prev)

    @cacheable
    def get_disallowed_after(self, prev):
        return self.seq.get_disallowed_after(prev)

    @cacheable
    def check(self):
        self.seq.check()


class Sequence(Parsable):
    """
    A sequence of terms which must be matched consecutively.
    """
    def __init__(self, name):
        self.name = name
        self.scopes = [self]
        self.precode = []
        # tuples of (Parsable, tag=None, code=[])
        self.terms = []

    def add_term(self, term, tag=None):
        self.terms.append((term, tag, []))

    def add_code(self, code):
        if not self.terms:
            self.precode.append(code)
        else:
            self.terms[-1][2].append(code)

    @property
    def title(self):
        return self.name

    def get_first_ex(self, start, prev):
        """
        Get the list of all tokens that this expression could start with.

        Ex: at runtime, when are processing a .maybe(), we need to be able to
        look ahead one token and know if that token can be the first token
        of this expression.
        """
        prev = add_to_prev(prev, self.name, "get_first")
        first = set()
        for term, _, _ in self.terms[start:]:
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

        return frozenset(first)

    @cacheable
    def get_first(self, prev=None):
        return self.get_first_ex(0, prev)

    @cacheable
    def get_disallowed_after(self, prev=None):
        """
        Get the list of all tokens which would cause ambiguities after this
        expression.

        Ex. a maybe(EOL) term could not be followed by a match(EOL) term; it
        would be ambiguous if an EOL matched to the maybe() or the match().
        """
        prev = add_to_prev(prev, self.name, "get_disallowed_after")
        disallowed = set()
        for term, _, _ in reversed(self.terms):
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

        return frozenset(disallowed)

    @cacheable
    def check(self):
        """
        At generation time, we need to ensure that for every Term in the
        expression, there is never a point where the lookahead token could
        possibly match the next token.
        """
        prev = {self.name}

        for term, _, _ in self.terms:
            # don't allow recursion
            if not isinstance(term, Expression):
                term.check()

        disallowed = set()
        for i, (term, _, _) in enumerate(self.terms):
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

            # Special case: check Recovery terms and call their set_after().
            if isinstance(term, Recovery):
                after_recovery = self.get_first_ex(i+1, None)
                if None in after_recovery:
                    raise ValueError(
                        "a Recovery must be followed by a never-empty match "
                        "within the same Sequence"
                    )
                term.set_after(after_recovery)


class Expression(Parsable):
    """
    Each Expression will generate one function in the parser.
    """
    def __init__(self, name, typ=None):
        self.name = name
        self.type = typ
        self.seq = Sequence(self.name + ".seq")
        self.scopes = [self.seq]
        self.nbranches = 0
        self.nzom = 0
        self.nrec = 0

    # API for defining the grammar.

    def __call__(self, fn):
        """Provide a definition for a fowards-declared expression"""
        assert fn.__name__ == self.name, fn.__name__ + " != " + self.name
        fn(self)
        return self

    def match(self, term, tag=None):
        self.scopes[-1].add_term(term, tag)
        return term

    @contextlib.contextmanager
    def maybe(self):
        term = Maybe()
        self.scopes.append(term)
        yield term
        self.scopes.pop()
        self.scopes[-1].add_term(term)

    @contextlib.contextmanager
    def zero_or_more(self):
        term = ZeroOrMore(self.name + '.zom' + str(self.nzom))
        self.nzom += 1
        self.scopes.append(term)
        yield term
        self.scopes.pop()
        self.scopes[-1].add_term(term)

    @contextlib.contextmanager
    def branches(self):
        term = Branches(self.name + '.br' + str(self.nbranches))
        self.nbranches += 1
        self.scopes.append(term)
        yield term
        self.scopes.pop()
        self.scopes[-1].add_term(term)

    @contextlib.contextmanager
    def recovery(self, code):
        term = Recovery(self.name + ".recovery" + str(self.nrec), code)
        self.nrec += 1
        self.scopes.append(term)
        yield term
        self.scopes.pop()
        self.scopes[-1].add_term(term)

    def exec(self, code):
        self.scopes[-1].add_code(code)

    # Non-user-facing methods.

    @property
    def title(self):
        return self.name

    def get_first(self, prev=None):
        # Refer to our Sequence.
        return self.seq.get_first(prev)

    def get_disallowed_after(self, prev=None):
        # Refer to our Sequence.
        return self.seq.get_disallowed_after(prev)

    def check(self):
        return self.seq.check()


class Grammar:
    def __init__(self):
        self.exprs = {}

    def expr(self, val):
        if isinstance(val, str):
            # Forward declaration case:
            #     @g.expr
            #     def my_expr(e):
            #         e.match(...)
            fn = None
            name = val
        else:
            # Declaration/definition case.
            #     my_expr = g.expr("my_expr")
            #
            #     ...
            #
            #     @my_expr
            #     def my_expr(e):
            #         e.match(...)
            fn = val
            name = fn.__name__

        assert name not in self.exprs
        e = Expression(name)
        self.exprs[name] = e
        if fn is not None:
            fn(e)
        return e

    def token(self, name):
        assert name not in self.exprs
        e = Token(name)
        self.exprs[name] = e
        return e


class C:
    """
    An object with all the methods for generating C code.
    """

    class Variables:
        def __init__(self):
            # a list of lists of available variables
            self.scopes = [[]]

        def new_scope(self):
            self.scopes.append([])

        def define(self, tag, typ, pos):
            for scope in self.scopes:
                assert tag not in scope, "tag " + tag + " shadows another tag"
            print("    #define $" + tag + " (p->semstack[call->stack + " + str(pos) + "].val." + typ + ")")
            self.scopes[-1].append(tag)

        def pop_scope(self):
            popped = self.scopes.pop()
            for tag in popped:
                print("    #undef $" + tag)


    def token_check(self, first):
        t = sorted(t for t in first if t is not None)
        return "token_in(*token, " + str(len(t)) + ", " + ", ".join(t) + ")"

    def mask_set(self, tokens):
        t = sorted(t for t in tokens if t is not None)
        return "mask_set(p->mask, " + str(len(t)) + ", " + ", ".join(t) + ")"

    def stackpersist(self, obj):
        """Return the persistent created during a match to obj."""
        if isinstance(obj, Token):
            return 1
        if isinstance(obj, Expression):
            return 1
        return 0

    def stackmax(self, obj):
        """Return the maximum stack size during a match to obj."""
        if isinstance(obj, Token):
            return 1
        if isinstance(obj, Expression):
            return 1
        if isinstance(obj, Maybe):
            return self.stackmax(obj.seq)
        if isinstance(obj, ZeroOrMore):
            return self.stackmax(obj.seq)
        if isinstance(obj, Branches):
            return max(self.stackmax(b) for b in obj.branches)
        if isinstance(obj, Recovery):
            return self.stackmax(obj.seq)
        if isinstance(obj, Sequence):
            persist = 0
            peak = 0
            for t, _, _ in obj.terms:
                peak = max(peak, persist + self.stackmax(t))
                persist += self.stackpersist(t)
            return peak
        raise RuntimeError("unrecognized object type: " + type(obj).__name__)

    def nstates(self, obj):
        """Count how many states we need for every entry."""
        if isinstance(obj, Token):
            return 1
        if isinstance(obj, Maybe):
            return self.nstates(obj.seq) + 1
        if isinstance(obj, ZeroOrMore):
            return self.nstates(obj.seq) + 1
        if isinstance(obj, Branches):
            return sum(self.nstates(b) for b in obj.branches) + 1
        if isinstance(obj, Recovery):
            return self.nstates(obj.seq) + 1
        if isinstance(obj, Sequence):
            return sum(self.nstates(t) for t, _, _ in obj.terms)
        if isinstance(obj, Expression):
            return 1
        raise RuntimeError("unrecognized object type: " + type(obj).__name__)

    def gen(self, obj, state, stack, var, tag=None):
        """Generate code to match an object."""
        if isinstance(obj, Token):
            print("    // match a " + obj.title + " token")
            print("    " + self.mask_set(obj.get_first()) + ";")
            print("    call->state = " + str(state) + ";")
            print("    AWAIT_TOKEN;")
            print("yy" + str(state) + ":")
            print("    if(*token != " + obj.title + ") SYNTAX_ERROR;")
            print("    p->semstack[call->stack + " + str(stack) + "] = sem;")
            print("    CONSUME_TOKEN;")
            print("    mask_clear(p->mask);")
            return state + self.nstates(obj), stack + 1

        if isinstance(obj, Maybe):
            print("    // maybe match " + obj.title)
            print("    call->state = " + str(state) + ";")
            print("    AWAIT_TOKEN;")
            print("yy" + str(state) + ":")
            print("    if(!" + self.token_check(obj.get_first()) + "){")
            print("        " + self.mask_set(obj.get_first()) + ";")
            print("        goto yy" + str(state) + "_done;")
            print("    }")
            self.gen(obj.seq, state+1, stack, var)
            print("yy" + str(state) + "_done:")
            return state + self.nstates(obj), stack

        if isinstance(obj, ZeroOrMore):
            print("    // match zero or more " + obj.title)
            print("yy" + str(state) + ":")
            print("    call->state = " + str(state) + ";")
            print("    AWAIT_TOKEN;")
            print("    if(!" + self.token_check(obj.get_first()) + "){")
            print("        " + self.mask_set(obj.get_first()) + ";")
            print("        goto yy" + str(state) + "_done;")
            print("    }")
            print("")
            self.gen(obj.seq, state + 1, stack, var)
            print("    // try matching another " + obj.title)
            print("    goto yy" + str(state) + ";")
            print("yy" + str(state) + "_done:")
            return state + self.nstates(obj), stack

        if isinstance(obj, Branches):
            print("    // match branches " + obj.title)
            print("    call->state = " + str(state) + ";")
            print("    AWAIT_TOKEN;")
            print("yy" + str(state) + ":")
            print("    switch(*token){")
            bstate = state + 1
            for i, b in enumerate(obj.branches):
                for t in b.get_first():
                    print(
                        "        case " + t + ": goto yy"
                        + str(state) + "_br_" + str(i) + ";"
                    )
                bstate += self.nstates(b)
            print("        default:")
            print("            " + self.mask_set(obj.get_first()) + ";")
            print("            SYNTAX_ERROR;")
            print("    }")
            print("")
            # branch parsing
            bstate = state + 1
            for i, b in enumerate(obj.branches):
                print("yy" + str(state) + "_br_" + str(i) + ":")
                bstate, stack = self.gen(b, bstate, stack, var)
                print("    goto yy" + str(state) + "_done;")
                print("")
            print("yy"+ str(state) + "_done:")
            return state + self.nstates(obj), stack

        if isinstance(obj, Recovery):
            assert obj.after is not None, ".set_after() was never called"
            print("    // prepare for recovery " + obj.title)
            print("    call->recover = " + str(state) + ";")
            print("")
            self.gen(obj.seq, state + 1, stack, var)
            print("")
            print("    goto yy" + str(state) + "_done;")
            print("yy"+ str(state) + ":")
            print("    // recovery: consume all tokens until a valid one")
            print("    AWAIT_TOKEN;")
            print("    if(!" + self.token_check(obj.after) + "){")
            print("        CONSUME_TOKEN;")
            print("        return 0;")
            print("    }")
            if obj.code is not None:
                print("    // USER CODE")
                print(textwrap.indent(textwrap.dedent(obj.code), "    "))
            print("yy"+ str(state) + "_done:")
            return state + self.nstates(obj), stack

        if isinstance(obj, Sequence):
            var.new_scope()
            for c in obj.precode:
                print(textwrap.indent(textwrap.dedent(c), "    "))
            for i, (t, tag, code) in enumerate(obj.terms):
                if tag is not None:
                    assert isinstance(t, (Token, Expression)), "wrong type for tag: " + type(t).__name__
                    assert t.type is not None, "obj " + t.title + " has tag " + tag + " but no type!"
                    var.define(tag, t.type, stack)
                state, stack = self.gen(t, state, stack, var, tag)
                if code:
                    print("    // USER CODE")
                for c in code:
                    print(textwrap.indent(textwrap.dedent(c), "    "))
                if i+1 != len(obj.terms):
                    print("")
            var.pop_scope()
            return state, stack

        if isinstance(obj, Expression):
            # code generated when calling this function
            print("    // match " + obj.title)
            print("    call->state = " + str(state) + ";")
            print("    CALL(" + self.parse_fn(obj) + ", " + str(stack) + ");")
            print("yy" + str(state) + ":")
            return state + self.nstates(obj), stack + 1

        raise RuntimeError("unrecognized object type: " + type(obj).__name__)

    def parse_fn(self, expr):
        """Get the generated function name for an Expression."""
        return "_parse_" + expr.title

    def declare_fn(self, expr):
        print("static int " + self.parse_fn(expr) + "(PARSE_FN_ARGS);")

    def define_fn(self, expr):
        """Generate the function definition for an Expression."""
        var = []
        print("static int " + self.parse_fn(expr) + "(PARSE_FN_ARGS){")
        # Ensure we have enough stack space to operate.
        print("    assert_sems_available(p, " + str(1 + self.stackmax(expr.seq)) + ");")
        # Jump to state.
        print("    switch(call->state){")
        print("        case " + str(0) + ": break;")
        for n in range(self.nstates(expr.seq)):
            print("        case " + str(n+1) + ": goto yy" + str(n+1) + ";")
        print("    }")
        print("")
        var = C.Variables()
        if expr.type is not None:
            var.define("$", expr.type, 0)

        # state starts at 1, since 0 is only the first time we're called
        # stack starts at 1, since we have always allocate the output
        self.gen(expr.seq, 1, 1, var)

        var.pop_scope()
        print("    // cleanup this call");
        print("    free_sems(p, call->stack+1);")
        print("    p->callslen--;");
        print("    return 0;")
        print("}")
        print("")

    def gen_file(self, g):
        names = sorted(g.exprs)

        # check all of the expressions
        for name in names:
            expr = g.exprs[name]
            expr.check()

        print(textwrap.dedent("""
            #include <stdio.h>
            #include <stdbool.h>
            #include <stdlib.h>
            #include <stdarg.h>
            #include <stdint.h>
            #include <string.h>
        """.strip("\n")))

        print("typedef enum {")
        print("    _NOT_A_TOKEN = 0,")
        for name in names:
            tok = g.exprs[name]
            if not isinstance(tok, Token):
                continue
            print("    " + tok.title + ",")
        print("} imf_token_t;")
        print("")

        print(textwrap.dedent(r"""
            struct imf_parser_t;
            typedef struct imf_parser_t imf_parser_t;
            struct call_t;
            typedef struct call_t call_t;
            struct sem_t;
            typedef struct sem_t sem_t;

            #define PARSE_FN_ARGS \
                call_t *call, imf_parser_t *p, imf_token_t *token, sem_t sem

            typedef int (*parse_fn_t)(PARSE_FN_ARGS);

            struct call_t {
                // a function to call
                parse_fn_t fn;

                // our starting position in the semstack
                int stack;

                // our position in the call
                int state;

                // the state to enter if we have to recover
                int recover;
            };

            struct sem_t {
                int type;
                // value
                union {
                    int i;
                } val;
                // location
                struct {
                    size_t start;
                    size_t end;
                } loc;
            };

            #define MASKLEN 1

            static void mask_clear(unsigned char *mask){
                memset(mask, 0, MASKLEN);
            }

            // accepts `n` ints, representing token values
            static void mask_set(unsigned char *mask, size_t n, ...){
                va_list ap;
                va_start(ap, n);

                for(size_t i = 0; i < n; i++){
                    int t = va_arg(ap, int);
                    mask[t/8] |= ((unsigned char)1) << (t%8);
                }

                va_end(ap);
            }

            static const char *token_name(int t);

            static void mask_print(unsigned char *mask, char *joiner){
                bool first = true;
                for(int i = 0; i < MASKLEN; i++){
                    for(int j = 0; j < 8; j++){
                        if(!(mask[i] & ((unsigned char)1)<<j)) continue;
                        printf(
                            "%s%s",
                            first ? (first=false,"") : joiner,
                            token_name(i*8+j)
                        );
                    }
                }
            }

            struct imf_parser_t {
                // stack of calls
                call_t *callstack;
                size_t callslen;
                size_t callsmax;
                // stack of semvals
                sem_t *semstack;
                size_t semslen;
                size_t semsmax;
                // a mask of all possible tokens
                unsigned char mask[MASKLEN];
            };

            call_t *calls_append(imf_parser_t *p){
                if(p->callslen == p->callsmax){
                    printf("callstack overflow!\n");
                    exit(1);
                }
                return &p->callstack[p->callslen++];
            }

            void assert_sems_available(imf_parser_t *p, size_t n){
                if(p->semslen + n >= p->semsmax){
                    printf("semstack overflow!\n");
                    exit (1);
                }
            }

            sem_t *sems_append(imf_parser_t *p){
                if(p->semslen == p->semsmax){
                    printf("semstack overflow!\n");
                    exit(1);
                }
                return &p->semstack[p->semslen++];
            }

            void free_sems(imf_parser_t *p, size_t first){
                for(size_t i = first; i < p->semsmax; i++){
                    // TODO: actually free stuff
                    p->semstack[i] = (sem_t){0};
                }
            }

            static const char *_fn_name(parse_fn_t fn);

            static void print_stack(imf_parser_t *p){
                for(size_t i = 0; i < p->callslen; i++){
                    call_t call = p->callstack[i];
                    printf("parsing %s (%d)\n", _fn_name(call.fn), call.state);
                }

            }

            #define CALL(_fn, _stack) do { \
                /* allocate call */ \
                call_t *subcall = calls_append(p); \
                *subcall = (call_t){ .fn = _fn,  .stack = call->stack + _stack }; \
                return 0; \
            } while(0)

            #define AWAIT_TOKEN \
                if(!*token) return 0

            #define CONSUME_TOKEN \
                *token = _NOT_A_TOKEN

            bool token_in(imf_token_t token, size_t n, ...){
                va_list ap;
                va_start(ap, n);

                bool out = false;
                for(size_t i = 0; i < n; i++){
                    if(token != va_arg(ap, int)) continue;
                    out = true;
                    break;
                }

                va_end(ap);
                return out;
            }

            #define SYNTAX_ERROR do { \
                printf("syntax error!\n"); \
                print_stack(p); \
                printf("expected one of: {"); \
                mask_print(p->mask, ","); \
                printf("} but got %s\n", token_name(*token)); \
                return 1; \
            } while(0)

            void do_parse(
                imf_parser_t *p,
                parse_fn_t entrypoint,
                imf_token_t token,
                sem_t sem,
                bool *ok
            ){
                *ok = false;

                if(p->callslen == 0){
                    // first token, initialize the call
                    call_t *call = calls_append(p);
                    *call = (call_t){
                        .fn = entrypoint, .stack = p->semslen-1
                    };
                }

                while(p->callslen){
                    size_t last = p->callslen;
                    call_t *call = &p->callstack[p->callslen-1];
                    int syntax_error = call->fn(call, p, &token, sem);
                    if(!syntax_error){
                        if(p->callslen == last){
                            // pause until the next token
                            return;
                        }
                        continue;
                    }
                    // syntax error; pop from the stack until we can recover
                    while(p->callslen){
                        call_t *call = &p->callstack[p->callslen-1];
                        if(call->recover){
                            // this call has a recovery state we can enter
                            call->state = call->recover;
                            call->recover = 0;
                            break;
                        }else{
                            free_sems(p, call->stack);
                            p->callslen--;
                        }
                    }
                }

                // parsed everything!
                *ok = true;
            }
        """.strip('\n')))

        # declare functions
        for name in names:
            expr = g.exprs[name]
            if not isinstance(expr, Expression):
                continue
            self.declare_fn(expr)
        print("")

        print("static const char *token_name(int t){")
        print("    switch(t){")
        for name in names:
            expr = g.exprs[name]
            if not isinstance(expr, Token):
                continue
            print("        case " + name + ": return \"" +name+ "\";")
        print("        default: return \"unknown\";")
        print("    }")
        print("}")
        print("")

        # get function names
        print("static const char *_fn_name(parse_fn_t fn){")
        for name in names:
            expr = g.exprs[name]
            if not isinstance(expr, Expression):
                continue
            print(
                "    if(fn == " + self.parse_fn(expr) + ")"
                + " return \""+self.parse_fn(expr)+"\";"
            )
        print("    return \"unknown\";")
        print("}")
        print("")

        # define functions
        for name in names:
            expr = g.exprs[name]
            if not isinstance(expr, Expression):
                continue
            self.define_fn(expr)

        print(textwrap.dedent(r"""
            void parse_line(imf_parser_t *p, imf_token_t token, sem_t sem, bool *ok){
                do_parse(p, _parse_line, token, sem, ok);
            }

            int main(int argc, char **argv){
                call_t calls[100];
                sem_t sems[100];
                size_t callsmax = sizeof(calls) / sizeof(*calls);
                size_t semsmax = sizeof(sems) / sizeof(*sems);
                imf_parser_t p = {
                    .callstack = calls,
                    .callsmax = callsmax,
                    .semstack = sems,
                    .semsmax = semsmax,
                };

                int tokens[] = {
                    NUM, 0,
                    PLUS, 0,
                    LPAREN, 0,
                    NUM, 0,
                    NUM, 0,
                    RPAREN, 0,
                    EOL, 0,

                    NUM, 18,
                    PLUS, 0,
                    LPAREN, 0,
                    NUM, 6,
                    RPAREN, 0,
                    EOL, 0,
                };
                size_t ntokens = sizeof(tokens) / sizeof(*tokens);

                bool ok = false;
                for(size_t i = 0; i < ntokens; i += 2){
                    // printf("feeding %s\n", token_name(tokens[i]));
                    sem_t sem = {.val.i = tokens[i+1]};
                    parse_line(&p, tokens[i], sem, &ok);

                    if(tokens[i] == EOL){
                        if(!ok){
                            printf("didn't get OK afterwards!\n");
                            return 1;
                        }
                    }else{
                        if(ok){
                            printf("got OK too early!\n");
                            return 1;
                        }
                    }
                }

                return 0;
            }
        """.strip("\n")), end="")
