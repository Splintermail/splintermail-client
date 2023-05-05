#include "libcitm/libcitm.h"

typedef struct {
    io_pair_cb cb;
    void *data;
    citm_conn_t *conn_dn;
    citm_connect_i *connect;
    link_t link;  // citm->io_pairs;
} io_pair_t;
DEF_CONTAINER_OF(io_pair_t, link, link_t)

static void connect_cb(void *data, citm_conn_t *conn_up, derr_t e){
    io_pair_t *io_pair = data;
    // always remove ourselves from citm's list
    link_remove(&io_pair->link);
    // success or failure: clean ourselves up and make the callback
    citm_conn_t *conn_dn = io_pair->conn_dn;
    io_pair_cb cb = io_pair->cb;
    void *cb_data = io_pair->data;
    free(io_pair);
    if(is_error(e) && e.type != E_CANCELED){
        TRACE_PROP(&e);
    }
    cb(cb_data, e, conn_dn, conn_up);
}

// no args are consumed on failure
derr_t io_pair_new(
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
        .cb = cb,
        .data = data,
        .conn_dn = conn_dn,
        .connect = connect
    };

    link_list_append(list, &io_pair->link);

    return e;

fail:
    free(io_pair);
    return e;
}

// citm can cancel us (it should also remove us from its list)
void io_pair_cancel(link_t *link){
    io_pair_t *io_pair = CONTAINER_OF(link, io_pair_t, link);
    io_pair->connect->cancel(io_pair->connect);
}
