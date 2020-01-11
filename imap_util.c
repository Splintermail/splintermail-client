#include "imap_util.h"

void imap_cmd_cb_prep(imap_cmd_cb_t *cb, size_t tag, imap_cmd_cb_call_f call,
        imap_cmd_cb_free_f free){
    *cb = (imap_cmd_cb_t){
        .tag = tag,
        .call = call,
        .free = free,
    };
    link_init(&cb->link);
}
