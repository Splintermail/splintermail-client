#include "libdstr/libdstr.h"

#include "libimaildir.h"

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
        .selected = false,
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

//// TODO: unify these send_* functions with the ones in sm_serve_logic?

static derr_t send_st_resp(dn_t *dn, const ie_dstr_t *tag, const dstr_t *msg,
        ie_status_t status){
    derr_t e = E_OK;

    // copy tag
    ie_dstr_t *tag_copy = ie_dstr_copy(&e, tag);

    // build text
    ie_dstr_t *text = ie_dstr_new(&e, msg, KEEP_RAW);

    // build response
    ie_st_resp_t *st_resp = ie_st_resp_new(&e, tag_copy, status, NULL, text);
    imap_resp_arg_t arg = {.status_type=st_resp};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);
    CHECK(&e);

    dn->conn->resp(dn->conn, resp);

    return e;
}

static derr_t send_ok(dn_t *dn, const ie_dstr_t *tag,
        const dstr_t *msg){
    derr_t e = E_OK;
    PROP(&e, send_st_resp(dn, tag, msg, IE_ST_OK) );
    return e;
}

// static derr_t send_no(dn_t *dn, const ie_dstr_t *tag,
//         const dstr_t *msg){
//     derr_t e = E_OK;
//     PROP(&e, send_st_resp(dn, tag, msg, IE_ST_NO) );
//     return e;
// }
//
// static derr_t send_bad(dn_t *dn, const ie_dstr_t *tag,
//         const dstr_t *msg){
//     derr_t e = E_OK;
//     PROP(&e, send_st_resp(dn, tag, msg, IE_ST_BAD) );
//     return e;
// }

////

static derr_t send_flags_resp(dn_t *dn){
    derr_t e = E_OK;

    ie_flags_t *flags = ie_flags_new(&e);
    flags = ie_flags_add_simple(&e, flags, IE_FLAG_ANSWERED);
    flags = ie_flags_add_simple(&e, flags, IE_FLAG_FLAGGED);
    flags = ie_flags_add_simple(&e, flags, IE_FLAG_DELETED);
    flags = ie_flags_add_simple(&e, flags, IE_FLAG_SEEN);
    // TODO: is this one supposed to be used in general?
    // flags = ie_flags_add_simple(&e, flags, IE_FLAG_DRAFT);
    imap_resp_arg_t arg = {.flags=flags};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_FLAGS, arg);
    CHECK(&e);

    dn->conn->resp(dn->conn, resp);

    return e;
}

static derr_t send_exists_resp_unsafe(dn_t *dn){
    derr_t e = E_OK;

    if(dn->m->msgs.tree.size > UINT_MAX){
        ORIG(&e, E_VALUE, "too many messages for exists response");
    }
    unsigned int exists = (unsigned int)dn->m->msgs.tree.size;

    imap_resp_arg_t arg = {.exists=exists};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_EXISTS, arg);
    CHECK(&e);

    dn->conn->resp(dn->conn, resp);

    return e;
}

static derr_t send_recent_resp_unsafe(dn_t *dn){
    derr_t e = E_OK;

    // TODO: support \Recent flag

    unsigned int recent = 0;
    imap_resp_arg_t arg = {.recent=recent};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_RECENT, arg);
    CHECK(&e);

    dn->conn->resp(dn->conn, resp);

    return e;
}

static derr_t send_unseen_resp_unsafe(dn_t *dn){
    derr_t e = E_OK;

    // TODO: what technically is "unseen"?  And what if nothing is unseen?
    (void)dn;

    return e;
}

static derr_t send_pflags_resp(dn_t *dn){
    derr_t e = E_OK;

    ie_pflags_t *pflags = ie_pflags_new(&e);
    pflags = ie_pflags_add_simple(&e, pflags, IE_PFLAG_ANSWERED);
    pflags = ie_pflags_add_simple(&e, pflags, IE_PFLAG_FLAGGED);
    pflags = ie_pflags_add_simple(&e, pflags, IE_PFLAG_DELETED);
    pflags = ie_pflags_add_simple(&e, pflags, IE_PFLAG_SEEN);
    // TODO: is this one supposed to be used in general?
    // pflags = ie_pflags_add_simple(&e, pflags, IE_PFLAG_DRAFT);

    ie_st_code_arg_t code_arg = {.pflags=pflags};
    ie_st_code_t *code = ie_st_code_new(&e, IE_ST_CODE_PERMFLAGS, code_arg);

    DSTR_STATIC(msg, "here, have some permanentflags.");
    ie_dstr_t *text = ie_dstr_new(&e, &msg, KEEP_RAW);

    ie_st_resp_t *st_resp = ie_st_resp_new(&e, NULL, IE_ST_OK, code, text);
    imap_resp_arg_t arg = {.status_type=st_resp};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);
    CHECK(&e);

    dn->conn->resp(dn->conn, resp);

    return e;
}

