#include "sm_serve.h"
#include "logger.h"
#include "imap_util.h"

static void server_imap_ev_returner(event_t *ev){
    imap_event_t *imap_ev = CONTAINER_OF(ev, imap_event_t, ev);
    imap_resp_free(imap_ev->arg.resp);
    free(imap_ev);
}

static derr_t imap_event_new(imap_event_t **out, server_t *server,
        imap_resp_t *resp){
    derr_t e = E_OK;
    *out = NULL;

    imap_event_t *imap_ev = malloc(sizeof(*imap_ev));
    if(!imap_ev) ORIG(&e, E_NOMEM, "nomem");
    *imap_ev = (imap_event_t){
        .type = IMAP_EVENT_TYPE_RESP,
        .arg = { .resp = resp },
    };

    /* we don't keep the server alive to free in-flight event_t's, so don't set
       the returner_arg to point to the server (which may get freed) */
    event_prep(&imap_ev->ev, server_imap_ev_returner, NULL);
    imap_ev->ev.session = &server->dn.s.session;
    imap_ev->ev.ev_type = EV_WRITE;

    *out = imap_ev;
    return e;
}

static void send_resp(derr_t *e, server_t *server, imap_resp_t *resp){
    if(is_error(*e)) goto fail;

    // create a response event
    imap_event_t *imap_ev;
    PROP_GO(e, imap_event_new(&imap_ev, server, resp), fail);

    // send the response to the imap session
    imap_session_send_event(&server->dn.s, &imap_ev->ev);

    return;

fail:
    imap_resp_free(resp);
}


static ie_dstr_t *build_capas(derr_t *e){
   if(is_error(*e)) goto fail;

   ie_dstr_t *capas = ie_dstr_new(e, &DSTR_LIT("IMAP4rev1"), KEEP_RAW);

   return capas;

fail:
    return NULL;
}


static derr_t send_greeting(server_t *server){
    derr_t e = E_OK;

    // build code
    ie_dstr_t *capas = build_capas(&e);
    ie_st_code_arg_t code_arg = {.capa = capas};
    ie_st_code_t *st_code = ie_st_code_new(&e, IE_ST_CODE_CAPA, code_arg);

    // build text
    ie_dstr_t *text = ie_dstr_new(&e, &DSTR_LIT("greetings, friend!"),
            KEEP_RAW);

    // build response
    ie_st_resp_t *st_resp = ie_st_resp_new(&e, NULL, IE_ST_OK, st_code, text);
    imap_resp_arg_t arg = {.status_type=st_resp};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);

    send_resp(&e, server, resp);

    return e;
}

bool server_more_work(server_t *server){
    return !server->greeting_sent
        || !link_list_isempty(&server->ts.unhandled_cmds)
        || !link_list_isempty(&server->ts.maildir_resps);
}

// we either need to consume the command or free it
static derr_t handle_one_command(server_t *server, imap_cmd_t *cmd){
    derr_t e = E_OK;

    const imap_cmd_arg_t *arg = &cmd->arg;
    (void)server;
    (void)arg;

    switch(cmd->type){
        case IMAP_CMD_CAPA:
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
            ORIG_GO(&e, E_INTERNAL, "Unhandled command", cu_cmd);
    }

cu_cmd:
    imap_cmd_free(cmd);
    return e;
}

// we either need to consume the resp or free it
static derr_t handle_one_maildir_resp(server_t *server, imap_resp_t *resp){
    derr_t e = E_OK;

    // detect if we are receiving commands from a maildir we closed
    // TODO: if we close one maildir and open another, how do we know where the
    // stream of one maildir ends and the other stream begins?
    if(!server->maildir){
        imap_resp_free(resp);
        return e;
    }

    // for now, just submit all maildir responses blindly
    imap_event_t *imap_ev;
    PROP_GO(&e, imap_event_new(&imap_ev, server, resp), fail);
    imap_session_send_event(&server->dn.s, &imap_ev->ev);

    return e;

fail:
    imap_resp_free(resp);
    return e;
}

derr_t server_do_work(server_t *server){
    derr_t e = E_OK;

    // send greeting
    if(!server->greeting_sent){
        PROP(&e, send_greeting(server));
        server->greeting_sent = true;
    }

    // unhandled commands
    while(!server->ts.closed){
        // pop a command
        uv_mutex_lock(&server->ts.mutex);
        link_t *link = link_list_pop_first(&server->ts.unhandled_cmds);
        uv_mutex_unlock(&server->ts.mutex);

        if(!link) break;

        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);

        // detect if we need to just pass the command to the maildir
        if(server->maildir){
            PROP(&e, server->maildir->cmd(server->maildir, cmd) );
            continue;
        }

        PROP(&e, handle_one_command(server, cmd) );
    }

    // responses from the maildir
    while(!server->ts.closed){
        // pop a response
        uv_mutex_lock(&server->ts.mutex);
        link_t *link = link_list_pop_first(&server->ts.maildir_resps);
        uv_mutex_unlock(&server->ts.mutex);

        if(!link) break;

        imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);

        PROP(&e, handle_one_maildir_resp(server, resp) );
    }

    return e;
};
