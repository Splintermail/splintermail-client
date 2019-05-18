#ifndef IMAP_ENGINE_H
#define IMAP_ENGINE_H

#include "engine.h"
#include "common.h"
#include "imap_read.h"

struct imape_t;
typedef struct imape_t imape_t;

// defined in imap_hooks_down.c
extern imap_parse_hooks_dn_t imape_hooks_dn;
// defined in imap_hooks_up.c
extern imap_parse_hooks_up_t imape_hooks_up;

typedef struct {
    void *session;
    imape_t *imape;
    // generic per-engine data stuff
    engine_data_state_t data_state;
    event_t start_ev;
    event_t close_ev;
    // IMAP-engine-specific stuff
    bool upwards;
    imap_reader_t reader;
    imap_reader_t reader;
    queue_t pending_reads;
    event_t *read_in;
    queue_cb_t read_in_qcb;

    // imap-session-specific things

} imape_data_t;

struct imape_t {
    bool initialized;
    // generic engine stuff
    uv_work_t work_req;
    session_iface_t session_iface;
    imape_data_t *(*session_get_imape_data)(void*);
    bool (*session_get_upwards)(void*);
    queue_t event_q;
    queue_t write_events;
    // upstream engine, to which we pass write and read_done events
    void *upstream;
    event_passer_t pass_up;
    // for handling quitting state
    bool quitting;
    event_t *quit_ev;
    size_t nwrite_events;
};

derr_t imape_init(imape_t *imape, size_t nwrite_events,
                  session_iface_t session_iface,
                  imape_data_t *(*session_get_imape_data)(void*),
                  bool (*session_get_upwards)(void*),
                  event_passer_t pass_up, void *upstream);
void imape_free(imape_t *imape);

// function is an event_passer_t
void imape_pass_event(void *imape_void, event_t *ev);

void imape_data_start(imape_data_t *id, imape_t *imape, void *session);
void imape_data_close(imape_data_t *id, imape_t *imape, void *session);

enum imape_ref_reason_t {
    IMAPE_REF_READ = 0,
    IMAPE_REF_START_EVENT,
    IMAPE_REF_CLOSE_EVENT,
    IMAPE_REF_LIFETIME,
    IMAPE_REF_MAXIMUM
};

// internal functions, but we will list them here for now
void handle_read(void); // all reads pass through this hook

void cmd_capability(void); // hard coded response
void cmd_noop(void); // requires sync
void cmd_logout(void); // triggers several checks, may require sync
// void cmd_starttls(void); // not implemented for now
// void cmd_authenticate(void); // not implemented for now
void cmd_login(void); // either starts a user context or associates with one
void cmd_select(void); // might launch a new imap client
void cmd_examine(void); // might launch a new imap client
void cmd_create(void); // definitely requires a sync
void cmd_delete(void); // sync
void cmd_rename(void); // sync
void cmd_subscribe(void); // wtf is this good for??
void cmd_unsubscribe(void); // wtf is this good for??
void cmd_list(void); // might require a sync?
void cmd_lsub(void); // might not require a sync?
void cmd_status(void); // sync
void cmd_append(void); // definite sync
void cmd_check(void); // sync I guess, but its a noop locally
void cmd_close(void); // might close an imap client, or some expunging (meaning a sync)
void cmd_expunge(void); // sync
void cmd_search(void); // sync for new messages, but ultimately handled locally
void cmd_fetch(void); // sync if message not found recognized, otherwise handled locally
void cmd_store(void); // sync
void cmd_copy(void); //
void cmd_uid(void); // UID copies of some of the above functions

void resp_status(void); /* an OK, NO, BAD, PREAUTH, or BYE, with optional status code.
                  Might also trigger a command-completed action */
void resp_capability(void); // make sure the capabilities we want are there
void resp_list(void); // Call a LIST hook, I guess?
// void resp_lsub(void); // not going to implement this
// void resp_search(void); // not going to implement this
void resp_flags(void); // make sure the flags we want are there
void resp_exists(void); // at any time, basically
void resp_recent(void); // this will be ignored?  I think I only care about exists
void resp_expunge(void);
void resp_fetch(void); // at any time, basically

#endif // IMAP_ENGINE_H
