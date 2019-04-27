#ifndef TLS_ENGINE_H
#define TLS_ENGINE_H

#include "common.h"
#include "engine.h"
#include "networking.h"

/*
   The TLS engine is basically three engine-level resources and a bunch of
   session-level resources.  The drawing below illustrates how most of the
   engine-level and session-level resources fit together for a single session.

   Not illustrated below are:
    - READ_DONE events, which get put in the tlse_t's read_events pool
    - read_events, from which the tlse_data_t's read_out is drawn
    - WRITE_DONE events, which get put in the tlse_t's write_events pool
    - write_events, from which the tlse_data_t's write_out is drawn
              __________________________________________________
             |                TLS ENGINE                        |
             |               ________                           |
   READ ------------------> |  ______|_<---------------------------- WRITE
             |              | |  ______|_                       |
             |              |_| | event  |                      |
             |                |_|  queue |                      |
             |                  |________|                      |
             |          (READs)     /\     (WRITEs)             |
             |      +--------------/  \--------------+          |
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
             |            |            | SSL | |            |   |
             |            |            | OBJ | |            |   |
    WRITE <-- write_out <-- rawout <-- |_____| | write_in <-+   |
             |            |____________________|    |           |
             |                                      +--------------> WRITE_DONE
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
     - Events which reference the session but are sitting in queues
     - If a tlse_data_t is awaiting one of:
        - tlse_data.pending_reads
        - tlse_data.pending_writes
        - tlse.read_events
        - tlse.write_events
     - During the processing of a session advance_state(), a reference is
       required to be held external to advance_state(), ensuring that the
       session does not accidentally disappear in the middle of processing
*/

struct tlse_data_t;
typedef struct tlse_data_t tlse_data_t;

// the TLS Engine, one per pipeline
typedef struct {
    bool initialized;
    // generic engine stuff
    uv_work_t work_req;
    session_iface_t session_iface;
    tlse_data_t *(*session_get_tlse_data)(void*);
    ssl_context_t *(*session_get_ssl_ctx)(void*);
    queue_t event_q;
    queue_t read_events;
    queue_t write_events;
    // downstream engine, to which we pass read and write_done events
    void *upstream;
    event_passer_t pass_up;
    // upstream engine, to which we pass write and read_done events
    void *downstream;
    event_passer_t pass_down;
    // for handling quitting state
    bool quitting;
    event_t *quit_ev;
    size_t nwrite_events;
} tlse_t;


typedef enum {
    TLS_STATE_IDLE,
    TLS_STATE_WAITING_FOR_EMPTY_WRITE_BIO,
    TLS_STATE_CLOSED,
} tls_state_t;


// TLS engine's per-session state
struct tlse_data_t {
    void *session;
    tlse_t *tlse;
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
    event_t *read_out;
    event_t *write_in;
    event_t *write_out;
    /* Why complicate things by using the full queue callback API, when you
       could instead just call advance_state() every time you passed a session
       another packet?  Because there's not any other way to identify which
       session is the first session waiting for in the engine-level queues.
       For example, when one session returns its unsused write_out to
       tlse->write_events, that session doesn't know which other session needs
       to claim the newly released write_event, and thus cannot call
       advance_state() correctly. */
    bool awaiting_read_in;
    bool awaiting_read_out;
    bool awaiting_write_in;
    bool awaiting_write_out;
    bool want_read;
    queue_cb_t read_out_qcb;
    queue_cb_t read_in_qcb;
    queue_cb_t write_out_qcb;
    queue_cb_t write_in_qcb;
};

derr_t tlse_init(tlse_t *tlse, size_t nread_events, size_t nwrite_events,
                 session_iface_t session_iface,
                 tlse_data_t *(*session_get_tlse_data)(void*),
                 ssl_context_t *(*session_get_ssl_ctx)(void*),
                 event_passer_t pass_up, void *upstream,
                 event_passer_t pass_down, void *downstream);

void tlse_free(tlse_t *tlse);

derr_t tlse_add_to_loop(tlse_t *tlse, uv_loop_t *loop);

// function is an event_passer_t
void tlse_pass_event(void *tlse_void, event_t *ev);

void tlse_data_start(tlse_data_t *td, tlse_t *tlse, void *session);
void tlse_data_close(tlse_data_t *td, tlse_t *tlse, void *session);

#endif // TLS_ENGINE_H
