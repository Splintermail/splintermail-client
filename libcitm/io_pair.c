#include "libcitm/libcitm.h"

typedef struct {
    io_pair_cb cb;
    void *data;

    // conn_dn is already connected when io_pair_t is created
    citm_conn_t *conn_dn;

    citm_connect_i *connect;

    // to be cancelable, the citm tracks us
    link_t link;  // citm->io_pairs;
} io_pair_t;
DEF_CONTAINER_OF(io_pair_t, link, link_t)

static void connect_cb(void *data, citm_conn_t *conn_up, derr_t e){
    io_pair_t *io_pair = data;
    // always remove ourselves from citm's list
    link_remove(&io_pair->link);
    if(is_error(e)){
        if(e.type != E_CANCELED){
            /* XXX: is dumping the error the best thing?
                    Maybe explain what it affected? */
            DUMP(e);
        }
        DROP_VAR(&e);
        // failure: close the conn_dn and clean up
        io_pair->conn_dn->close(io_pair->conn_dn);
        free(io_pair);
        return;
    }
    // success: clean ourselves up and make the callback
    citm_conn_t *conn_dn = io_pair->conn_dn;
    io_pair_cb cb = io_pair->cb;
    void *cb_data = io_pair->data;
    free(io_pair);
    cb(cb_data, conn_dn, conn_up);
}

void io_pair_new(
    citm_io_i *io,
    citm_conn_t *conn_dn,
    io_pair_cb cb,
    void *data,
    link_t *list
){
    derr_t e = E_OK;

    io_pair_t *io_pair = DMALLOC_STRUCT_PTR(&e, io_pair);
    CHECK_GO(&e, fail);

    *io_pair = (io_pair_t){
        .conn_dn = conn_dn,
        .cb = cb,
        .data = data,
    };

    PROP_GO(&e,
        io->connect_imap(io,
            connect_cb,
            io_pair,
            &io_pair->connect
        ),
    fail);

    // success:
    link_list_append(list, &io_pair->link);
    return;

fail:
    // failure: shut down conn_dn
    if(io_pair) free(io_pair);
    conn_dn->close(conn_dn);
    // XXX: is dumping the error the best thing?  Explain what it affected?
    DUMP(e);
    DROP_VAR(&e);
}

// citm can cancel us (it should also remove us from its list)
void io_pair_cancel(link_t *link){
    io_pair_t *io_pair = CONTAINER_OF(link, io_pair_t, link);
    io_pair->connect->cancel(io_pair->connect);
}
