#include "libdstr/libdstr.h"
#include "libuvthread/libuvthread.h"
#include "libimap/libimap.h"
#include "libimaildir/libimaildir.h"
#include "libengine/libengine.h"

// TODO: proper key management
extern keypair_t *g_keypair;

#include "manager.h"

struct wake_event_t;
typedef struct wake_event_t wake_event_t;
struct wake_event_t {
    event_t ev;
    void (*handler)(wake_event_t *wake_ev);
};
DEF_CONTAINER_OF(wake_event_t, ev, event_t);

#include "passthru.h"
#include "fetcher.h"
#include "server.h"
#include "sf_pair.h"
#include "user.h"
#include "citm_engine.h"
