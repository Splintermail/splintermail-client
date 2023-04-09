typedef enum {
    IMAP_SEC_INSECURE,
    IMAP_SEC_STARTTLS,
    IMAP_SEC_TLS,
} imap_security_e;

struct citm_conn_t;
typedef struct citm_conn_t citm_conn_t;

struct citm_conn_t {
    stream_i *stream;
    imap_security_e security;
    SSL_CTX *ctx;
    // verify_name must be non-empty for non-INSECURE client connections
    // verify_name is expected to have externally-managed .data
    dstr_t verify_name;
    // close() will also free the conn; you can't touch it again
    // .close() is illegal unless you can guarantee there is no pending IO
    void (*close)(citm_conn_t*);
};
DEF_STEAL_PTR(citm_conn_t)

struct citm_connect_i;
typedef struct citm_connect_i citm_connect_i;
struct citm_connect_i {
    void (*cancel)(citm_connect_i*);
};

// conn is NULL on failure
typedef void (*citm_conn_cb)(void*, citm_conn_t *conn, derr_t e);

struct citm_io_i;
typedef struct citm_io_i citm_io_i;
struct citm_io_i {
    derr_t (*connect_imap)(citm_io_i*, citm_conn_cb, void*, citm_connect_i**);
};

/* citm_t is the io-agnostic business logic.

   Everything inside citm_t is io-agnostic to make unit testing easy.

   Different stages of a connection are handled by separate objects, meaning
   that no object has too big of a state machine for reasonable unit testing.

     - stage 1, io_pair_t: after receiving a conn_dn, make a conn_up
     - stage 2, anon_t: with conn_up and conn_dn, until successful login
     - stage 3, preuser_t: create and synchronize a matching keysync_t
     - stage 4, user_t.sf_pair: full blown citm

   citm_t ownership tree:

       citm_t
         - io_pairs[]
         - anons[]
         - preusers[]
             - servers[]
             - clients[]
             - xkey client
         - users[]
             - sf_pairs[]
                - server
                - client
             - xkey client
*/


// citm_t is the io-agnostic business logic
typedef struct {
    citm_io_i *io;
    scheduler_i *scheduler;
    link_t io_pairs;
    link_t anons;
    hashmap_t preusers;
    hashmap_t users;
} citm_t;

derr_t citm_init(citm_t *citm, citm_io_i *io, scheduler_i *scheduler);
void citm_free(citm_t *citm);

void citm_cancel(citm_t *citm);

void citm_new_connection(citm_t *citm, citm_conn_t *conn);

/////

derr_t citm(
   const char *local_host,
   const char *local_svc,
   const char *key,
   const char *cert,
   const char *remote_host,
   const char *remote_svc,
   const string_builder_t *maildir_root,
   bool indicate_ready
);

// this is exposed so that the windows service-handler can call it manually
void stop_loop_on_signal(int signum);
