# main = msg
#
# subhdrval = maybe(UNSTRUCT) + EOL
# hdrval = zero_or_more(hdr, joiner=WS)
# hdr = HDRNAME + ':' + hdrval + EOL
#
# body = maybe(BODY);
#
# msg = zero_or_more(hdr) + EOL + body + DONE
#
# # msg: hdrs* EOL body DONE;
# # hdr: HDRNAME ':' hdrval;
# # hdrval: subhdrval*(WS);
# # subhdrval: UNSTRUCT? EOL;
# # body: BODY?;
#
#
# msg = Expression().param(t)
#
# def msg():
#     return zero_or_more(hdr()),
#         .then()
#         .then()
#
#
#
# ##############
#
# cmd: TAG[t] SP command(t);
# command(tag): CAPA capa_cmd(tag)
#             | NOOP noop_cmd(tag)
#             | LOGOUT logout_cmd(tag)
#             | AUTHENTICATE authenticate_cmd(tag)
#
# @expr
# def cmd(t):
#     return [
#         CAPA + capa_cmd(t),
#         NOOP + noop_cmd(t),
#         LOGOUT + logout_cmd(t)
#     ]
#
# def cmd(t):
#     return [
#         "CAPA" + capa_cmd(t)
#     ]
#
# with Param() as t:
#     cmd = [
#         "CAPA" + capa_cmd(t),
#         "NOOP" + noop_cmd(t),
#     ]

########

class Expression:
    def __init__(self, name=None):
        self.name = None
        self.terms = []

        self.multiplier = ""

    def show_as_term(self):
        if self.name is not None:
            return self.name
        return str(self)

    def __str__(self):
        return '(' + " ".join([t.show_as_term() for t in self.terms]) + ')' + self.multiplier

    def __add__(self, other):
        if self.multiplier:
            return Expression() + self + other
        self.terms.append(other)
        return self

    def maybe(self):
        self.multiplier = '?'
        return self

    def zero_or_more(self, joiner=None, trail=False):
        self.multiplier = '*' if not joiner else f'*({joiner})'
        return self

    def __eq__(self, other):
        return (
            type(self) == type(other)
            and self.terms == other.terms
            and self.multiplier == other.multiplier
        )

    def __hash__(self):
        return hash((*self.terms, self.multiplier))

    def __iter__(self):
        yield self
        for term in self.terms:
            yield from iter(term)

    def __repr__(self):
        return str(self)

class Token(Expression):
    def __init__(self, name, *, var=None):
        self.name = name
        self.var = var

    def show_as_term(self):
        return str(self)

    def __str__(self):
        if len(self.name) == 1:
            return f"'{self.name}'"
        return self.name

    def __add__(self, other):
        return Expression() + self + other

    def __call__(self, var):
        return Token(self.name, var=var)

    def maybe(self):
        e = (Expression() + self).maybe()
        e.name = f"maybe_{self.name.lower()}"
        return e

    def zero_or_more(self, joiner=None, trail=False):
        return (Expression() + self).zero_or_more(joiner, trail)

    def __eq__(self, other):
        return type(self) == type(other) and self.name == other.name

    def __hash__(self):
        return hash(self.name)

    def __iter__(self):
        yield self

    def __repr__(self):
        return str(self)

########

BODY = Token("BODY")
COLON = Token(":")
EOL = Token("EOL")
HDRNAME = Token("HDRNAME")
UNSTRUCT = Token("UNSTRUCT")
WS = Token("WS")

DONE = Token("DONE")

# msg: hdrs* EOL body DONE;
# hdr: HDRNAME ':' hdrval;
# hdrval: subhdrval*(WS);
# subhdrval: UNSTRUCT? EOL;
# body: BODY?;

def autoname(expr_fn):
    def _fn(*arg, **kwarg):
        e = expr_fn(*arg, **kwarg)
        e.name = expr_fn.__name__
        return e
    return _fn

@autoname
def body():
    return BODY.maybe()

# subhdrval: UNSTRUCT? EOL;
@autoname
def subhdrval():
    return UNSTRUCT.maybe() + EOL

# hdrval: subhdrval*(WS);
@autoname
def hdrval():
    return subhdrval().zero_or_more(joiner=WS)

# hdr: HDRNAME ':' hdrval;
@autoname
def hdr():
    return HDRNAME + COLON + hdrval()

@autoname
def msg():
    return hdr().zero_or_more() + EOL + body() + DONE

main = msg()
for expr in set(main):
    if not isinstance(expr, Token):
        print(expr.name, ':=', expr)

# for expr in main:
#     print(expr)

#######

print("#######")

import re

print("""

struct imf_stack_t;
typedef struct imf_stack_t imf_stack_t;
struct imf_stack_t {
    imf_stack_type_e t;
    imf_stack_value_u v;
};

typedef call_t {
    // a function to call
    parse_fn_t fn;

    // the point on the stack where we previously were
    int prev;

    // our position in the call
    int state;

    // a mask of all allowable tokens that could follow the expression
    token_mask_t follow;
};

typedef union imf_stack_t {
    call_t call;
};

""")

class Term:
    pass

