import abc
import contextlib
import re
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


class Token(Parsable):
    def __init__(self, name):
        self.name = name

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

    def add_term(self, term):
        self.seq.add_term(term)

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


class ZeroOrMore(Parsable):
    def __init__(self, name):
        self.name = name
        self.seq = Sequence(self.name + '.seq')

    def add_term(self, term):
        self.seq.add_term(term)

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


class Sequence(Parsable):
    """
    A sequence of terms which must be matched consecutively.
    """
    def __init__(self, name):
        self.name = name
        self.scopes = [self]
        self.terms = []

    def add_term(self, term):
        self.terms.append(term)

    @property
    def title(self):
        return self.name

    @cacheable
    def get_first(self, prev=None):
        """
        Get the list of all tokens that this expression could start with.

        Ex: at runtime, when are processing a .maybe(), we need to be able to
        look ahead one token and know if that token can be the first token
        of this expression.
        """
        prev = add_to_prev(prev, self.name, "get_first")
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

        return frozenset(first)

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

        return frozenset(disallowed)

    @cacheable
    def check(self):
        """
        At generation time, we need to ensure that for every Term in the
        expression, there is never a point where the lookahead token could
        possibly match the next token.
        """
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

class Branches(Parsable):
    def __init__(self, name):
        self.name = name
        self.branches = []
        self._branch = None

    @property
    def title(self):
        return self.name

    def add_term(self, term):
        assert self._branch is not None, (
            "inside a branches() context, but not any branch() subcontext!"
        )
        self._branch.add_term(term)

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

    # API for defining the grammar.

    def __call__(self, fn):
        """Provide a definition for a fowards-declared expression"""
        assert fn.__name__ == self.name, fn.__name__ + " != " + self.name
        fn(self)
        return self

    def match(self, term, tag=None):
        self.scopes[-1].add_term(term)
        return term

    @contextlib.contextmanager
    def zero_or_more(self, tag=None):
        term = ZeroOrMore(self.name + '.zom' + str(self.nzom))
        self.nzom += 1
        self.scopes.append(term)
        yield term
        self.scopes.pop()
        self.scopes[-1].add_term(term)

    @contextlib.contextmanager
    def maybe(self, tag=None):
        term = Maybe()
        self.scopes.append(term)
        yield term
        self.scopes.pop()
        self.scopes[-1].add_term(term)

    @contextlib.contextmanager
    def branches(self, tag=None):
        term = Branches(self.name + '.br' + str(self.nbranches))
        self.nbranches += 1
        self.scopes.append(term)
        yield term
        self.scopes.pop()
        self.scopes[-1].add_term(term)

    def exec(self, code):
        pass
        # variables = re.findall("\\$[a-zA-Z_][a-zA-Z0-9_]*", code)
        # for v in variables:
        #     name = v[1:]
        #     assert name in self.named_terms

        # # TODO: sort by longest name first to avoid bad substitutions!
        # code = code.replace("$$", "(stack)")
        # for name, term in self.named_terms.items():
        #     code = code.replace("$" + name, "(" + str(term) + ")")

        # print("        " + code)

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

def gen_token_check(first):
    return "token_in(*token, 0, " + ",".join(t for t in first if t is not None) + ")"

