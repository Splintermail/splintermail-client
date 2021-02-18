#ifndef LIBIMAILDIR_H
#define LIBIMAILDIR_H

#include <uv.h>

#include "libdstr/libdstr.h"
#include "libimap/libimap.h"
#include "libuvthread/libuvthread.h"
#include "libcrypto/libcrypto.h"

struct imaildir_t;
typedef struct imaildir_t imaildir_t;

#include "util.h"
#include "msg.h"
#include "name.h"
#include "up.h"
#include "dn.h"
#include "imaildir.h"
#include "dirmgr.h"
#include "search.h"

#endif // LIBIMAILDIR_H
