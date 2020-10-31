#ifndef MYSQL_HELPER_H
#define MYSQL_HELPER_H

// mysql throws missing prototype error with gcc, so ignore that warning
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#endif // __GNUC__

#include <mysql.h>

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif // __GNUC__

#include "libdstr/libdstr.h"

// an error in the SQL library
derr_type_t E_SQL;

#endif // MYSQL_HELPER_H
