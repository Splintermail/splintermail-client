#include "libdstr/libdstr.h"
#include "libduv/libduv.h"

#include "server/kvpsync/util.h"
#include "server/kvpsync/pkts.h"
#include "server/kvpsync/recv.h"
#include "server/kvpsync/send.h"

#define MIN_RESPONSE (15 * SECOND)