class MatchToken(Term):
    def __init__(self, token, term_idx, name=None):
        self.token = token
        self.term_idx = term_idx
        self.name = name

    def nstates(self):
        return 1

    def gen(self, after):
        print("    // match " + str(self.token))
        print("    if(call->state == " + str(self.term_idx) + "){")
        print("        AWAIT_TOKEN;")
        print("        if(*token != " + str(self.token) + ") SYNTAX_ERROR;")
        print("        TAKE_TOKEN;")
        print("        call->state++;")
        print("    }")
        print("")

class MaybeToken(Term):
    def __init__(self, token, term_idx, name=None):
        self.token = token
        self.term_idx = term_idx
        self.name = name

    def nstates(self):
        return 1

    def gen(self, after):
        print("    // maybe match " + str(self.token))
        print("    if(call->state == " + str(self.term_idx) + "){")
        print("        AWAIT_TOKEN;")
        print("        if(*token == " + str(self.token) + "){")
        print("            TAKE_TOKEN;")
        print("        }")
        print("        call->state++;")
        print("    }")
        print("")

class MatchExpression(Term):
    def __init__(self, expr, term_idx, name=None):
        self.expr = expr
        self.term_idx = term_idx
        self.name = name

    def nstates(self):
        return 2

    def gen(self, after):
        print("    // match " + str(self.expr))
        print("    if(call->state == " + str(self.term_idx) + "){")
        print("        call->state++;")
        print("        CALL(" + self.expr.parse_fn() + ");")
        print("    }")
        print("    if(call->state == " + str(self.term_idx + 1) + "){")
        print("        // USER CODE")
        print("    }")
        print("")


class ZeroOrMoreExpression(Term):
    def __init__(self, expr, term_idx, name=None, joiner=None):
        self.expr = expr
        self.term_idx = term_idx
        self.name = name
        self.joiner = joiner

    def nstates(self):
        return 3

    def gen(self, after):
        print("    // match zero or more " + str(self.expr))
        print("    while(call->state < " + str(self.term_idx + 2) + "){")
        print("        if(call->state == " + str(self.term_idx) + "){")
        print("            AWAIT_TOKEN;")
        print("            if(" + self.expr.starts_with_fn() + "(*token){")
        print("                call->state++;")
        print("                return CALL(" + self.expr.parse_fn() + ")")
        print("            }else{")
        print("                call->state += 2;")
        print("            }")
        print("        }")
        print("        if(call->state == " + str(self.term_idx + 1) + "){")
        print("            // USER CODE")
        print("            call->state--;")
        print("        }")
        print("    }")
        print("")


class Expression:
    def __init__(self, name):
        self.name = name
        self.term_idx = 0
        self.terms = []

    def __str__(self):
        return self.name

    def parse_fn(self):
        return "_parse_" + self.name

    def starts_with_fn(self):
        return "_" + self.name + "_starts_with"

    def match(self, obj, name=None):
        if isinstance(obj, Token):
            term = MatchToken(obj, self.term_idx, name)
        else:
            term = MatchExpression(obj, self.term_idx, name)
        self.term_idx += term.nstates()
        self.terms.append(term)
        return term

    def zero_or_more(self, obj, name=None, *, joiner=None):
        if isinstance(obj, Token):
            1/0
        elif isinstance(obj, Expression):
            term = ZeroOrMoreExpression(obj, self.term_idx, name, joiner)
        else:
            1/0
        self.term_idx += term.nstates()
        self.terms.append(term)
        return term

    def maybe(self, obj, name=None):
        if isinstance(obj, Token):
            term = MaybeToken(obj, self.term_idx, name)
        elif isinstance(obj, Expression):
            1/0
        else:
            1/0
        self.term_idx += term.nstates()
        self.terms.append(term)
        return term

    def exec(self, code):
        variables = re.findall("\\$[a-zA-Z_][a-zA-Z0-9_]*", code)
        for v in variables:
            name = v[1:]
            assert name in self.named_terms

        # TODO: sort by longest name first to avoid bad substitutions!
        code = code.replace("$$", "(stack)")
        for name, term in self.named_terms.items():
            code = code.replace("$" + name, "(" + str(term) + ")")

        print("        " + code)

    def gen(self, after):
        print("int stack _parse_" + self.name + "(")
        print("    derr_t *e,")
        print("    imf_parser_t *p,")
        print("    int call_pos,")
        print("    imf_token_t *token")
        print("){")
        print("    call_t *call = p->stack[call_pos].call")
        print("    int stack = call_pos;")
        print("")

        for term in self.terms:
            term.gen(None)

        print("    return call->prev;")
        print("}")
        print("")

# msg: hdrs* EOL body DONE;
# hdr: HDRNAME ':' hdrval;
# hdrval: subhdrval*(WS);
# subhdrval: UNSTRUCT? EOL;
# body: BODY?;

subhdrval = Expression("subhdrval")
subhdrval.maybe(UNSTRUCT)
subhdrval.match(EOL)

hdrval = Expression("hdrval")
hdrval.zero_or_more(subhdrval, joiner=WS)

hdr = Expression("hdr")
hdr.match(HDRNAME)
hdr.match(COLON)
hdr.match(hdrval)

hdrs = Expression("hdrs")
hdrs.zero_or_more(hdr)

msg = Expression("msg")
msg.match(hdrs)
msg.match(HDRNAME, "h")
msg.match(BODY, "b")
# msg.exec("$$ = $b")



# subhdrval.gen(None)
# hdrval.gen(None)
# hdr.gen(None)
# hdrs.gen(None)
msg.gen(None)
