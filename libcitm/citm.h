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
    // free() is only safe after the stream is awaited
    void (*free)(citm_conn_t*);
    link_t link;
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
     - stage 4, user_t.sc_t: full blown citm

   citm_t ownership tree:

       citm_t
         - io_pairs[]
            - conn_dn
            - connect_i
         - anons[]
            - server
            - client
         - preusers{}
             - servers[]
             - clients[]
             - xkey client
         - users{}
             - sc[]
                - server
                - client
             - xkey client
         - holds{}
             - servers[]
             - clients[]
*/


// citm_t is the io-agnostic business logic
typedef struct {
    citm_io_i *io;
    scheduler_i *scheduler;
    schedulable_t schedulable;

    string_builder_t root;
    link_t io_pairs;  // io_pair_t->link
    link_t anons;  // anon_t->link
    hashmap_t preusers;  // preuser_t->elem
    hashmap_t users;  // user_t->elem
    hashmap_t holds;  // citm_hold_t->elem

    // objects we are awaiting in order to delete
    struct {
        link_t conns;
        link_t clients;
        link_t servers;
    } closing;

    bool canceled;
} citm_t;

derr_t citm_init(
    citm_t *citm,
    citm_io_i *io,
    scheduler_i *scheduler,
    string_builder_t root
);
void citm_free(citm_t *citm);

void citm_cancel(citm_t *citm);

/* XXX: a good idea, but requires us to get rid of the silly
       "citm doesn't have an advance_state" rule to be useful */
// void citm_dump(citm_t *citm, FILE *f);

void citm_on_imap_connection(citm_t *citm, citm_conn_t *conn);
