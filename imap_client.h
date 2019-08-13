#ifndef IMAP_CLIENT_H
#define IMAP_CLIENT_H

// The (immutable) information requried for creating an upwards imap session.
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

// The mutable state of an upwards imap session.
typedef struct {
    // *spec should be set during imape_data_prestart
    imap_client_spec_t *spec;
    // which folder to be syncing
    dstr_t folder;
} imap_client_t;

// The list of commands the controller can issue
typedef enum {
    IMAP_CLIENT_CMD_LIST,   // list all the folders
    IMAP_CLIENT_CMD_FOLDER, // set the folder to synchronize
    IMAP_CLIENT_CMD_CLOSE,  // logout
} imap_client_command_type_t;

#endif // IMAP_CLIENT_H
