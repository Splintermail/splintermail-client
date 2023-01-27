#ifndef _WIN32
// unix
    #include <netdb.h>
#else
// windows
    #include <ws2tcpip.h>
#endif

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

typedef struct {
    dstr_off_t scheme;
    dstr_off_t host;
    dstr_off_t port;
} addrspec_t;

#include <liburl/generated/url_parse.h> // generated

// returns bool ok, errbuf can be NULL if you don't want a rendered error
// Recommended errbuf size is 512.
bool parse_url_ex(const dstr_t *text, url_t *out, dstr_t *errbuf);
// throws E_PARAM on failed parse
derr_t parse_url(const dstr_t *text, url_t *out);

// returns bool ok, errbuf can be NULL if you don't want a rendered error
// Recommended errbuf size is 512.
bool parse_url_reference_ex(const dstr_t *text, url_t *out, dstr_t *errbuf);
// throws E_PARAM on failed parse
derr_t parse_url_reference(const dstr_t *text, url_t *out);

// watch out! url is only valid as long as the dstr_t is
url_t must_parse_url(const dstr_t *text);

//

// returns bool ok, errbuf can be NULL if you don't want a rendered error
// Recommended errbuf size is 512.
bool parse_addrspec_ex(const dstr_t *text, addrspec_t *out, dstr_t *errbuf);
// throws E_PARAM on failed parse
derr_t parse_addrspec(const dstr_t *text, addrspec_t *out);

// watch out! url is only valid as long as the dstr_t is
addrspec_t must_parse_addrspec(const dstr_t *text);

// out must be freed with freeaddrinfo
derr_t getaddrspecinfo(
    const addrspec_t spec, bool passive, struct addrinfo **out
);
