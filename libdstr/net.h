typedef struct {
    fmt_i iface;
    const struct sockaddr *sa;
} _fmt_ntop_t;

typedef struct {
    fmt_i iface;
    const struct sockaddr_storage *ss;
} _fmt_ntops_t;

typedef struct {
    fmt_i iface;
    const struct sockaddr_in *sin;
} _fmt_ntop4_t;

typedef struct {
    fmt_i iface;
    const struct sockaddr_in6 *sin6;
} _fmt_ntop6_t;

derr_type_t _fmt_ntop(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_ntops(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_ntop4(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_ntop6(const fmt_i *iface, writer_i *out);

#define FNTOP(sa) (&((_fmt_ntop_t){ {_fmt_ntop}, sa }.iface))
#define FNTOPS(ss) (&((_fmt_ntops_t){ {_fmt_ntops}, ss }.iface))
#define FNTOP4(sin) (&((_fmt_ntop4_t){ {_fmt_ntop4}, sin }.iface))
#define FNTOP6(sin6) (&((_fmt_ntop6_t){ {_fmt_ntop6}, sin6 }.iface))

const struct sockaddr *ss2sa(const struct sockaddr_storage *ss);

uint16_t must_addr_port(const struct sockaddr *sa);
uint16_t must_addrs_port(const struct sockaddr_storage *ss);

derr_type_t addr_copy_quiet(
    const struct sockaddr *in, struct sockaddr_storage *ss
);
derr_t addr_copy(const struct sockaddr *in, struct sockaddr_storage *ss);

derr_t read_addr(struct sockaddr_storage *ss, const char *addr, uint16_t port);

bool addr_eq(const struct sockaddr *a, const struct sockaddr *b);
bool addrs_eq(
    const struct sockaddr_storage *a, const struct sockaddr_storage *b
);
