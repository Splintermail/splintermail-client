#include "libdstr/libdstr.h"
#include "libduv/libduv.h"
#include "libimap/libimap.h"
#include "libimaildir/libimaildir.h"
#include "libengine/libengine.h"

#include "manager.h"

struct wake_event_t;
typedef struct wake_event_t wake_event_t;
struct wake_event_t {
    event_t ev;
    void (*handler)(wake_event_t *wake_ev);
};
DEF_CONTAINER_OF(wake_event_t, ev, event_t)

/* a struct for passing EV_READ and EV_SESSION_CLOSE events through the citme
   without having to know which type of object the sessions belong to */
typedef struct {
    void (*close)(imap_session_t *s);
    void (*read_ev)(imap_session_t *s, event_t *ev);
} citme_session_owner_i;

// any session-owning object should safely cast to this type
typedef struct {
    imap_session_t s;
    citme_session_owner_i session_owner;
} citme_session_owner_t;
DEF_CONTAINER_OF(citme_session_owner_t, s, imap_session_t)

#include "passthru.h"
#include "fetcher.h"
#include "server.h"
#include "sf_pair.h"
#include "keysync.h"
#include "fpr_watcher.h"
#include "user.h"
#include "citm_engine.h"
#include "citm.h"
#include "date.h"
