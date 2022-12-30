#include "libdstr/libdstr.h"
#include "libduv/libduv.h"

#include "server/libkvpsync/util.h"
#include "server/libkvpsync/pkts.h"
#include "server/libkvpsync/recv.h"
#include "server/libkvpsync/send.h"

#define MIN_RESPONSE (15 * SECOND)
