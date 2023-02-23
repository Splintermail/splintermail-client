// based on https://stackoverflow.com/a/24010059
#define M_NARGS(...) M_NARGS_(__VA_ARGS__, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define M_NARGS_(_10, _9, _8, _7, _6, _5, _4, _3, _2, _1, N, ...) N

// concatenation
#define M_CONC(A, B) M_CONC_(A, B)
#define M_CONC_(A, B) A##B

//
#define M_GET_ELEM(N, ...) M_CONC(M_GET_ELEM_, N)(__VA_ARGS__)
#define M_GET_ELEM_0(_0, ...) _0
#define M_GET_ELEM_1(_0, _1, ...) _1
#define M_GET_ELEM_2(_0, _1, _2, ...) _2
#define M_GET_ELEM_3(_0, _1, _2, _3, ...) _3
#define M_GET_ELEM_4(_0, _1, _2, _3, _4, ...) _4
#define M_GET_ELEM_5(_0, _1, _2, _3, _4, _5, ...) _5
#define M_GET_ELEM_6(_0, _1, _2, _3, _4, _5, _6, ...) _6
#define M_GET_ELEM_7(_0, _1, _2, _3, _4, _5, _6, _7, ...) _7
#define M_GET_ELEM_8(_0, _1, _2, _3, _4, _5, _6, _7, _8, ...) _8
#define M_GET_ELEM_9(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, ...) _9
#define M_GET_ELEM_10(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, ...) _10

#define M_GET_NONLAST(N, ...) M_CONC(M_GET_NONLAST_, N)(__VA_ARGS__)
#define M_GET_NONLAST_0(a, ...)
#define M_GET_NONLAST_1(a,b, ...) a
#define M_GET_NONLAST_2(a,b,c, ...) a,b
#define M_GET_NONLAST_3(a,b,c,d, ...) a,b,c
#define M_GET_NONLAST_4(a,b,c,d,e, ...) a,b,c,d
#define M_GET_NONLAST_5(a,b,c,d,e,f, ...) a,b,c,d,e
#define M_GET_NONLAST_6(a,b,c,d,e,f,g, ...) a,b,c,d,e,f
#define M_GET_NONLAST_7(a,b,c,d,e,f,g,h, ...) a,b,c,d,e,f,g
#define M_GET_NONLAST_8(a,b,c,d,e,f,g,h,i, ...) a,b,c,d,e,f,g,h
#define M_GET_NONLAST_9(a,b,c,d,e,f,g,h,i,j, ...) a,b,c,d,e,f,g,h,i
#define M_GET_NONLAST_10(a,b,c,d,e,f,g,h,i,j,k, ...) a,b,c,d,e,f,g,h,i,j,

// Get last argument - placeholder decrements by one
#define M_GET_LAST(...) M_GET_ELEM(M_NARGS(__VA_ARGS__), _, __VA_ARGS__ ,,,,,,,,,,,)
#define M_NONLAST(...) M_GET_NONLAST(M_NARGS(__VA_ARGS__), _, __VA_ARGS__ ,,,,,,,,,,,)

