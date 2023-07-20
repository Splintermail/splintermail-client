#ifndef LIBWEB_H
#define LIBWEB_H

#include "libdstr/libdstr.h"

// types for the parser
typedef struct {
    int code;
    dstr_t reason;
} http_status_line_t;

// headers or parameters
typedef struct {
    dstr_t key;
    dstr_t value;
} http_pair_t;

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

#include <libweb/generated/web_parse.h> // generated
#include "web_scan.h"

#include "http.h"
#include "url.h"
#include "weblink.h"
#include "headers.h"

#endif // LIBWEB_H

