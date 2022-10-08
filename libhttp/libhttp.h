#ifndef LIBHTTP_H
#define LIBHTTP_H

#include "libdstr/libdstr.h"
#include "libparsing/libparsing.h"
#include "liburl/liburl.h"
#include "libduv/libduv.h"

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

#include <libhttp/generated/http_parse.h> // generated
#include "http_scan.h"

#include "http.h"
#include "marshal.h"
#include "borrow.h"
#include "limit.h"
#include "chunked.h"

#endif // LIBHTTP_H
