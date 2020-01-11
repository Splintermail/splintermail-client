#ifndef IMAP_UTIL_H
#define IMAP_UTIL_H

/*
A collection of structs and functions to help write imap clients and servers.
*/

#include "imap_expression.h"

/* imap_cmd_cb_t: a closure for handling responses to imap commands.

   call() may or may not happen, but free() will always happen */
struct imap_cmd_cb_t;
typedef struct imap_cmd_cb_t imap_cmd_cb_t;
typedef derr_t (*imap_cmd_cb_call_f)(imap_cmd_cb_t *cb,
        const ie_st_resp_t *st_resp);
typedef void (*imap_cmd_cb_free_f)(imap_cmd_cb_t *cb);
struct imap_cmd_cb_t {
    // the tag the command was issued with
    size_t tag;
    imap_cmd_cb_call_f call;
    imap_cmd_cb_free_f free;
    link_t link;
};
DEF_CONTAINER_OF(imap_cmd_cb_t, link, link_t);

void imap_cmd_cb_prep(imap_cmd_cb_t *cb, size_t tag, imap_cmd_cb_call_f call,
        imap_cmd_cb_free_f free);

#endif // IMAP_UTIL_H
