/* dstr_toi and friends have stricter type enforcement than atoi or strtol.
   Additionally, they result in an error if any part of *in is not part of the
   parsed number.  The idea is that you should know exactly what part of your
   string is a number and you should know what sort of number it is. */
#define INTEGERS_MAP(XX) \
    XX(u, unsigned int) \
    XX(ul, unsigned long) \
    XX(size, size_t) \
    XX(ull, unsigned long long) \
    XX(u64, uint64_t) \
    XX(umax, uintmax_t) \
    XX(i, int) \
    XX(l, long) \
    XX(ll, long long) \
    XX(i64, int64_t) \
    XX(imax, intmax_t)


derr_type_t dstr_tou_quiet(const dstr_t in, unsigned int *out, int base);
derr_type_t dstr_toul_quiet(const dstr_t in, unsigned long *out, int base);
derr_type_t dstr_tosize_quiet(const dstr_t in, size_t *out, int base);
derr_type_t dstr_toull_quiet(const dstr_t in, unsigned long long *out, int base);
derr_type_t dstr_tou64_quiet(const dstr_t in, uint64_t *out, int base);
derr_type_t dstr_toumax_quiet(const dstr_t in, uintmax_t *out, int base);

derr_type_t dstr_toi_quiet(const dstr_t in, int *out, int base);
derr_type_t dstr_tol_quiet(const dstr_t in, long *out, int base);
derr_type_t dstr_toll_quiet(const dstr_t in, long long *out, int base);
derr_type_t dstr_toi64_quiet(const dstr_t in, int64_t *out, int base);
derr_type_t dstr_toimax_quiet(const dstr_t in, intmax_t *out, int base);

#define DECLARE_DSTR_TO_INTEGER(suffix, type) \
    derr_t dstr_to ## suffix(const dstr_t *in, type *out, int base);
INTEGERS_MAP(DECLARE_DSTR_TO_INTEGER)
#undef DECLARE_DSTR_TO_INTEGER
