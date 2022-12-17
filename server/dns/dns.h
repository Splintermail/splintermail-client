// lstr_t secret is only set for respond_acme
typedef size_t (*respond_f)(
    void *arg,const dns_pkt_t pkt, char *out, size_t cap
);

// all defined respond_f's
size_t norespond(void *arg, const dns_pkt_t, char*, size_t);
size_t respond_notimpl(void *arg, const dns_pkt_t, char*, size_t);
size_t respond_refused(void *arg, const dns_pkt_t, char*, size_t);
size_t respond_name_error(void *arg, const dns_pkt_t, char*, size_t);
size_t respond_root(void *arg, const dns_pkt_t, char*, size_t);
size_t respond_user(void *arg, const dns_pkt_t, char*, size_t);
size_t respond_acme(void *arg, const dns_pkt_t, char*, size_t);

// always sets *respond and *user
respond_f sort_pkt(const dns_pkt_t pkt, const lstr_t *rname, size_t n);

// a closure for reading from the kvpsync_recv_t
struct kvp_i;
typedef struct kvp_i kvp_i;
struct kvp_i {
    const dstr_t *(*get)(kvp_i*, lstr_t user);
};

// returns zero to not respond
size_t handle_packet(
    char *qbuf, size_t qlen, kvp_i *kvp, char *rbuf, size_t rcap
);
