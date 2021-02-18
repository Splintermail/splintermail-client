struct imape_t;
typedef struct imape_t imape_t;
struct imape_data_t;
typedef struct imape_data_t imape_data_t;
struct imape_control_i;
typedef struct imape_control_i imape_control_i;

struct imape_t {
    bool initialized;
    // generic engine stuff
    engine_t engine;
    uv_work_t work_req;
    queue_t event_q;
    queue_t write_events;
    // upstream engine, to which we pass write events
    engine_t *upstream;
    // quit_downstream engine, to which we pass only QUIT events
    engine_t *quit_downstream;
    // for handling quitting state
    bool quitting;
    event_t *quit_ev;
    size_t nwrite_events;
};
DEF_CONTAINER_OF(imape_t, engine, engine_t);

// TODO: make this interface all functions and no data
// (this should happen with a revamp of how extensions are handled)
struct imape_control_i {
    bool is_client;
    extensions_t exts;
};

struct imape_data_t {
    // prestart stuff
    session_t *session;
    imape_t *imape;
    ref_fn_t ref_up;
    ref_fn_t ref_down;
    imape_control_i *control;
    engine_t *downstream;  // where we send READ events
    // generic per-engine data stuff
    engine_data_state_t data_state;
    event_t start_ev;
    event_t close_ev;

    imap_reader_t reader;

    // write subsystem
    queue_cb_t write_qcb;
    bool write_requested;
    event_t *write_ev;
    size_t write_skip;

    link_t unwritten;  // cmd_event_t->ev->link
};
DEF_CONTAINER_OF(imape_data_t, write_qcb, queue_cb_t)

derr_t imape_init(imape_t *imape, size_t nwrite_events, engine_t *upstream,
        engine_t *quit_downstream);
void imape_free(imape_t *imape);
derr_t imape_add_to_loop(imape_t *imape, uv_loop_t *loop);

void imape_data_prestart(imape_data_t *id, imape_t *imape, session_t *session,
        ref_fn_t ref_up, ref_fn_t ref_down, imape_control_i *control,
        engine_t *downstream);
void imape_data_start(imape_data_t *id);
void imape_data_close(imape_data_t *id);

enum imape_ref_reason_t {
    IMAPE_REF_READ = 0,
    IMAPE_REF_WRITE,
    IMAPE_REF_START_EVENT,
    IMAPE_REF_CLOSE_EVENT,
    IMAPE_REF_LIFETIME,
    IMAPE_REF_MAXIMUM
};

dstr_t *imape_ref_reason_to_dstr(enum imape_ref_reason_t reason);

// outgoing READ events are wrapped in imap_event_t's
// incoming WRITE events are wrapped in imap_event_t's

typedef enum {
    IMAP_EVENT_TYPE_CMD,
    IMAP_EVENT_TYPE_RESP,
} imap_event_type_e;

typedef union {
    imap_cmd_t *cmd;
    imap_resp_t *resp;
} imap_event_arg_u;

typedef struct {
    event_t ev;
    imap_event_type_e type;
    imap_event_arg_u arg;
} imap_event_t;
DEF_CONTAINER_OF(imap_event_t, ev, event_t);
