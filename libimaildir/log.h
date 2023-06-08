// log.h is not imported as part of libimaildir.h

typedef enum {
    LOG_KEY_UIDVLDS,     // "v" for "validity"
    LOG_KEY_HIMODSEQUP,  // "h" for "high"
    LOG_KEY_MODSEQDN,    // "d" for "down"
    LOG_KEY_MSG,         // "m" for "message"
} log_key_type_e;

typedef union {
    msg_key_t msg_key;
} log_key_arg_u;

typedef struct {
    log_key_type_e type;
    log_key_arg_u arg;
} log_key_t;

derr_t log_key_marshal(log_key_t *lk, dstr_t *out);
derr_t log_key_unmarshal(const dstr_t *key, log_key_t *lk);

derr_t marshal_uidvlds(unsigned int up, unsigned int dn, dstr_t *out);
derr_t parse_uidvlds(const dstr_t in, unsigned int *up, unsigned int *dn);

/*
    Marshaled Log line metadata format:

        1:7:12345:b[:afsdx:DATE]
        | |   |   |    |
        | |   |   |    |
        | |   |   |    flags (if not expunged)
        | |   |   |
        | |   |   "u"nfilled / "f"illed / "e"xpunged / "x": expunge pushed
        | |   |
        | |   modseq number
        | |
        | uid_dn
        |
        version number

    DATE looks like:

       2020.12.25.23.59.59.-7.00
                           |
                           timezone is signed
*/

derr_t marshal_date(imap_time_t intdate, dstr_t *out);
derr_t parse_date(const dstr_t *dstr, imap_time_t *intdate);

derr_t marshal_message(const msg_t *msg, dstr_t *out);
derr_t marshal_expunge(const msg_expunge_t *expunge, dstr_t *out);
// parses either a message or an expunge (they have the same key)
derr_t parse_value(
    const dstr_t in, msg_key_t key,  msg_t **msg, msg_expunge_t **expunge
);
