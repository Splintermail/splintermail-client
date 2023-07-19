extern derr_type_t E_SSL;        // an encryption-related error

void trace_ssl_errors(derr_t *e);

derr_type_t _fmt_ssl_errors(const fmt_i *iface, writer_i *out);

#define FSSL (&(fmt_i){ _fmt_ssl_errors })
