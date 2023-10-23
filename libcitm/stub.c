#include "libcitm/libcitm.h"

typedef struct {
    scheduler_i *scheduler;
    schedulable_t schedulable;

    stub_cb cb;
    void *data;
    citm_conn_t *conn;

    link_t link;  // uv_citm->stubs;

    stream_write_t write;

    bool write_started : 1;
    bool write_done : 1;
    bool shutdown_done : 1;
    bool done : 1;
} stub_t;
DEF_CONTAINER_OF(stub_t, link, link_t)
DEF_CONTAINER_OF(stub_t, schedulable, schedulable_t)

static void advance_state(stub_t *stub);

static void schedule(stub_t *stub){
    stub->scheduler->schedule(stub->scheduler, &stub->schedulable);
}

static void scheduled(schedulable_t *schedulable){
    stub_t *stub = CONTAINER_OF(schedulable, stub_t, schedulable);
    advance_state(stub);
}

static void failed_await_cb(
    stream_i *s, derr_t e, link_t *reads, link_t *writes
){
    (void)reads;
    (void)writes;
    citm_conn_t *conn = s->data;
    DROP_VAR(&e);
    link_remove(&conn->link);
    conn->free(conn);
}

static void await_cb(
    stream_i *stream, derr_t e, link_t *reads, link_t *writes
){
    stub_t *stub = stream->data;
    (void)reads;
    (void)writes;
    if(is_error(e) && !stub->done){
        // we didn't cancel it
        TRACE(&e, "failure occured while communicating with stub\n");
        DUMP(e);
        DROP_VAR(&e);
        stub->done = true;
    }
    schedule(stub);
}

// just closes conn on error
void stub_new(
    scheduler_i *scheduler,
    citm_conn_t *conn,
    stub_cb cb,
    void *data,
    link_t *list
){
    stub_t *stub = malloc(sizeof(*stub));
    if(!stub) goto fail;

    // success

    *stub = (stub_t){
        .scheduler = scheduler,
        .cb = cb,
        .data = data,
        .conn = conn,
    };

    conn->stream->data = stub;
    conn->stream->await(conn->stream, await_cb);

    schedulable_prep(&stub->schedulable, scheduled);

    link_list_append(list, &stub->link);

    schedule(stub);

    return;

fail:
    LOG_ERROR("failed to allocate stub\n");
    conn->stream->data = conn;
    conn->stream->await(conn->stream, failed_await_cb);
    conn->stream->cancel(conn->stream);
}

static void write_cb(stream_i *stream, stream_write_t *req){
    // we only really care about the shutdown
    (void)stream;
    (void)req;
}

static void shutdown_cb(stream_i *stream){
    stub_t *stub = stream->data;
    stub->shutdown_done = true;
    schedule(stub);
}

static void advance_state(stub_t *stub){
    stream_i *stream = stub->conn ? stub->conn->stream : NULL;

    if(stub->done) goto cu;

    ONCE(stub->write_started){
        DSTR_STATIC(msg, "* BYE installation needs configuring\r\n");
        stream_must_write(stream, &stub->write, &msg, 1, write_cb);
        stream->shutdown(stream, shutdown_cb);
    }
    if(!stub->shutdown_done) return;

    // wrote a reason to the client then shutdown, now close the connection
    stub->done = true;

cu:
    if(stream){
        stream->cancel(stream);
        if(!stream->awaited) return;
        stub->conn->free(stub->conn);
        stub->conn = NULL;
    }

    schedulable_cancel(&stub->schedulable);
    link_remove(&stub->link);
    stub_cb cb = stub->cb;
    void *cb_data = stub->data;
    free(stub);
    cb(cb_data);
}

// citm can cancel us
void stub_cancel(link_t *link){
    stub_t *stub = CONTAINER_OF(link, stub_t, link);
    stub->done = true;
    schedule(stub);
}
