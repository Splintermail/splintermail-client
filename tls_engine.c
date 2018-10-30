#include "tls_engine.h"
#include "common.h"
#include "logger.h"


/*
               ______________________
              |   OpenSSL API BOX    |
              |             ______   |
 BIO_write() -|-> rawin -->|      |--|-> SSL_read() ----> (to IMAP engine)
              |            | SSL* |  |
 BIO_read() <-|- rawout <--|______|<-|- SSL_write() <--- (from IMAP engine)
              |______________________|

    tls engine read_cb:
      - put the data into rawin via BIO_write()
      - try SSL_read()
          - if it works, call IMAP engine read_cb
          - if it fails with WANT_READ or WANT_WRITE, that's fine
      - check if there is any data to write in rawout
          - if so, write it using libuv
          - if not, check if there is

    tls engine write_cb:
      - check if there is more to be written from IMAP engine
          - if so, call SSL_write()



*/

/*
    Application behavior:
        Every imap command will be implemented as a state machine
        Every write will be the following:
            a get_next_chunk() function, for filling write_buffers
            write callbacks will decrement the number of writes_in_flight
            what about write errors?
                I think the connection gets aborted on a lower level

    Because the TLS procedure can inject extra overhead traffic, or do packet
    defrag, it's not a simple 1 to 1 packet relationship between the TLS
    engine's upstream and downstream buffers.  Therefore, there can be no such
    thing as a single context for a single write.

    We could either have a one-write-in-flight strategy or have a separate
    write buffer pool for both sides, or potentially use different strategies
    upstream and downstream.

    If I did this fully multi-threaded, how would I handle the situation where
    the write buffer pool is empty?
      - One thread tries to get a write buffer but there are none
      - thread signs up for "write_buffer_available_cb"
      - when the IO engine releases a write buffer, it dispatches an IMAP
        function thread to continue placing the write request
      - a race condition is possible where another IMAP function steals the
        write buffer and the same thread goes back into the queue, but that
        should be harmless

    What about IMAP context cleanup, while it is in write_buffer_available_cb?


*/


/*
    // internal functions, useful as hooks:
    tlse_encrypt():
        call SSL_write()
        --
        if WANT_READ:
            do nothing, wait for callback
        elif WANT_WRITE:
            (this should never happen, the memory BIO should always accept it)
        --
        if rawout not empty:
            pass rawout to libuv for writing

    tlse_decrypt():
        call SSL_read()
        if WANT_READ:
            do nothing, wait for callback
        if WANT_WRITE:
            (this should never happen, the memory BIO should always accept it)
        if rawout non-empty, pass rawout to libuv for writing

*/



derr_t tlse_decrypt(ixs_t *ixs, bool upwards, dstr_t *in){
    //if(upwards){
    //    SSL* ssl = ssl_up;
    //    BIO* rawin = rawin_up;
    //    BIO* rawout = rawout_up;
    //    dstr_t* decin = decin_up;
    //    dstr_t* decout = decout_up;
    //}else{
    //    SSL* ssl = ssl_dn;
    //    BIO* rawin = rawin_dn;
    //    BIO* rawout = rawout_dn;
    //    dstr_t* decin = decin_dn;
    //    dstr_t* decout = decout_dn;
    //}
    (void)ixs;
    (void)upwards;
    LOG_DEBUG("passed to tls_decrypt: %x", FD(in));
    return E_OK;
}


// how to handle SSL handshakes or other weirdities:

// We have one read callback.
/* If the read callback identifies that there's something to write, we need
   to write it and */