static derr_t send_uidnext_resp_unsafe(dn_t *dn){
    derr_t e = E_OK;

    unsigned int max_uid = 0;

    // check the highest value in msgs tree
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atlast(&trav, &dn->m->msgs.tree);
    if(node){
        msg_base_t *base = CONTAINER_OF(node, msg_base_t, node);
        max_uid = MAX(max_uid, base->ref.uid);
    }

    // check the highest value in expunged tree
    node = jsw_atlast(&trav, &dn->m->expunged.tree);
    if(node){
        msg_expunge_t *expunge = CONTAINER_OF(node, msg_expunge_t, node);
        max_uid = MAX(max_uid, expunge->uid);
    }

    ie_st_code_arg_t code_arg = {.uidnext = max_uid + 1};
    ie_st_code_t *code = ie_st_code_new(&e, IE_ST_CODE_UIDNEXT, code_arg);

    DSTR_STATIC(msg, "get ready, it's coming");
    ie_dstr_t *text = ie_dstr_new(&e, &msg, KEEP_RAW);

    ie_st_resp_t *st_resp = ie_st_resp_new(&e, NULL, IE_ST_OK, code, text);
    imap_resp_arg_t arg = {.status_type=st_resp};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);
    CHECK(&e);

    dn->conn->resp(dn->conn, resp);

    return e;
}

static derr_t send_uidvld_resp(dn_t *dn){
    derr_t e = E_OK;

    maildir_log_i *log = dn->m->log.log;
    ie_st_code_arg_t code_arg = {.uidvld = log->get_uidvld(log)};
    ie_st_code_t *code = ie_st_code_new(&e, IE_ST_CODE_UIDVLD, code_arg);

    DSTR_STATIC(msg, "ride or die");
    ie_dstr_t *text = ie_dstr_new(&e, &msg, KEEP_RAW);

    ie_st_resp_t *st_resp = ie_st_resp_new(&e, NULL, IE_ST_OK, code, text);
    imap_resp_arg_t arg = {.status_type=st_resp};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);
    CHECK(&e);

    dn->conn->resp(dn->conn, resp);

    return e;
}

static derr_t select_cmd(dn_t *dn, const ie_dstr_t *tag,
        const ie_select_cmd_t *select){
    derr_t e = E_OK;

    // make sure the select did not include QRESYNC or CONDSTORE
    if(select->params){
        ORIG(&e, E_PARAM, "QRESYNC and CONDSTORE not supported");
    }

    // obtain lock
    uv_rwlock_rdlock(&dn->m->msgs.lock);
    uv_rwlock_rdlock(&dn->m->expunged.lock);
    uv_rwlock_rdlock(&dn->m->mods.lock);

    // generate/send required SELECT responses
    PROP(&e, send_flags_resp(dn) );
    PROP(&e, send_exists_resp_unsafe(dn) );
    PROP(&e, send_recent_resp_unsafe(dn) );
    PROP(&e, send_unseen_resp_unsafe(dn) );
    PROP(&e, send_pflags_resp(dn) );
    PROP(&e, send_uidnext_resp_unsafe(dn) );
    PROP(&e, send_uidvld_resp(dn) );

    // release lock
    uv_rwlock_rdunlock(&dn->m->mods.lock);
    uv_rwlock_rdunlock(&dn->m->expunged.lock);
    uv_rwlock_rdunlock(&dn->m->msgs.lock);


    /* TODO: check if we are in DITM mode or serve-locally mode.  For now we
             only support serve-locally mode */
    PROP(&e, send_ok(dn, tag, &DSTR_LIT("welcome in")) );

    return e;
}

// we either need to consume the cmd or free it
static derr_t conn_dn_cmd(maildir_i *maildir, imap_cmd_t *cmd){
    derr_t e = E_OK;

    dn_t *dn = CONTAINER_OF(maildir, dn_t, maildir);

    const ie_dstr_t *tag = cmd->tag;
    const imap_cmd_arg_t *arg = &cmd->arg;

    if(cmd->type == IMAP_CMD_SELECT && dn->selected){
        ORIG_GO(&e, E_INTERNAL, "SELECT sent to selected dn_t", cu_cmd);
    }
    if(cmd->type != IMAP_CMD_SELECT && !dn->selected){
        ORIG_GO(&e, E_INTERNAL, "non-SELECT sent to unselected dn_t", cu_cmd);
    }

    switch(cmd->type){
        case IMAP_CMD_SELECT:
            PROP_GO(&e, select_cmd(dn, tag, arg->select), cu_cmd);
            break;

        case IMAP_CMD_CAPA:
        case IMAP_CMD_NOOP:
        case IMAP_CMD_LOGOUT:
        case IMAP_CMD_STARTTLS:
        case IMAP_CMD_AUTH:
        case IMAP_CMD_LOGIN:
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
