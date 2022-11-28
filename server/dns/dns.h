typedef size_t (*respond_f)(
    const dns_pkt_t pkt, const lstr_t user, char *out, size_t cap
);

// all defined respond_f's
size_t norespond(const dns_pkt_t, const lstr_t, char*, size_t);
size_t respond_notimpl(const dns_pkt_t, const lstr_t, char*, size_t);
size_t respond_refused(const dns_pkt_t, const lstr_t, char*, size_t);
size_t respond_name_error(const dns_pkt_t, const lstr_t, char*, size_t);
size_t respond_root(const dns_pkt_t, const lstr_t, char*, size_t);
size_t respond_user(const dns_pkt_t, const lstr_t, char*, size_t);
size_t respond_acme(const dns_pkt_t, const lstr_t, char*, size_t);

// always sets *respond and *user
respond_f sort_pkt(const dns_pkt_t pkt, const lstr_t *rname, size_t n);

size_t handle_packet(char *qbuf, size_t qlen, char *rbuf, size_t rcap);
