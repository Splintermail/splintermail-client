#include "libcitm/libcitm.h"

typedef struct {
    scheduler_i *scheduler;
    schedulable_t schedulable;

    io_pair_cb cb;
    void *data;
    citm_conn_t *conn_dn;
    citm_connect_i *connect;

    link_t link;  // citm->io_pairs;

    // error belongs to connection attempt, and nothing else
    derr_t e;
    stream_write_t write;

    bool write_started : 1;
    bool shutdown_done : 1;
    bool done : 1;
} io_pair_t;
DEF_CONTAINER_OF(io_pair_t, link, link_t)
DEF_CONTAINER_OF(io_pair_t, schedulable, schedulable_t)

static void advance_state(io_pair_t *io_pair);

static void schedule(io_pair_t *io_pair){
    io_pair->scheduler->schedule(io_pair->scheduler, &io_pair->schedulable);
}

static void scheduled(schedulable_t *schedulable){
    io_pair_t *io_pair = CONTAINER_OF(schedulable, io_pair_t, schedulable);
    advance_state(io_pair);
}

// for the conn_dn stream, only when we're informing it of a failure
static void await_cb(
    stream_i *stream, derr_t e, link_t *reads, link_t *writes
){
    io_pair_t *io_pair = stream->data;
    (void)reads;
    (void)writes;
    if(io_pair->done){
        DROP_CANCELED_VAR(&e);
    }
    if(is_error(e) && !io_pair->done){
        // we didn't cancel it
        TRACE(&e,
            "failure occured while informing client of connection failure\n"
        );
        DUMP(e);
        DROP_VAR(&e);
        io_pair->done = true;
    }
    schedule(io_pair);
}

static void connect_cb(void *data, citm_conn_t *conn_up, derr_t e){
    io_pair_t *io_pair = data;
    if(is_error(e) && e.type != E_CANCELED){
        // non-canceled failure
        TRACE_PROP(&e, e);
        if(io_pair->conn_dn->security != IMAP_SEC_TLS){
            // not TLS, so we have a way to tell the client why
            io_pair->e = e;
            stream_i *stream = io_pair->conn_dn->stream;
            stream->data = io_pair;
            stream->await(stream, await_cb);
            schedule(io_pair);
            return;
        }
    }
    // either success, or non-sharable failure
    link_remove(&io_pair->link);
    citm_conn_t *conn_dn = io_pair->conn_dn;
    io_pair_cb cb = io_pair->cb;
    void *cb_data = io_pair->data;
    free(io_pair);
    cb(cb_data, e, conn_dn, conn_up);
}

// no args are consumed on failure
derr_t io_pair_new(
    scheduler_i *scheduler,
    citm_io_i *io,
    citm_conn_t *conn_dn,
    io_pair_cb cb,
    void *data,
    link_t *list
){
    derr_t e = E_OK;

    io_pair_t *io_pair = DMALLOC_STRUCT_PTR(&e, io_pair);
    CHECK(&e);

    citm_connect_i *connect;
    PROP_GO(&e,
        io->connect_imap(io,
            connect_cb,
            io_pair,
            &connect
        ),
    fail);

    // success

    *io_pair = (io_pair_t){
        .scheduler = scheduler,
        .cb = cb,
        .data = data,
        .conn_dn = conn_dn,
        .connect = connect
    };

    schedulable_prep(&io_pair->schedulable, scheduled);

    link_list_append(list, &io_pair->link);

    return e;

fail:
    free(io_pair);
    return e;
}

static void write_cb(stream_i *stream, stream_write_t *req){
    // we only really care about the shutdown
    (void)stream;
    (void)req;
}

static void shutdown_cb(stream_i *stream){
    io_pair_t *io_pair = stream->data;
    io_pair->shutdown_done = true;
    schedule(io_pair);
}

#define ONCE(x) if(!x && (x = true))

// advance_state is only called after a connection attempt has failed
static void advance_state(io_pair_t *io_pair){
    stream_i *stream = io_pair->conn_dn ? io_pair->conn_dn->stream : NULL;

    if(io_pair->done) goto cu;

    ONCE(io_pair->write_started){
        DSTR_STATIC(msg, "* BYE failed to connect to upstream server\r\n");
        stream_must_write(stream, &io_pair->write, &msg, 1, write_cb);
        stream->shutdown(stream, shutdown_cb);
    }
    if(!io_pair->shutdown_done) return;

    // wrote a reason to the client then shutdown, now close the connection
    io_pair->done = true;

cu:
    if(stream){
        stream->cancel(stream);
        if(!stream->awaited) return;
        io_pair->conn_dn->free(io_pair->conn_dn);
        io_pair->conn_dn = NULL;
    }

    schedulable_cancel(&io_pair->schedulable);
    link_remove(&io_pair->link);
    io_pair_cb cb = io_pair->cb;
    void *cb_data = io_pair->data;
    derr_t e = io_pair->e;
    free(io_pair);
    cb(cb_data, e, NULL, NULL);
}

// citm can cancel us
void io_pair_cancel(link_t *link){
    io_pair_t *io_pair = CONTAINER_OF(link, io_pair_t, link);
    if(!is_error(io_pair->e)){
        // pre-connect; cancel the connection
        io_pair->connect->cancel(io_pair->connect);
    }else{
        // post-connect; cancel informing-the-client
        io_pair->done = true;
        schedule(io_pair);
    }
}
