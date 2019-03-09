#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/err.h>

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
void tlse_raw_read(){
    // put packet in rawin
    // if handshake_completed == false:
    //     try_ssl_handshake()
    // if handshake_completed:
    //     try_ssl_read()
}

void tlse_raw_write_done(){
    // decrement writes_in_flight
    // try_raw_write()
}

void tlse_dec_read_done(){
    // reads_in_flight = 0
    // try_ssl_read()
}

void tlse_dec_write(){
    // store buffer as decin
    // dec_writes_pending = 1
    // if handshake_completed == false:
    //     try_ssl_handshake()
    // if handshake_completed:
    //     try_ssl_write()
}

static void try_ssl_read(){
    // if handshake_completed == false and rawin not empty:
    //     try_ssl_completed()
    // if reads_in_flight == 0 and rawin not empty:
    //     attempt SSL_read() (or SSL_connect(), as appropriate)
    //     if success:
    //         imape_read()
    //         reads_in_flight = 1
    //     libuv_read_done()
    //     try_raw_write()
}
static void try_ssl_write(){
    // if dec_writes_pending == 1 and rawout IS empty:
    //     attempt SSL_write() (or SSL_connect(), as appropriate)
    //     try_raw_write()
    //     if SSL_write success:
    //         dec_writes_pending = 0;
    //         release ssl_write_buffer
    //         imape_write_done()
}
static void try_raw_write(){
    //     add write with uv_write
    //     try_ssl_write()
}
static void try_ssl_handshake(){

}
*/


// internal, between the two subnodes

static derr_t try_raw_write(ixs_t *ixs);
static derr_t try_ssl_write(ixs_t *ixs);
static derr_t try_ssl_read(ixs_t *ixs);
static derr_t try_ssl_handshake(ixs_t *ixs);

static void handle_raw_read(ixs_t *ixs, read_buf_t *rb, derr_t status){
    /* TODO: if rawin is too full, we should store the read_buf for later,
       because every time we empty read_buf into rawin, we need to call
       loop_read_done, which will allow libuv to send us another packet (which
       might not be what we want) */

    derr_t error;

    // pass errors immediately
    PROP_GO(status, pass_error);

    // if the session is invalid, just release the buffer
    if(ixs->is_valid == false){
        loop_read_done(ixs->loop, ixs, rb);
        return;
    }

    // do a sanity check before casting size_t to int
    if(rb->dstr.size > INT_MAX){
        ORIG_GO(E_INTERNAL, "buffer is WAY too big", release_buf);
    }

    // write input buffer to rawin
    size_t written;
    int ret = BIO_write_ex(ixs->rawin, rb->dstr.data, rb->dstr.len, &written);
    if(ret != 1){
        log_ssl_errors();
        ORIG_GO(E_SSL, "writing to memory buffer failed", release_buf);
    }
    if(written != rb->dstr.len){
        ORIG_GO(E_INTERNAL, "BIO rejected some bytes!", release_buf);
    }

    // release the buffer
    loop_read_done(ixs->loop, ixs, rb);

    // do we need to try to connect before we can try to read?
    if(ixs->handshake_completed == false){
        PROP_GO( try_ssl_handshake(ixs), pass_error);
    }

    // attempt an SSL read, which may trigger further action
    if(ixs->handshake_completed == true){
        PROP_GO( try_ssl_read(ixs), pass_error);
    }

    return;

release_buf:
    loop_read_done(ixs->loop, ixs, rb);
pass_error:
    // TODO: don't drop these errors; pass them as an event to IMAP engine
    return;
}

void tlse_raw_read(ixs_t *ixs, read_buf_t *rb, derr_t status){
    // upref session before queueing event
    ixs_ref_up(ixs);

    // queue event here

    // this would be run on the TLS engine thread as an event handler
    handle_raw_read(ixs, rb, status);

    // done with event
    ixs_ref_down(ixs);
}


