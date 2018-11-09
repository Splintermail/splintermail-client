#ifndef IX_H
#define IX_H

struct loop_t;
struct ixu_t;
struct ixs_t;
struct ixc_t;
struct ssl_context_t;

typedef struct loop_t loop_t;
typedef struct ixu_t ixu_t;
typedef struct ixs_t ixs_t;
typedef struct ixc_t ixc_t;

/* ix_t ("IMAP context") is just a tagged union of other types of contexts.
   Mostly this is necessary because when walking through the handles associated
   with a libuv loop using uv_walk(), there's no built-in way to identify the
   type of handle you are looking at, and I have no desire to maintain separate
   lists of handles manually. */

enum ix_type_e {
    IX_TYPE_LOOP,
    IX_TYPE_LISTENER,
    IX_TYPE_USER,
    IX_TYPE_SESSION,
    IX_TYPE_COMMAND,
};

union ix_data_u {
    struct loop_t *loop;
    struct ixu_t *ixu;
    struct ixs_t *ixs;
    struct ixc_t *ixc;
    // for listener sockets, we only need a single pointer:
    struct ssl_context_t *ssl_ctx;
};

typedef struct {
    enum ix_type_e type;
    union ix_data_u data;
} ix_t;

#endif // IX_H
