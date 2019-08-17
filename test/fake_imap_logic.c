#include <logger.h>

#include "fake_imap_logic.h"

struct fake_imap_logic_t;
typedef struct fake_imap_logic_t fake_imap_logic_t;

struct fake_imap_logic_t{
    // interface for imape_data_t
    imap_logic_t logic;
    imape_data_t *id;
};
DEF_CONTAINER_OF(fake_imap_logic_t, logic, imap_logic_t)

static void fake_imap_logic_free(imap_logic_t *logic){
    fake_imap_logic_t *fil = CONTAINER_OF(logic, fake_imap_logic_t, logic);
    free(fil);
}

static void fil_handle_read(imap_logic_t *logic, const event_t *ev){
    derr_t e = E_OK;
    fake_imap_logic_t *fil = CONTAINER_OF(logic, fake_imap_logic_t, logic);
    imape_data_t *id = fil->id;
    imape_t *imape = id->imape;
    session_t *session = id->session;
    // echo the message back for now
    link_t *link = queue_pop_first(&imape->write_events, true);
    event_t *write_ev = CONTAINER_OF(link, event_t, link);
    /* Now that we have taken the event from write_events, it's never
       thread-safe put it back from the worker thread, unless we were
       to signal the main thread to check to possibly send the QUIT_UP.
       So instead, if there is an error, we will just pass the event
       back to the main thread as a WRITE_DONE.  That is why we are
       setting the session and doing a ref_up right now. */
    write_ev->session = session;
    id->ref_up(session, IMAPE_REF_WRITE);
    IF_PROP(&e, dstr_copy(&ev->buffer, &write_ev->buffer)){
        // have the main thread put the event back
        write_ev->ev_type = EV_WRITE_DONE;
        imape->engine.pass_event(&imape->engine, write_ev);
        // close the session
        session->close(session, e);
        PASSED(e);
        imape_data_onthread_close(id);
    }else{
        // send the write
        write_ev->ev_type = EV_WRITE;
        imape->upstream->pass_event(imape->upstream, write_ev);
    }
}

derr_t fake_imap_logic_init(imape_data_t *id, imap_logic_t **out){
    derr_t e = E_OK;
    fake_imap_logic_t *fil = malloc(sizeof(*fil));
    if(fil == NULL){
        *out = NULL;
        ORIG(&e, E_NOMEM, "no memory for malloc");
    }

    fil->id = id;
    fil->logic.handle_read_event = fil_handle_read;
    fil->logic.free = fake_imap_logic_free;
    // only handle reads for now
    fil->logic.handle_command_event = NULL;
    fil->logic.handle_maildir_event = NULL;

    *out = &fil->logic;

    return e;
}