static void handle_raw_write_done(ixs_t *ixs, write_buf_t *wb, derr_t status){
    derr_t error;

    // decrement raw_writes
    ixs->raw_writes--;

    // return write buffer to pool
    loop_t *loop = ixs->loop;
    queue_append(&loop->write_bufs, &wb->qe);

    /* TODO: skipping straight to pass_error here does not let us attempt any
       "pull" operations to try to clean out buffers.  Right now that is OK but
       it might not always be ok */
    // now, check the error
    PROP_GO(status, pass_error);

    // session valid check
    if(ixs->is_valid == false) return;

    // try and push another raw write
    PROP_GO( try_raw_write(ixs), pass_error);

    return;

pass_error:
    // TODO: pass this error to the next node
    return;
}

void tlse_raw_write_done(ixs_t *ixs, write_buf_t *wb, derr_t status){
    // upref session before queueing event
    ixs_ref_up(ixs);

    // queue event here

    // this would be run on the TLS engine thread as an event handler
    handle_raw_write_done(ixs, wb, status);

    // TLS engine done with event
    ixs_ref_down(ixs);
}


static void handle_dec_read_done(ixs_t *ixs){
    derr_t error;
    // one less read in flight
    ixs->imap_reads--;

    if(ixs->is_valid == false){
        return;
    }

    // try and push another ssl read
    PROP_GO( try_ssl_read(ixs), pass_error);

    return;

pass_error:
    // TODO: actually pass the error
    return;
}

void tlse_dec_read_done(ixs_t *ixs){
    // upref session before queueing event
    ixs_ref_up(ixs);

    // queue event here

    // this would be run on the TLS engine thread as an event handler
    handle_dec_read_done(ixs);

    // TLS engine done with event
    ixs_ref_down(ixs);
}


static void handle_dec_write(ixs_t *ixs){
    derr_t error;

    // we are using the buffer directly, no need to copy anything

    ixs->dec_writes_pending++;

    if(ixs->is_valid == false){
        return;
    }

    // do we need to try to connect before we can try to write?
    if(ixs->handshake_completed == false){
        PROP_GO( try_ssl_handshake(ixs), pass_error);
    }

    if(ixs->handshake_completed == true){
        PROP_GO( try_ssl_write(ixs), pass_error);
    }

pass_error:
    // TODO: pass the error
    return;
}


void tlse_dec_write(ixs_t *ixs){
    // upref session before queueing event
    ixs_ref_up(ixs);

    // queue event here

    // this would be run on the TLS engine thread as an event handler
    handle_dec_write(ixs);

    // TLS engine done with event
    ixs_ref_down(ixs);
}


static derr_t try_ssl_read(ixs_t *ixs){
    // don't read if rawin is empty or if we already have a read in flight
    if(ixs->imap_reads != 0 || BIO_eof(ixs->rawin)){
        return E_OK;
    }

    // attempt an SSL_read()
    size_t amnt_read;
    int ret = SSL_read_ex(ixs->ssl, ixs->decout.data, ixs->decout.size,
                          &amnt_read);
    if(ret != 1){
        switch(SSL_get_error(ixs->ssl, ret)){
            case SSL_ERROR_WANT_READ:
                // this is fine, just break
                break;
            case SSL_ERROR_WANT_WRITE:
                // this should never happen
                ORIG(E_INTERNAL, "got SSL_ERROR_WANT_WRITE");
                break;
            default:
                log_ssl_errors();
                ORIG(E_SSL, "error in SSL_read");
        }
    }
    // only if SSL_read succeeded, take these steps:
    else{
        // store amnt_read
        ixs->decout.len = amnt_read;
        // increment the number of reads pushed to IMAP engine
        ixs->imap_reads++;
        // simulate imape_read():
        {
            LOG_ERROR("imape_read(): %x", FD(&ixs->decout));
            tlse_dec_read_done(ixs);
        }

        /* if there is more data to be read in the SSL buffer, that will be
           checked when IMAP engine calls tlse_dec_read_done(), not here */
    }

    /* whether or not the SSL_read succeeded, we may have generated TLS data
       to write to the socket */

    PROP( try_raw_write(ixs) );

    return E_OK;
}


