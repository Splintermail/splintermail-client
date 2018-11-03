#include "tls_engine.h"
#include "ixs.h"
#include "common.h"
#include "logger.h"

static void log_ssl_errors(void){
    unsigned long e;
    while( (e = ERR_get_error()) ){
        char buffer[256];
        ERR_error_string_n(e, buffer, sizeof(buffer));
        LOG_ERROR("OpenSSL error: %x\n", FS(buffer));
    }
}

/*
void tlse_raw_read(ix_t *ix){
    // put packet in rawin
    // try_ssl_read()
}

void tlse_raw_write_done(ix_t *ix){
    // decrement writes_in_flight
    // try_raw_write()
}

void tlse_dec_read_done(ix_t *ix){
    // reads_in_flight = 0
    // try_ssl_read()
}

void tlse_dec_write(ix_t *ix){
    // store buffer as ssl_write_buffer
    // try_ssl_write()
}

static void try_ssl_read(ix_t *ix){
    // if reads_in_flight == 0 and rawin not empty:
    //     attempt SSL_read()
    //     if success:
    //         imape_read()
    //         reads_in_flight = 1
    //     libuv_read_done()
    //     try_raw_write()
}
static void try_ssl_write(ix_t *ix){
    // if ssl_write_buffer NOT empty and rawout IS empty:
    //     attempt SSL_write()
    //     try_raw_write()
    //     if SSL_write success:
    //         release ssl_write_buffer
    //         imape_write_done()
}
static void try_raw_write(ix_t *ix){
    // if rawout not empty and writes_in_flight < 2:
    //     add write with uv_write
    //     try_ssl_write
}
*/


// internal, between the two subnodes

static void try_raw_write(ix_t *ix);
static void try_ssl_write(ix_t *ix);
static void try_ssl_read(ix_t *ix);

derr_t tlse_raw_read(ix_t *ix, read_buf_t *rb){
    derr_t error = E_OK;
    ixs_t* ixs = ix->data.ixs;
    bool upwards = (IX_TYPE_SESSION_UP == ix->type);
    BIO *rawin = upwards ? ixs->rawin_up : ixs->rawin_dn;

    // do a sanity check before casting size_t to int
    if(rb->dstr.size > INT_MAX){
        ORIG_GO(E_INTERNAL, "buffer is WAY too big", release_buf);
    }
    int len = (int)rb->dstr.len;

    // write input buffer to rawin
    int ret = BIO_write(rawin, rb->dstr.data, len);
    if(ret != len){
        ORIG_GO(E_INTERNAL, "writing to memory buffer failed", release_buf);
    }

release_buf:
    // TODO: devise a better way to get the buffer pool pointer
    loop_t* loop = upwards ? ixs->sock_up.loop : ixs->sock_dn.loop;
    // release read_buf (this is separate from the read_done callback)
    llist_append(loop->read_bufs, &rb->llist_elem);
    if(error) return error;

    // attempt to push the SSL read through, which will trigger further action
    PROP( try_ssl_read(ix_t *ix) );

    return E_OK;
}


derr_t tlse_raw_write_done(ix_t *ix){
    ixs_t* ixs = ix->data.ixs;
    bool upwards = (IX_TYPE_SESSION_UP == ix->type);
    // decrement writes_in_flight
    // try_raw_write()
}


derr_t tlse_dec_read_done(ix_t *ix){
    ixs_t* ixs = ix->data.ixs;
    bool upwards = (IX_TYPE_SESSION_UP == ix->type);
    // reads_in_flight = 0
    // try_ssl_read()
}


derr_t tlse_dec_write(ix_t *ix){
    ixs_t* ixs = ix->data.ixs;
    bool upwards = (IX_TYPE_SESSION_UP == ix->type);
    // store buffer as ssl_write_buffer
    // try_ssl_write()
}


static derr_t try_ssl_read(ix_t *ix){
    ixs_t* ixs = ix->data.ixs;
    bool upwards = (IX_TYPE_SESSION_UP == ix->type);
    size_t *tls_reads = upwards ? ixs->tls_reads_up : ixs->tls_reads_dn;
    BIO *rawin = upwards ? ixs->rawin_up : ixs->rawin_dn;
    SSL *ssl = upwards ? ixs->ssl_up : ixs->ssl_dn;

    // don't read if rawin is empty or if we already have a read in flight
    if(*tls_reads != 0 || BIO_eof(rawin)){
        return E_OK;
    }

    // get a buffer to write into
    // TODO: abstract this somehow
    dstr_t *buf = upwards ? &ixs->decout_up : &ixs->decout_dn;

    // attempt an SSL_read()
    size_t amnt_read;
    int ret = SSL_read(ssl, buf->data, buf.size, &amnt_read);
    if(ret != 1){
        switch(SSL_get_error(ssl, ret)){
            case SSL_ERROR_WANT_READ:
                // this is fine, just break
                break;
            case SSL_ERROR_WANT_WRITE:
                // this should never happen
                ORIG(E_INTERNAL, "got SSL_ERROR_WANT_WRITE");
                break;
            default:
                log_ssl_errors();
                ORIG_GO(E_SSL, "error in SSL_read", fail);
        }
    }else{
        // only if SSL_read succeeded:
        buf->len = amnt_read;
        *tls_reads++;
        // simulate imape_read()
        LOG_ERROR("imape_read(): %x", FD(buf));
        tlse_dec_read_done(ix);
    }

    /* TODO: wait, what if there is extra data to be read in the SSL buffer?
       We definitely need to do something to check for it. */

    // TODO: libuv doesn't have a read_done yet, but we would call it here
    /* the reason we call it here and not right after we put data into the
       rawin memory BIO is because we don't want data to stack up
       forever in the rawin BIO.  However, whether or not the SSL read
       succeeded, at this point we want to signal to the libuv thread that we
       are definitely ready for another packet. */

    PROP( try_raw_write(ix_t *ix) );
}


static derr_t try_ssl_write(ix_t *ix){
    ixs_t* ixs = ix->data.ixs;
    bool upwards = (IX_TYPE_SESSION_UP == ix->type);
    // if ssl_write_buffer NOT empty and rawout IS empty:
    //     attempt SSL_write()
    //     try_raw_write()
    //     if SSL_write success:
    //         release ssl_write_buffer
    //         imape_write_done()
}


static derr_t try_raw_write(ix_t *ix){
    ixs_t* ixs = ix->data.ixs;
    bool upwards = (IX_TYPE_SESSION_UP == ix->type);
    size_t *raw_writes = upwards ? ixs->raw_writes_up : ixs->raw_writes_dn;
    BIO *rawout = upwards ? ixs->rawout_up : ixs->rawout_dn;

    // do nothing if we have multiple writes in flight or if rawout is empty
    if(*raw_writes > 1 || BIO_eof(rawout)){
        return E_OK;
    }

    // add a write to libuv's
    // TODO: abstract this whole section
    uv_tcp_t *sock = upwards? &ixs->sock_up : &ixs->sock_dn;
    uv_write_t req;
    int ret = uv_write(&req, (uv_stream_t*)sock, write_cb);
    if(ret < 0){

    }


    // if rawout not empty and writes_in_flight < 2:
    //     add write with uv_write
    //     try_ssl_write
}
