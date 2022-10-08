typedef struct {
    dstr_off_t scheme;
    dstr_off_t user; // userinfo before first colon
    dstr_off_t pass; // userinfo after first colon
    dstr_off_t host;
    dstr_off_t port;
    dstr_off_t path;
    dstr_off_t query;
    dstr_off_t fragment;
} url_t;

#include <liburl/generated/url_parse.h> // generated

// returns bool ok, errbuf can be NULL if you don't want a rendered error
// Recommended errbuf size is 512.
bool parse_url(const dstr_t *text, url_t *out, dstr_t *errbuf);

// returns bool ok, errbuf can be NULL if you don't want a rendered error
// Recommended errbuf size is 512.
bool parse_url_reference(const dstr_t *text, url_t *out, dstr_t *errbuf);

// watch out! url is only valid as long as the dstr_t is
url_t must_parse_url(const dstr_t *text);
