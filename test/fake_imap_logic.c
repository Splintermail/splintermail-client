#include <logger.h>

#include "fake_imap_logic.h"

struct fake_imap_logic_t;
typedef struct fake_imap_logic_t fake_imap_logic_t;

struct fake_imap_logic_t{
    // interface for imape_data_t
    imap_logic_t logic;
    imape_data_t *id;
    link_t events;
};
DEF_CONTAINER_OF(fake_imap_logic_t, logic, imap_logic_t)

static void fake_imap_logic_free(imap_logic_t *logic){
    fake_imap_logic_t *fil = CONTAINER_OF(logic, fake_imap_logic_t, logic);
    free(fil);
}

static derr_t fil_new_event(imap_logic_t *logic, event_t *ev){
    derr_t e = E_OK;
    fake_imap_logic_t *fil = CONTAINER_OF(logic, fake_imap_logic_t, logic);
    link_list_append(&fil->events, &ev->link);
    return e;
}

static derr_t fil_handle_read(fake_imap_logic_t *fil, event_t *ev){
    derr_t e = E_OK;
    imape_data_t *id = fil->id;
    imape_t *imape = fil->id->imape;
    session_t *session = fil->id->session;
    // TODO: don't block here
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
    PROP_GO(&e, dstr_copy(&ev->buffer, &write_ev->buffer), fail_write_ev);

    // send the write
    write_ev->ev_type = EV_WRITE;
    imape->upstream->pass_event(imape->upstream, write_ev);

fail_write_ev:
    if(is_error(e)){
        // have the main thread put the event back
        write_ev->ev_type = EV_WRITE_DONE;
        imape->engine.pass_event(&imape->engine, write_ev);
    }

    // always return the read buffer
    ev->ev_type = EV_READ_DONE;
    imape->upstream->pass_event(imape->upstream, ev);

    return e;
}

static derr_t fil_do_work(imap_logic_t *logic){
    derr_t e = E_OK;
    fake_imap_logic_t *fil = CONTAINER_OF(logic, fake_imap_logic_t, logic);

    if(link_list_isempty(&fil->events)){
        return e;
    }

    link_t *link = link_list_pop_first(&fil->events);
    event_t *ev = CONTAINER_OF(link, event_t, link);
    // TODO: handle other events
    if(ev->ev_type != EV_READ){
        ORIG(&e, E_VALUE, "Invalid event for fake_imap_logic");
    }

    PROP(&e, fil_handle_read(fil, ev) );

    return e;
}

static bool fil_more_work(imap_logic_t *logic){
    fake_imap_logic_t *fil = CONTAINER_OF(logic, fake_imap_logic_t, logic);

    return !link_list_isempty(&fil->events);
}

derr_t fake_imap_logic_init(imap_logic_t **out, void* data, imape_data_t *id){
    (void)data;
    derr_t e = E_OK;
    fake_imap_logic_t *fil = malloc(sizeof(*fil));
    if(fil == NULL){
        *out = NULL;
        ORIG(&e, E_NOMEM, "no memory for malloc");
    }

    fil->id = id;
    fil->logic.new_event = fil_new_event;
    fil->logic.do_work = fil_do_work;
    fil->logic.more_work = fil_more_work;
    fil->logic.free = fake_imap_logic_free;

    *out = &fil->logic;

    link_init(&fil->events);

    return e;
}
