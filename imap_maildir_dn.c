#include "imap_maildir_dn.h"
#include "libdstr/logger.h"
#include "imap_util.h"

// forward declarations
static derr_t conn_dn_cmd(maildir_i*, imap_cmd_t*);

static derr_t maildir_resp_not_allowed(maildir_i* maildir, imap_resp_t* resp){
    (void)maildir;
    (void)resp;
    derr_t e = E_OK;
    ORIG(&e, E_INTERNAL, "response not allowed from an downwards connection");
}

derr_t dn_new(dn_t **out, maildir_conn_dn_i *conn, imaildir_t *m){
    derr_t e = E_OK;
    *out = NULL;

    dn_t *dn = malloc(sizeof(*dn));
    if(!dn) ORIG(&e, E_NOMEM, "nomem");
    *dn = (dn_t){
        .m = m,
        .conn = conn,
        .maildir = {
            .cmd = conn_dn_cmd,
            .resp = maildir_resp_not_allowed,
            // TODO: what to do about these functions?
            .synced = NULL,
            .selected = NULL,
            .unselect = NULL,
        },
    };
    link_init(&dn->link);

    *out = dn;

    return e;
};

// dn_free is meant to be called right after imaildir_unregister_dn()
void dn_free(dn_t **dn){
    if(*dn == NULL) return;
    /* it's not allowed to remove the dn_t from imaildir.access.dns here, due
       to race conditions in the cleanup sequence */

    // release the interface
    (*dn)->conn->release((*dn)->conn, E_OK);

    // free memory
    free(*dn);
    *dn = NULL;
}

// we either need to consume the cmd or free it
static derr_t conn_dn_cmd(maildir_i *maildir, imap_cmd_t *cmd){
    derr_t e = E_OK;

    dn_t *dn = CONTAINER_OF(maildir, dn_t, maildir);

    const imap_cmd_arg_t *arg = &cmd->arg;
    (void)dn;
    (void)arg;

    switch(cmd->type){
        case IMAP_CMD_CAPA:
        case IMAP_CMD_NOOP:
        case IMAP_CMD_LOGOUT:
        case IMAP_CMD_STARTTLS:
        case IMAP_CMD_AUTH:
        case IMAP_CMD_LOGIN:
        case IMAP_CMD_SELECT:
        case IMAP_CMD_EXAMINE:
        case IMAP_CMD_CREATE:
        case IMAP_CMD_DELETE:
        case IMAP_CMD_RENAME:
        case IMAP_CMD_SUB:
        case IMAP_CMD_UNSUB:
        case IMAP_CMD_LIST:
        case IMAP_CMD_LSUB:
        case IMAP_CMD_STATUS:
        case IMAP_CMD_APPEND:
        case IMAP_CMD_CHECK:
        case IMAP_CMD_CLOSE:
        case IMAP_CMD_EXPUNGE:
        case IMAP_CMD_SEARCH:
        case IMAP_CMD_FETCH:
        case IMAP_CMD_STORE:
        case IMAP_CMD_COPY:
        case IMAP_CMD_ENABLE:
            ORIG_GO(&e, E_INTERNAL, "unhandled command", cu_cmd);
    }

cu_cmd:
    imap_cmd_free(cmd);

    return e;
}
