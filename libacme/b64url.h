derr_type_t bin2b64url_quiet(const dstr_t bin, dstr_t *b64);
derr_t bin2b64url(const dstr_t bin, dstr_t *b64);

derr_type_t _fmt_b64url(const fmt_i *iface, writer_i *out);
#define FB64URL(d) (&(_fmt_dstr_t){ {_fmt_b64url}, d }.iface)

derr_type_t _jdump_b64url(jdump_i *iface, writer_i *out, int indent, int pos);
#define DB64URL(d) (&(_jdump_dstr_t){{_jdump_b64url}, d}.iface)

derr_type_t b64url2bin_quiet(const dstr_t b64, dstr_t *bin);
derr_t b64url2bin(const dstr_t b64, dstr_t *bin);
