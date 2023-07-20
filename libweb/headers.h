// parsers for random http headers

// returns bool ok, errbuf can be NULL if you don't want a rendered error
// recommended errbuf size is 512
bool parse_retry_after_ex(const dstr_t text, time_t *out, dstr_t *errbuf);

// throws E_PARAM on failed parse
derr_t parse_retry_after(const dstr_t text, time_t *out);
