#ifndef IMAP_CLIENT_H
#define IMAP_CLIENT_H

struct imap_client_t;
typedef struct imap_client_t imap_client_t;

#include "imap_engine.h"

/* The (immutable) information requried for creating an upwards imap session.
   The imap controller will pass this struct in when creating an upwards imap
   session, populated with callbacks so the imap engine can tell the controller
   when something important happens. */
typedef struct imap_client_spec_t {
    const char *addr;
    const char *svc;
    dstr_t user;
    dstr_t pass;
    // callbacks (in addition to the folder-level updates via imap_maildir_t)
    void (*logged_in)(struct imap_client_spec_t*);
    void (*uptodate)(struct imap_client_spec_t*);
    void (*msg_recvd)(struct imap_client_spec_t*);
    // These should come from the imap session
    // void (*closed)(struct imap_client_spec_t*);
    // void (*error)(struct imap_client_spec_t*, derr_t);
} imap_client_spec_t;

/* The mutable state of an upwards imap session.  This is created by the imap
   engine.  When an event comes in for an upwards imape_data_t, the imap worker
   will ask call imap_client functions to update this state and it will handle
   any resulting write events. */
struct imap_client_t {
    // *spec should be set during imape_data_prestart
    const imap_client_spec_t *spec;
    // which folder to be syncing
    dstr_t folder;
};

// The list of commands the controller can issue
typedef enum {
    IMAP_CLIENT_CMD_LIST,   // list all the folders
    IMAP_CLIENT_CMD_FOLDER, // set the folder to synchronize
    IMAP_CLIENT_CMD_CLOSE,  // logout
} imap_client_command_type_t;

void imap_client_handle_read_event(imape_data_t *id, const event_t *ev);
void imap_client_handle_command_event(imape_data_t *id, const event_t *ev);
void imap_client_handle_maildir_event(imape_data_t *id, const event_t *ev);

#endif // IMAP_CLIENT_H
