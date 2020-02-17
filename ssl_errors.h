#ifndef SSL_ERRORS_H
#define SSL_ERRORS_H

#include "libdstr/libdstr.h"

extern derr_type_t E_SSL;        // an encryption-related error

void trace_ssl_errors(derr_t *e);

#endif // SSL_ERRORS_H
