#ifndef TLS_ENGINE_H
#define TLS_ENGINE_H

#include "libdstr/libdstr.h"
#include "engine.h"
#include "networking.h"

/*
   The TLS engine is basically three engine-level resources and a bunch of
   session-level resources.  The drawing below illustrates how most of the
   engine-level and session-level resources fit together for a single session.

   Not illustrated below are:
    - READ_DONE events, which get put in the tlse_t's read_events pool
    - read_events, a queue from which the tlse_data_t's read_out is drawn
    - WRITE_DONE events, which get put in the tlse_t's write_events pool
    - write_events, a queue from which the tlse_data_t's write_out is drawn
              __________________________________________________
             |                TLS ENGINE                        |
             |               ________                           |
   READ ------------------> |  ______|_<---------------------------- WRITE
             |              | |  ______|_                       |
             |              |_| | event  |                      |
             |                |_|  queue |                      |
             |                  |________|                      |
             |          (READs)     /\     (WRITEs)             |
             |       ______________/  \______________           |
 tlse_t      |      |                                |          | tlse_t
 - - - - - - - - - -|- - - - - - - - - - - - - - - - | - - - - -|- - - - - -
 tlse_data_t |      |                                |          | tlse_data_t
             |  ____v____                            |          |
             | |  _______|_                      ____v____      |
             | | |  _______|_                   |  _______|_    |
             | |_| | pending |                  | |  _______|_  |
             |   |_|  reads  |                  |_| | pending | |
             |     |_________|                    |_| writes  | |
             |       |     ____________________     |_________| |
             |       |    |   TLS ENGINE DATA  |            |   |
             |       v    |             _____  |            |   |
 READ_DONE <--- read_in ---> rawin --> |     | | read_out --------> READ
             |            |   BIO      | SSL | |            |   |
             |            |            | OBJ | |            |   |
    WRITE <-- write_out <-- rawout <-- |_____| | write_in <-+   |
             |            |  BIO               |    |           |
             |            |____________________|    +--------------> WRITE_DONE
             |__________________________________________________|

   There are three SSL operations, SSL_read, SSL_write, and SSL_do_handshake.

   Those operations touch the following buffers directly: rawin, rawout,
   read_out, and write_in.  However,

   Operation involves:  | rawin | rawout | read_out | write_in
   ---------------------|-------|--------|----------|---------
   SSL_read             |   x   |   x    |          |    x
   SSL_write            |   x   |   x    |    x     |
   SSL_do_handshake     |   x   |   x    |          |

   Much of the complexity of the code involves states where we are trying to
   make (for example) an SSL_read possible but we don't have a read_out yet, or
   an SSL_write possible while we have a write_in, but a handshake is required
   so SSL_write returns SSL_ERROR_WANT_READ and we have to wait for a READ
   event to come through.


   Reference counting in the TLS engine happens in the following places:
     - READ or WRITE events passed by this session
     - The start events and close events, from tlse_data_start until each event
       is handled by tls_process_events()
*/

struct tlse_data_t;
typedef struct tlse_data_t tlse_data_t;

// the TLS Engine, one per pipeline
typedef struct {
    bool initialized;
    // generic engine stuff
    engine_t engine;
    uv_work_t work_req;
    queue_t event_q;
    queue_t read_events;
    queue_t write_events;
    // upstream engine, to which we pass write and read_done events
    engine_t *upstream;
    // downstream engine, to which we pass read and write_done events
    engine_t *downstream;
    // for handling quitting state
    bool quitting;
    event_t *quit_ev;
    size_t nwrite_events;
} tlse_t;
DEF_CONTAINER_OF(tlse_t, engine, engine_t);


typedef enum {
    TLS_STATE_IDLE,
    TLS_STATE_WAITING_FOR_EMPTY_WRITE_BIO,
    TLS_STATE_CLOSED,
} tls_state_t;


// TLS engine's per-session state
struct tlse_data_t {
    // data to be set by prestart
    session_t *session;
    tlse_t *tlse;
    bool upwards;
    ssl_context_t *ssl_ctx;
    ref_fn_t ref_up;
    ref_fn_t ref_down;
    // generic per-engine data stuff
    engine_data_state_t data_state;
    event_t start_ev;
    event_t close_ev;
    // TLS-engine-specific stuff
    tls_state_t tls_state;
    SSL *ssl;
    BIO *rawin;
    BIO *rawout;
    // lists of unhandled received events
    queue_t pending_reads;
    queue_t pending_writes;
    // events which are being processed
    event_t *read_in;
    event_t *write_in;
    /* after write_in is passed through the SSL buffer, we will store the the
       write_in as write_in_unresponded, then we will enter the
       WAIT_FOR_EMTPY_WRITE_BIO state, and when the last byte is sent we will
       enqueue the WRITE_DONE for write_in_unresponded to happen when the
       WRITE_DONE for that last byte is sent back to us. */
    event_t *write_in_unresponded;
    /* Why complicate things by using the full queue callback API, when you
       could instead just call advance_state() every time you passed a session
       another packet?  Because there's not any other way to identify which
       session is the first session waiting in the engine-level queues. For
       example, when one session returns its unsused write_out to
       tlse->write_events, that session doesn't know which other session needs
       to claim the newly released write_event, and thus cannot call
       advance_state() correctly. */
    queue_cb_t read_out_qcb;
    queue_cb_t read_in_qcb;
    queue_cb_t write_out_qcb;
    queue_cb_t write_in_qcb;
    bool want_read;
    bool eof_recvd;
    bool eof_sent;
    bool tls_eof_recvd;
};
DEF_CONTAINER_OF(tlse_data_t, read_out_qcb, queue_cb_t);
DEF_CONTAINER_OF(tlse_data_t, read_in_qcb, queue_cb_t);
DEF_CONTAINER_OF(tlse_data_t, write_out_qcb, queue_cb_t);
DEF_CONTAINER_OF(tlse_data_t, write_in_qcb, queue_cb_t);

derr_t tlse_init(tlse_t *tlse, size_t nread_events, size_t nwrite_events,
        engine_t *upstream, engine_t *downstream);

void tlse_free(tlse_t *tlse);

derr_t tlse_add_to_loop(tlse_t *tlse, uv_loop_t *loop);

/* prestart() is for setting before any errors can happen and before any
   messages can be sent. */
void tlse_data_prestart(tlse_data_t *td, tlse_t *tlse, session_t *session,
        ref_fn_t ref_up, ref_fn_t ref_down, ssl_context_t *ssl_ctx,
        bool upwards);

void tlse_data_start(tlse_data_t *td);
void tlse_data_close(tlse_data_t *td);

enum tlse_ref_reason_t {
    TLSE_REF_READ = 0,
    TLSE_REF_WRITE,
    TLSE_REF_START_EVENT,
    TLSE_REF_CLOSE_EVENT,
    TLSE_REF_MAXIMUM
};

dstr_t *tlse_ref_reason_to_dstr(enum tlse_ref_reason_t reason);

#endif // TLS_ENGINE_H