static derr_t try_ssl_write(ixs_t *ixs){
    derr_t error;
    // do nothing if dec_writes_pending is 0 or rawout is not empty
    if(ixs->dec_writes_pending == 0 || !BIO_eof(ixs->rawout)){
        return E_OK;
    }

    // attempt SSL_write
    size_t written;
    int ret = SSL_write_ex(ixs->ssl, ixs->decin.data, ixs->decin.len, &written);
    if(ret != 1){
        switch(SSL_get_error(ixs->ssl, ret)){
            case SSL_ERROR_WANT_READ:
                // this is fine, just break
                break;
            case SSL_ERROR_WANT_WRITE:
                // we are not going to try again with this buffer
                ixs->dec_writes_pending--;
                // this should never happen
                ORIG_GO(E_INTERNAL, "got SSL_ERROR_WANT_WRITE", release_buf);
                break;
            default:
                // we are not going to try again with this buffer
                ixs->dec_writes_pending--;
                log_ssl_errors();
                ORIG_GO(E_SSL, "error in SSL_read", release_buf);
        }
    }

    // in case of a successful write:
    if(ret == 1){
        /* indicate decin is empty, before attempting to push data via
           try_raw_write, since try_raw_write will attempt to call this
           function again (which would not be useful in this case) */
        ixs->dec_writes_pending--;
    }

    /* regardless of whether write was successful, attempt to push anything in
       rawout (might be a handshake, for instance) to the socket */
    PROP_GO( try_raw_write(ixs), release_buf);

    // only in case of a successful write:
    if(ret == 1){
        // alert IMAP engine that write was successful
        {
            // TODO: call the real function
            LOG_ERROR("imap_write_done()\n");
        }
    }

    return E_OK;

release_buf:
    // in an error situation, be sure to release the buffer

    // alert IMAP engine that write was unsuccessful
    /* TODO: Uh-oh, looks like I have a forking error here, it's being returned
       in two places. */
    {
        // TODO: call the real function
        LOG_ERROR("imap_write_done()\n");
    }

    return error;
}


static derr_t try_ssl_handshake(ixs_t *ixs){
    // attempt SSL_connect
    int ret = SSL_do_handshake(ixs->ssl);
    if(ret != 1){
        switch(SSL_get_error(ixs->ssl, ret)){
            case SSL_ERROR_WANT_READ:
                // this is fine, just break
                break;
            case SSL_ERROR_WANT_WRITE:
                // this should never happen
                ORIG(E_INTERNAL, "got SSL_ERROR_WANT_WRITE");
                break;
            default:
                log_ssl_errors();
                ORIG(E_SSL, "error in SSL_read");
        }
    }else{
        // in case of a successful connect:
        ixs->handshake_completed = true;
    }

    /* regardless of whether handshake was completed, attempt to push anything
       in rawout to the socket */
    PROP( try_raw_write(ixs) );

    return E_OK;
}


static derr_t try_raw_write(ixs_t *ixs){
    derr_t error;
    // do nothing if we have multiple writes in flight or if rawout is empty
    if(ixs->raw_writes > 1 || BIO_eof(ixs->rawout)){
        return E_OK;
    }

    // get a write_buf
    loop_t *loop = ixs->loop;
    queue_elem_t *wait_qcb = &ixs->wait_for_write_buf_qcb;
    write_buf_t *wb = queue_pop_first(&loop->write_bufs, wait_qcb);

    // if there's no buffer available, then just exit now
    if(wb == NULL){
        return E_OK;
    }

    // read from rawout into the write buffer
    size_t amnt_read;
    int ret = BIO_read_ex(ixs->rawout, wb->dstr.data, wb->dstr.size, &amnt_read);
    if(ret != 1 || amnt_read == 0){
        log_ssl_errors();
        ORIG_GO(E_SSL, "reading from memory buffer failed", release_buf);
    }

    // store the length read from rawout
    wb->dstr.len = amnt_read;

    // also store the result into the uv_buf_t
    wb->buf.base = wb->dstr.data;
    wb->buf.len = wb->dstr.len;

    // also associate this ixs with the write_buf
    wb->ixs = ixs;

    // add a write to libuv
    ixs->raw_writes++;

    // errors will be returned via write_done
    loop_add_write(loop, ixs, wb);

    return E_OK;

release_buf:
    // just put the buffer back in the pool, we could not use it
    queue_append(&loop->write_bufs, &wb->qe);
    return error;
}
