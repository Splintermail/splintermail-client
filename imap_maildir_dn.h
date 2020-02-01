#ifndef IMAP_MAILDIR_DN_H
#define IMAP_MAILDIR_DN_H

#include "imap_maildir.h"

/* the component of an imap_maildir responsible for synchronizing dnwards */

// dn_t is all the state we have for an upwards connection
typedef struct {
    imaildir_t *m;
    // the interfaced provided to us
    maildir_conn_dn_i *conn;
    // the interface we provide
    maildir_i maildir;
    link_t link;  // imaildir_t->access.dns
} dn_t;
DEF_CONTAINER_OF(dn_t, maildir, maildir_i);
DEF_CONTAINER_OF(dn_t, link, link_t);

derr_t dn_new(dn_t **out, maildir_conn_dn_i *conn, imaildir_t *m);
void dn_free(dn_t **dn);

void send_resp(dn_t *dn, imap_resp_t *resp);

#endif // IMAP_MAILDIR_DN_H