class C:
    """
   An object with all the methods for generating C code.
    """
    def __init__(self):
        pass

    def nstack(self, obj):
        """Count how much stack space we need for every entry."""
        if isinstance(obj, Token):
            return 1
        if isinstance(obj, Expression):
            return 1
        # other Parsables don't get to store their state on the stack
        return 0

    def nstates(self, obj):
        """Count how many states we need for every entry."""
        if isinstance(obj, Token):
            return 1
        if isinstance(obj, Maybe):
            return self.nstates(obj.seq) + 1
        if isinstance(obj, ZeroOrMore):
            return self.nstates(obj.seq) + 2
        if isinstance(obj, Sequence):
            return sum(self.nstates(t) for t in obj.terms)
        if isinstance(obj, Branches):
            return sum(self.nstates(b) for b in obj.branches) + 2
        if isinstance(obj, Expression):
            return 2
        raise RuntimeError("unrecognized object type: " + type(obj).__name__)

    def gen(self, obj, state, stack, tag=None):
        """Generate code to match an object."""
        if isinstance(obj, Token):
            print("    // match a " + obj.title + " token")
            print("yy" + str(state) + ":")
            print("    printf(\"TOKEN " + obj.title + "\\n\");")
            print("    call->state = " + str(state) + ";")
            print("    AWAIT_TOKEN;")
            print("    if(*token != " + obj.title + ") SYNTAX_ERROR;")
            print("    TAKE_TOKEN;")
            print("")
            return state + self.nstates(obj), stack

        if isinstance(obj, Maybe):
            print("    // maybe match " + obj.title)
            print("yy" + str(state) + ":")
            print("    printf(\"MAYBE\\n\");")
            print("    call->state = " + str(state) + ";")
            print("    AWAIT_TOKEN;")
            print("    if(!" + gen_token_check(obj.get_first()) + "){")
            print("        goto yy" + str(state+self.nstates(obj)) + ";")
            print("    }")
            print("")
            self.gen(obj.seq, state+1)
            return state + self.nstates(obj), stack

        if isinstance(obj, ZeroOrMore):
            print("    // match zero or more " + obj.title)
            print("yy" + str(state) + ":")
            print("    printf(\"ZERO_OR_MORE " + obj.title + "\\n\");")
            print("    call->state = " + str(state) + ";")
            print("    AWAIT_TOKEN;")
            print("    if(!" + gen_token_check(obj.get_first()) + "){")
            print("        goto yy" + str(state+self.nstates(obj)-1) + ";")
            print("    }")
            print("")
            self.gen(obj.seq, state + 1)
            print("    // try matching another " + obj.title)
            print("    goto yy" + str(state) + ";")
            print("yy" + str(state+self.nstates(obj)-1) + ":")
            print("")
            return state + self.nstates(obj), stack

        if isinstance(obj, Sequence):
            print("    printf(\"SEQUENCE " + obj.title + "\\n\");")
            for t in obj.terms:
                state = self.gen(t, state)
            return state

        if isinstance(obj, Branches):
            print("    // match branches " + obj.title)
            print("yy" + str(state) + ":")
            print("    printf(\"BRANCHES " + obj.title + "\\n\");")
            print("    call->state = " + str(state) + ";")
            print("    AWAIT_TOKEN;")
            print("    switch(*token){")
            bstate = state + 1
            for b in obj.branches:
                for t in b.get_first():
                    print("        case " + t + ": goto yy" + str(bstate) + ";")
                bstate += self.nstates(b)
            print("        default: SYNTAX_ERROR;")
            print("    }")
            # branch parsing
            bstate = state + 1
            for b in obj.branches:
                bstate = self.gen(b, bstate)
                print("    goto yy" + str(state+self.nstates(obj)-1) + ";")
            print("yy"+ str(state+self.nstates(obj)-1) + ":")

            return state + self.nstates(obj), stack

        if isinstance(obj, Expression):
            # code generated when calling this function
            print("    printf(\"EXPRESSION " + obj.title + "\\n\");")
            print("    // match " + obj.title)
            print("yy" + str(state) + ":")
            print("    call->state = " + str(state + 1) + ";")
            print("    CALL(" + self.parse_fn(obj) + ");")
            print("yy" + str(state + 1) + ":")
            print("    // USER CODE")
            print("")
            return state + self.nstates(obj), stack

        raise RuntimeError("unrecognized object type: " + type(obj).__name__)

    def parse_fn(self, expr):
        """Get the generated function name for an Expression."""
        return "_parse_" + expr.title

    def declare_fn(self, expr):
        print("static size_t " + self.parse_fn(expr) + "(PARSE_FN_ARGS);")

    def define_fn(self, expr):
        """Generate the function definition for an Expression."""
        print("static size_t " + self.parse_fn(expr) + "(PARSE_FN_ARGS){")
        print("    call_t *call = &p->stack[call_pos].call;")
        # print("    size_t stack = call_pos;")
        print("")
        # Jump to state.
        print("    switch(call->state){")
        for n in range(self.nstates(expr.seq)):
            print("        case " + str(n) + ": goto yy" + str(n) + ";")
        print("    }")
        print("")
        if expr.type is not None:
            print("    size_t stack = call_pos+1");
            print("    #define $$ (p->stack[call_pos-1]." + expr.type + ")")

        self.gen(expr.seq, 0, 0)

        if expr.type is not None:
            print("    #undef $$")
        print("    return call->prev;")
        print("}")
        print("")

    def gen_file(self, g):
        names = sorted(g.exprs)

        print(textwrap.dedent("""
            #include <stdio.h>
            #include <stdbool.h>
            #include <stdlib.h>
            #include <stdarg.h>
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

            typedef size_t (*parse_fn_t)(imf_parser_t *, size_t, imf_token_t*);

            typedef struct {
                // a function to call
                parse_fn_t fn;

                // the point on the stack where we previously were
                int prev;

                // our position in the call
                int state;
            } call_t;

            typedef union {
                call_t call;
            } imf_stack_t;

            struct imf_parser_t {
                imf_stack_t *stack;
                size_t stacklen;
                size_t stackmax;
                size_t call;
            };

            imf_stack_t *stack_append(imf_parser_t *p){
                if(p->stacklen == p->stackmax){
                    return NULL;
                }
                return &p->stack[p->stacklen++];
            }

            #define CALL(_fn) do { \
                imf_stack_t *s = stack_append(p); \
                if(!s) exit(1); \
                s->call = (call_t){ .fn = _fn, .prev = call_pos }; \
                return call_pos + 1; \
            } while(0)

            #define AWAIT_TOKEN do { \
                if(!*token) return call_pos; \
            } while(0)

            #define TAKE_TOKEN do { \
                *token = _NOT_A_TOKEN; \
            } while(0)

            bool token_in(imf_token_t token, size_t n, ...){
                va_list ap;
                va_start(ap, n);

                for(size_t i = 0; i < n; i++){
                    if(token == va_arg(ap, int)){
                        va_end(ap);
                        return true;
                    }
                }

                va_end(ap);
                return false;
            }

            #define SYNTAX_ERROR exit(4)

            void do_parse(
                imf_parser_t *p,
                parse_fn_t entrypoint,
                imf_token_t token,
                bool *ok
            ){
                *ok = false;

                if(p->call == 0){
                    // first token
                    imf_stack_t *s = stack_append(p);
                    s = stack_append(p);
                    if(!s) exit(1);
                    s->call = (call_t){ .fn = entrypoint, .prev = 0 };
                    p->call = 1;
                }

                while(p->call){
                    printf("calling!\n");
                    size_t last = p->call;
                    call_t *call = &p->stack[p->call].call;
                    p->call = call->fn(p, p->call, &token);
                    if(p->call == (size_t)-1){
                        // error!
                        p->call = 0;
                        exit(2);
                    }else if(p->call == last){
                        // pause until the next token
                        return;
                    }
                }

                // parsed everything!
                *ok = true;
            }

            #define PARSE_FN_ARGS \
                imf_parser_t *p, size_t call_pos, imf_token_t *token
        """.strip('\n')))

        # check all of the expressions
        for name in names:
            expr = g.exprs[name]
            expr.check()

        # declare functions
        for name in names:
            expr = g.exprs[name]
            if not isinstance(expr, Expression):
                continue
            self.declare_fn(expr)
        print("")

        # define functions
        for name in names:
            expr = g.exprs[name]
            if not isinstance(expr, Expression):
                continue
            self.define_fn(expr)

        print(textwrap.dedent(r"""
            void parse_expr(imf_parser_t *p, imf_token_t token, bool *ok){
                do_parse(p, _parse_expr, token, ok);
            }

            int main(int argc, char **argv){
                imf_stack_t stack[100];
                size_t max = sizeof(stack) / sizeof(*stack);
                imf_parser_t p = { .stack = stack, .stackmax = 100 };

                int tokens[] = {
                    NUM,
                    PLUS,
                    NUM,
                    EOL,
                };
                size_t ntokens = sizeof(tokens) / sizeof(*tokens);

                bool ok = false;
                for(size_t i = 0; i < ntokens; i++){
                    if(ok){
                        printf("got OK too early!\n");
                        return 1;
                    }
                    parse_expr(&p, tokens[i], &ok);
                }

                if(!ok){
                    printf("didn't get OK afterwards!\n");
                    return 1;
                }

                return 0;
            }
        """.strip("\n")), end="")
