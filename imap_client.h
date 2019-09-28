#ifndef IMAP_CLIENT_H
#define IMAP_CLIENT_H

struct imap_controller_up_t;
typedef struct imap_controller_up_t imap_controller_up_t;
struct imap_client_spec_t;
typedef struct imap_client_spec_t imap_client_spec_t;

#include "engine.h"
#include "queue.h"
#include "imap_engine.h"

// calls that a client-side imap logic can make into the controller

// the interface that the imap controller exposes to an imap_client_t
struct imap_controller_up_t {
    // callbacks (in addition to the folder-level updates via imap_maildir_t)
    void (*logged_in)(const imap_controller_up_t*, session_t*);
    void (*uptodate)(const imap_controller_up_t*, session_t*);
    void (*msg_recvd)(const imap_controller_up_t*, session_t*);
    // These should come from the imap session
    // void (*closed)(struct imap_client_spec_t*);
    // void (*error)(struct imap_client_spec_t*, derr_t);
};

/* The (immutable) information requried for creating an upwards imap session.
   The mutable state of the imap client is in imap_client_t (not exposed). */
struct imap_client_spec_t {
    const char *host;
    const char *service;
    dstr_t user;
    dstr_t pass;
};

typedef struct {
    const imap_client_spec_t *spec;
    const imap_controller_up_t *controller;
} imap_client_alloc_arg_t;

// the void* argument should be an imap_client_alloc_arg_t*
derr_t imap_client_logic_alloc(imap_logic_t **out, void *arg_void,
        imape_data_t *id);

// The list of commands the controller can issue
typedef enum {
    IMAP_CLIENT_CMD_LIST,   // list all the folders
    IMAP_CLIENT_CMD_FOLDER, // set the folder to synchronize
    IMAP_CLIENT_CMD_CLOSE,  // logout
} imap_client_command_type_t;

#endif // IMAP_CLIENT_H
