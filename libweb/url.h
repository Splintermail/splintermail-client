#ifndef _WIN32
// unix
    #include <netdb.h>
#else
// windows
    #include <ws2tcpip.h>
#endif

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
