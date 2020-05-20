#include "citm.h"

// forward declarations
static void send_resp(derr_t *e, server_t *server, imap_resp_t *resp);
static derr_t send_ok(server_t *server, const ie_dstr_t *tag,
        const dstr_t *msg);
static derr_t send_no(server_t *server, const ie_dstr_t *tag,
        const dstr_t *msg);
static derr_t send_greeting(server_t *server);
static derr_t do_logout(server_t *server, const ie_dstr_t *tag);
static derr_t do_close(server_t *server, const ie_dstr_t *tag);
static derr_t do_select(server_t *server, imap_cmd_t *select_cmd);

static inline imap_cmd_t *steal_cmd(imap_cmd_t **cmd){
    imap_cmd_t *temp = *cmd;
    *cmd = NULL;
    return temp;
}

static inline ie_dstr_t *steal_dstr(ie_dstr_t **tag){
    ie_dstr_t *temp = *tag;
    *tag = NULL;
    return temp;
}

static inline ie_st_code_t *steal_st_code(ie_st_code_t **code){
    ie_st_code_t *temp = *code;
    *code = NULL;
    return temp;
}

static inline ie_st_resp_t *steal_st_resp(ie_st_resp_t **resp){
    ie_st_resp_t *temp = *resp;
    *resp = NULL;
    return temp;
}

DSTR_STATIC(PREAUTH_dstr, "PREAUTH");
DSTR_STATIC(AUTHENTICATED_dstr, "AUTHENTICATED");
DSTR_STATIC(SELECTED_dstr, "SELECTED");
DSTR_STATIC(UNKNOWN_dstr, "unknown");

const dstr_t *imap_server_state_to_dstr(imap_server_state_t state){
    switch(state){
        case PREAUTH: return &PREAUTH_dstr;
        case AUTHENTICATED: return &AUTHENTICATED_dstr;
        case SELECTED: return &SELECTED_dstr;
    }
    return &UNKNOWN_dstr;
}

// a reusable paused function for server->paused
static bool paused_while_maildir_has_ref(server_t *server){
    return server->maildir_has_ref;
}

// a reusable after_paused function that handles the server->pause_tag
static derr_t after_tagged_pause(server_t *server){
    derr_t e = E_OK;

    PROP_GO(&e, server->after_tagged_pause(server, server->pause_tag), cu);

cu:
    ie_dstr_free(server->pause_tag);
    server->pause_tag = NULL;
    return e;
}

/* a pause for when you have started up but you don't want to send a greeting
   until you connect to the remote server */
static bool greet_paused(server_t *server){
    return !server->greeting_allowed;
}

derr_t start_greet_pause(server_t *server){
    server->paused = greet_paused;
    server->after_pause = send_greeting;
    return E_OK;
}

/* a pause for when you want to select but you need a response from above and
   possibly for another folder to be close */
static bool select_paused(server_t *server){
    return server->maildir_has_ref || server->select_state == SELECT_PENDING;
}

static derr_t after_select_pause(server_t *server){
    derr_t e = E_OK;

    // steal some things that we will consume or free
    imap_cmd_t *cmd = steal_cmd(&server->pause_cmd);
    ie_st_resp_t *ext_resp = steal_st_resp(&server->select_st_resp);

    if(server->select_state == SELECT_SUCCEEDED){
        // proceed normally
        PROP_GO(&e, do_select(server, steal_cmd(&cmd)), cu);
    }else{
        // relay the status-type response we got from above
        ie_dstr_t *tag = steal_dstr(&cmd->tag);
        ie_status_t status = ext_resp->status;
        ie_st_code_t *code = steal_st_code(&ext_resp->code);
        ie_dstr_t *text = steal_dstr(&ext_resp->text);

        ie_st_resp_t *st_resp = ie_st_resp_new(&e, tag, status, code, text);
        imap_resp_arg_t arg = {.status_type=st_resp};
        imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);

        send_resp(&e, server, resp);
        CHECK_GO(&e, cu);
    }

cu:
    imap_cmd_free(cmd);
    ie_st_resp_free(ext_resp);

    return e;
}

static derr_t start_select_pause(server_t *server, imap_cmd_t **pause_cmd){
    // we need the whole command, not just the tag
    server->pause_cmd = steal_cmd(pause_cmd);

    server->select_state = SELECT_PENDING;
    server->paused = select_paused;
    server->after_pause = after_select_pause;

    return E_OK;
}

// a pause for when you have to close a maildir_dn before logging out
static derr_t start_logout_pause(server_t *server, const ie_dstr_t *tag){
    derr_t e = E_OK;

    server->pause_tag = ie_dstr_copy(&e, tag);
    CHECK(&e);

    server->paused = paused_while_maildir_has_ref;
    server->after_pause = after_tagged_pause;
    server->after_tagged_pause = do_logout;

    return e;
}

// a pause for when you have to close a maildir_dn before responding to CLOSE
static derr_t start_close_pause(server_t *server, const ie_dstr_t *tag){
    derr_t e = E_OK;

    server->pause_tag = ie_dstr_copy(&e, tag);
    CHECK(&e);

    server->paused = paused_while_maildir_has_ref;
    server->after_pause = after_tagged_pause;
    server->after_tagged_pause = do_close;

    return e;
}

// a pause while you wait to login via a different connection
static bool login_paused(server_t *server){
    return server->login_state == LOGIN_PENDING;
}

static derr_t after_login_pause(server_t *server){
    derr_t e = E_OK;

    const ie_dstr_t *tag = server->pause_tag;
    if(server->login_state == LOGIN_SUCCEEDED){
        server->imap_state = AUTHENTICATED;
        PROP_GO(&e, send_ok(server, tag, &DSTR_LIT("logged in")), cu);
    }else{
        PROP_GO(&e, send_no(server, tag, &DSTR_LIT("dice, try again")), cu);
    }

cu:
    ie_dstr_free(server->pause_tag);
    server->pause_tag = NULL;
    return e;
}

static derr_t start_login_pause(server_t *server, const ie_dstr_t *tag){
    derr_t e = E_OK;

    server->pause_tag = ie_dstr_copy(&e, tag);
    CHECK(&e);

    server->paused = login_paused;
    server->after_pause = after_login_pause;
    server->login_state = LOGIN_PENDING;

    return e;
}

// a pause for when you are waiting for a passthru command to return something
static bool passthru_paused(server_t *server){
    return !server->passthru_resp;
}

static derr_t after_passthru_pause(server_t *server){
    derr_t e = E_OK;

    passthru_resp_t *passthru_resp = server->passthru_resp;
    server->passthru_resp = NULL;
    PROP(&e, server->after_passthru_pause(server, passthru_resp) );

    return e;
}

static derr_t start_passthru_pause(server_t *server,
        derr_t (*after)(server_t*, passthru_resp_t*)){
    derr_t e = E_OK;

    server->after_passthru_pause = after;

    server->paused = passthru_paused;
    server->after_pause = after_passthru_pause;

    return e;
}

//

static void server_imap_ev_returner(event_t *ev){
    server_t *server = ev->returner_arg;

    imap_event_t *imap_ev = CONTAINER_OF(ev, imap_event_t, ev);
    imap_resp_free(imap_ev->arg.resp);
    free(imap_ev);

    // one less unreturned event
    ref_dn(&server->refs);
}

// the last message we send gets this returner
static void final_event_returner(event_t *ev){
    imap_session_close(ev->session, E_OK);
    // call the main returner, which frees the event
    server_imap_ev_returner(ev);
}

static derr_t imap_event_new_ex(imap_event_t **out, server_t *server,
        imap_resp_t *resp, bool final){
    derr_t e = E_OK;
    *out = NULL;

    imap_event_t *imap_ev = malloc(sizeof(*imap_ev));
    if(!imap_ev) ORIG(&e, E_NOMEM, "nomem");
    *imap_ev = (imap_event_t){
        .type = IMAP_EVENT_TYPE_RESP,
        .arg = { .resp = resp },
    };

    event_returner_t returner =
        final ? final_event_returner : server_imap_ev_returner;
    event_prep(&imap_ev->ev, returner, server);
    imap_ev->ev.session = &server->s.session;
    imap_ev->ev.ev_type = EV_WRITE;

    // one more unreturned event
    ref_up(&server->refs);

    *out = imap_ev;
    return e;
}

static derr_t imap_event_new(imap_event_t **out, server_t *server,
        imap_resp_t *resp){
    derr_t e = E_OK;
    PROP(&e, imap_event_new_ex(out, server, resp, false) );
    return e;
}

static void send_resp_ex(derr_t *e, server_t *server, imap_resp_t *resp,
        bool final){
    if(is_error(*e)) goto fail;

    // TODO: support extensions better
    extensions_t exts = {0};
    resp = imap_resp_assert_writable(e, resp, &exts);
    CHECK_GO(e, fail);

    // create a response event
    imap_event_t *imap_ev;
    PROP_GO(e, imap_event_new_ex(&imap_ev, server, resp, final), fail);

    // send the response to the imap session
    imap_session_send_event(&server->s, &imap_ev->ev);

    return;

fail:
    imap_resp_free(resp);
}

static void send_resp(derr_t *e, server_t *server, imap_resp_t *resp){
    send_resp_ex(e, server, resp, false);
}

static derr_t send_st_resp(server_t *server, const ie_dstr_t *tag,
        const dstr_t *msg, ie_status_t status, bool final){
    derr_t e = E_OK;

    // copy tag
    ie_dstr_t *tag_copy = ie_dstr_copy(&e, tag);

    // build text
    ie_dstr_t *text = ie_dstr_new(&e, msg, KEEP_RAW);

    // build response
    ie_st_resp_t *st_resp = ie_st_resp_new(&e, tag_copy, status, NULL, text);
    imap_resp_arg_t arg = {.status_type=st_resp};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);

    send_resp_ex(&e, server, resp, final);
    CHECK(&e);

    return e;
}

static derr_t send_ok(server_t *server, const ie_dstr_t *tag,
        const dstr_t *msg){
    derr_t e = E_OK;
    PROP(&e, send_st_resp(server, tag, msg, IE_ST_OK, false) );
    return e;
}

static derr_t send_no(server_t *server, const ie_dstr_t *tag,
        const dstr_t *msg){
    derr_t e = E_OK;
    PROP(&e, send_st_resp(server, tag, msg, IE_ST_NO, false) );
    return e;
}

static derr_t send_bad(server_t *server, const ie_dstr_t *tag,
        const dstr_t *msg){
    derr_t e = E_OK;
    PROP(&e, send_st_resp(server, tag, msg, IE_ST_BAD, false) );
    return e;
}

static derr_t send_bye(server_t *server, const dstr_t *msg){
    derr_t e = E_OK;
    PROP(&e, send_st_resp(server, NULL, msg, IE_ST_BYE, false) );
    return e;
}

static derr_t send_invalid_state_resp(server_t *server, const ie_dstr_t *tag){
    derr_t e = E_OK;

    DSTR_VAR(msg, 128);
    PROP(&e, FMT(&msg, "command not allowed in %x state",
            FD(imap_server_state_to_dstr(server->imap_state))) );

    PROP(&e, send_bad(server, tag, &msg) );

    return e;
}

static derr_t assert_state(server_t *server, imap_server_state_t state,
        const ie_dstr_t *tag, bool *ok){
    derr_t e = E_OK;

    *ok = (server->imap_state == state);
    if(*ok) return e;

    PROP(&e, send_invalid_state_resp(server, tag) );

    return e;
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

static derr_t send_capas(server_t *server, const ie_dstr_t *tag){
    derr_t e = E_OK;

    // build CAPABILITY response
    ie_dstr_t *capas = build_capas(&e);
    imap_resp_arg_t arg = {.capa=capas};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_CAPA, arg);

    send_resp(&e, server, resp);

    CHECK(&e);

    PROP(&e, send_ok(server, tag,
                &DSTR_LIT("if you didn't know, now you know")) );

    return e;
}

static derr_t check_login(server_t *server, const ie_dstr_t *tag,
        const ie_login_cmd_t *login){
    derr_t e = E_OK;

    // prepare a login pause
    PROP(&e, start_login_pause(server, tag) );

    // report the login attempt to the sf_pair
    ie_login_cmd_t *login_cmd_copy = ie_login_cmd_copy(&e, login);
    CHECK(&e);

    server->cb->login(server->cb, login_cmd_copy);

    return e;
}


/* send_passthru_st_resp is the builder-api version of passthru_done that is
   easy to include in other server->after_passthru_pause() functions */
static void send_passthru_st_resp(derr_t *e, server_t *server,
        passthru_resp_t *passthru_resp){
    if(is_error(*e)) goto cu;

    // send the tagged status-type response with the correct tag
    ie_st_resp_t *st_resp = ie_st_resp_new(e,
        steal_dstr(&passthru_resp->tag),
        passthru_resp->st_resp->status,
        ie_st_code_copy(e, passthru_resp->st_resp->code),
        steal_dstr(&passthru_resp->st_resp->text)
    );

    imap_resp_arg_t arg = { .status_type = st_resp };
    imap_resp_t *resp = imap_resp_new(e, IMAP_RESP_STATUS_TYPE, arg);

    send_resp(e, server, resp);

    CHECK_GO(e, cu);

cu:
    passthru_resp_free(passthru_resp);
}


/* passthru_done is a generic server->after_passthru_pause() for things with no
   additional arguments */
static derr_t passthru_done(server_t *server, passthru_resp_t *passthru_resp){
    derr_t e = E_OK;

    send_passthru_st_resp(&e, server, passthru_resp);
    CHECK(&e);

    return e;
}

// list_done is a server->after_passthru_pause()
static derr_t list_done(server_t *server, passthru_resp_t *passthru_resp){
    derr_t e = E_OK;

    // send the LIST responses in sorted order
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, &passthru_resp->arg.list->tree);
    while(node){
        // get the response from this node
        ie_list_resp_t *list_resp = CONTAINER_OF(node, ie_list_resp_t, node);

        // pop this node now, since send_resp will free this response on errors
        node = jsw_pop_atnext(&trav);

        imap_resp_arg_t arg = {.list = list_resp};
        imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_LIST, arg);
        send_resp(&e, server, resp);
    }

    send_passthru_st_resp(&e, server, passthru_resp);
    CHECK(&e);

    return e;
}

// lsub_done is a server->after_passthru_pause()
static derr_t lsub_done(server_t *server, passthru_resp_t *passthru_resp){
    derr_t e = E_OK;

    // send the LSUB responses in sorted order
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, &passthru_resp->arg.lsub->tree);
    while(node){
        // get the response from this node
        ie_list_resp_t *lsub_resp = CONTAINER_OF(node, ie_list_resp_t, node);

        // pop this node now, since send_resp will free this response on errors
        node = jsw_pop_atnext(&trav);

        imap_resp_arg_t arg = {.lsub = lsub_resp};
        imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_LSUB, arg);
        send_resp(&e, server, resp);
    }

    send_passthru_st_resp(&e, server, passthru_resp);
    CHECK(&e);

    return e;
}

// status_done is a server->after_passthru_pause()
static derr_t status_done(server_t *server, passthru_resp_t *passthru_resp){
    derr_t e = E_OK;

    // send the STATUS response (there may not be one if the commmand failed)
    if(passthru_resp->arg.status){
        ie_status_resp_t *status = passthru_resp->arg.status;
        passthru_resp->arg.status = NULL;

        imap_resp_arg_t arg = { .status = status };
        imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS, arg);

        send_resp(&e, server, resp);
    }

    send_passthru_st_resp(&e, server, passthru_resp);
    CHECK(&e);

    return e;
}

static derr_t passthru_cmd(server_t *server, const ie_dstr_t *tag,
        const imap_cmd_t *cmd){
    derr_t e = E_OK;

    ie_dstr_t *tag_copy = ie_dstr_copy(&e, tag);
    passthru_type_e type;
    passthru_req_arg_u arg = {0};
    derr_t (*after_passthru_pause)(server_t*, passthru_resp_t*);
    switch(cmd->type){
        case IMAP_CMD_LIST:
            type = PASSTHRU_LIST;
            arg.list = ie_list_cmd_copy(&e, cmd->arg.list);
            after_passthru_pause = list_done;
            break;

        case IMAP_CMD_LSUB:
            type = PASSTHRU_LSUB;
            arg.lsub = ie_list_cmd_copy(&e, cmd->arg.lsub);
            after_passthru_pause = lsub_done;
            break;

        case IMAP_CMD_STATUS:
            type = PASSTHRU_STATUS;
            arg.status = ie_status_cmd_copy(&e, cmd->arg.status);
            after_passthru_pause = status_done;
            break;

        case IMAP_CMD_CREATE:
            type = PASSTHRU_CREATE;
            arg.create = ie_mailbox_copy(&e, cmd->arg.create);
            after_passthru_pause = passthru_done;
            break;

        case IMAP_CMD_DELETE:
            type = PASSTHRU_DELETE;
            arg.create = ie_mailbox_copy(&e, cmd->arg.delete);
            after_passthru_pause = passthru_done;
            break;

        case IMAP_CMD_SUB:
            type = PASSTHRU_SUB;
            arg.create = ie_mailbox_copy(&e, cmd->arg.sub);
            after_passthru_pause = passthru_done;
            break;

        case IMAP_CMD_UNSUB:
            type = PASSTHRU_UNSUB;
            arg.create = ie_mailbox_copy(&e, cmd->arg.unsub);
            after_passthru_pause = passthru_done;
            break;

        default:
            ORIG(&e, E_INTERNAL, "illegal command type in passthru_cmd");
    }
    passthru_req_t *passthru_req;
    passthru_req = passthru_req_new(&e, tag_copy, type, arg);
    CHECK(&e);

    server->cb->passthru_req(server->cb, passthru_req);

    // wait for a response
    PROP(&e, start_passthru_pause(server, after_passthru_pause) );


    return e;
}

// we either need to consume *select_cmd or free it
static derr_t do_select(server_t *server, imap_cmd_t *select_cmd){
    derr_t e = E_OK;

    const dstr_t *dir_name = ie_mailbox_name(select_cmd->arg.select->m);

    server->maildir_has_ref = true;

    PROP_GO(&e, dirmgr_open_dn(server->dirmgr, dir_name, &server->conn_dn,
                &server->maildir_dn), fail_ref);

    // ref_up for maildir
    ref_up(&server->refs);

    // pass this SELECT command to the maildir_dn
    PROP(&e, server->maildir_dn->cmd(server->maildir_dn,
                select_cmd) );

    server->imap_state = SELECTED;

    return e;

fail_ref:
    server->maildir_has_ref = false;

    imap_cmd_free(select_cmd);
    return e;
}

// this runs after the maildir_dn has finished closing
// (this is a close_pause_t->after() callback)
static derr_t do_close(server_t *server, const ie_dstr_t *tag){
    derr_t e = E_OK;

    // build text
    DSTR_STATIC(msg, "get offa my lawn!");
    ie_dstr_t *text = ie_dstr_new(&e, &msg, KEEP_RAW);

    // build response
    ie_dstr_t *tag_copy = ie_dstr_copy(&e, tag);
    ie_st_resp_t *st_resp = ie_st_resp_new(&e, tag_copy, IE_ST_OK, NULL, text);
    imap_resp_arg_t arg = {.status_type=st_resp};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);

    send_resp(&e, server, resp);
    CHECK(&e);

    server->imap_state = AUTHENTICATED;

    return e;
}

// this may or may not have to wait for the maildir_dn to close before it runs
static derr_t do_logout(server_t *server, const ie_dstr_t *tag){
    derr_t e = E_OK;

    PROP(&e, send_bye(server, &DSTR_LIT("goodbye, my love...")) );

    // send a message which will close the session upon WRITE_DONE
    DSTR_STATIC(final_msg, "I'm gonna be strong, I can make it through this");
    PROP(&e, send_st_resp(server, tag, &final_msg, IE_ST_OK, true) );

    return e;
}

// we either need to consume the command or free it
static derr_t handle_one_command(server_t *server, imap_cmd_t *cmd){
    derr_t e = E_OK;

    const imap_cmd_arg_t *arg = &cmd->arg;
    const ie_dstr_t *tag = cmd->tag;
    bool state_ok;

    switch(cmd->type){
        case IMAP_CMD_CAPA:
            PROP_GO(&e, send_capas(server, tag), cu_cmd);
            break;

        case IMAP_CMD_NOOP:
            PROP_GO(&e, send_ok(server, tag, &DSTR_LIT("done, son!")), cu_cmd);
            break;

        case IMAP_CMD_LOGOUT:
            if(server->imap_state == SELECTED){
                // close the maildir_dn
                server_close_maildir_onthread(server);
                // call do_logout() after the maildir_dn finishes closing
                PROP_GO(&e, start_logout_pause(server, tag), cu_cmd);
            }else{
                PROP_GO(&e, do_logout(server, cmd->tag), cu_cmd);
            }
            break;

        case IMAP_CMD_STARTTLS:
            PROP_GO(&e, send_bad(server, tag,
                &DSTR_LIT("STARTTLS not supported, connect with TLS instead")),
                cu_cmd);
            break;

        case IMAP_CMD_AUTH:
            PROP_GO(&e, send_bad(server, tag,
                &DSTR_LIT("AUTH not supported, use LOGIN instead")), cu_cmd);
            break;

        case IMAP_CMD_LOGIN:
            PROP_GO(&e, assert_state(server, PREAUTH, tag, &state_ok), cu_cmd);
            if(state_ok){
                PROP(&e, check_login(server, tag, arg->login) );
            }
            break;

        // passthru commands
        case IMAP_CMD_LIST:
        case IMAP_CMD_LSUB:
        case IMAP_CMD_STATUS:
        case IMAP_CMD_CREATE:
        case IMAP_CMD_DELETE:
        case IMAP_CMD_SUB:
        case IMAP_CMD_UNSUB:
            if(server->imap_state != AUTHENTICATED
                    && server->imap_state != SELECTED){
                PROP_GO(&e, send_invalid_state_resp(server, tag), cu_cmd);
                break;
            }
            PROP(&e, passthru_cmd(server, tag, cmd) );
            break;

        case IMAP_CMD_SELECT:
            if(server->imap_state != AUTHENTICATED
                    && server->imap_state != SELECTED){
                PROP_GO(&e, send_invalid_state_resp(server, tag), cu_cmd);
                break;
            }

            if(server->imap_state == SELECTED){
                // close the maildir_dn
                server_close_maildir_onthread(server);
            }

            /* Ask the sf_pair for permission to SELECT the folder.  Permission
               may not be grated if e.g. the fetcher finds out the folder does
               not exist or if it is the keybox folder */
            ie_mailbox_t *m_copy = ie_mailbox_copy(&e, arg->select->m);
            CHECK_GO(&e, cu_cmd);
            server->cb->select(server->cb, m_copy);

            /* now wait for both the response from above and for the maildir_dn
               to close */
            PROP_GO(&e, start_select_pause(server, &cmd), cu_cmd);
            break;

        case IMAP_CMD_CLOSE:
            PROP_GO(&e, assert_state(server, SELECTED, tag, &state_ok),
                    cu_cmd);
            if(state_ok){
                // close the maildir_dn
                server_close_maildir_onthread(server);
                // call do_close() after the maildir_dn finishes closing
                PROP_GO(&e, start_close_pause(server, tag), cu_cmd);
            }
            break;

        case IMAP_CMD_EXAMINE:
        case IMAP_CMD_APPEND:
        case IMAP_CMD_CHECK:
        case IMAP_CMD_EXPUNGE:
        case IMAP_CMD_SEARCH:
        case IMAP_CMD_FETCH:
        case IMAP_CMD_STORE:
        case IMAP_CMD_COPY:
        case IMAP_CMD_RENAME:
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

    // detect if we are receiving commands from a maildir_dn we closed
    if(!server->maildir_dn){
        imap_resp_free(resp);
        return e;
    }

    // detect if this is a tagged status_type response were waiting for
    if(server->await_tag && resp->type == IMAP_RESP_STATUS_TYPE
            && resp->arg.status_type->tag
            && dstr_cmp(&server->await_tag->dstr,
                        &resp->arg.status_type->tag->dstr) == 0){
        ie_dstr_free(server->await_tag);
        server->await_tag = 0;
    }

    // otherwise, just submit all maildir_dn responses blindly
    imap_event_t *imap_ev;
    PROP_GO(&e, imap_event_new(&imap_ev, server, resp), fail);
    imap_session_send_event(&server->s, &imap_ev->ev);

    return e;

fail:
    imap_resp_free(resp);
    return e;
}

static bool intercept_cmd_type(imap_cmd_type_t type){
    switch(type){
        /* (SELECT is special; it may trigger a dirmgr_close_dn, then it always
            triggers a dirmgr_open_dn in the sm_serve_logic, and then it is
            also passed into the maildir_dn as the first command, but not here)
            */
        case IMAP_CMD_SELECT:

        // CAPABILTIES can all be handled in one place
        case IMAP_CMD_CAPA:

        // also intercept close-like commands
        case IMAP_CMD_LOGOUT:
        case IMAP_CMD_CLOSE:
            return true;
        default:
            return false;
    }
}

// on failure, we must free the whole command
static derr_t server_await_if_async(server_t *server, imap_cmd_t *cmd){
    derr_t e = E_OK;

    switch(cmd->type){
        // right now, the only async commands are ones that edit the maildir_dn
        case IMAP_CMD_STORE:
            server->await_tag = ie_dstr_copy(&e, cmd->tag);
            CHECK_GO(&e, fail);
            break;

        default:
            break;
    }

    return e;

fail:
    imap_cmd_free(cmd);
    return e;
}

derr_t server_do_work(server_t *server, bool *noop){
    derr_t e = E_OK;

    *noop = true;

    // if the maildir_dn needs on-thread work... do it.
    while(server->maildir_dn
            && server->maildir_dn->more_work(server->maildir_dn)){
        PROP(&e, server->maildir_dn->do_work(server->maildir_dn) );
        *noop = false;
    }

    // unhandled client commands from the client
    while(!server->closed && !server->await_tag && !server->paused){
        // pop a command
        link_t *link = link_list_pop_first(&server->unhandled_cmds);
        if(!link) break;

        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);

        // detect if we need to just pass the command to the maildir_dn
        if(server->maildir_dn && !intercept_cmd_type(cmd->type)){
            // asynchronous commands must be awaited:
            PROP(&e, server_await_if_async(server, cmd) );

            PROP(&e, server->maildir_dn->cmd(server->maildir_dn, cmd) );
            continue;
        }

        PROP(&e, handle_one_command(server, cmd) );
        *noop = false;
    }

    // responses from the maildir_dn
    while(!server->closed){
        // pop a response
        link_t *link = link_list_pop_first(&server->maildir_resps);
        if(!link) break;

        imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);

        PROP(&e, handle_one_maildir_resp(server, resp) );
        *noop = false;
    }

    // handle delayed actions
    if(!server->closed && server->paused && !server->paused(server)){
        PROP(&e, server->after_pause(server) );
        server->paused = NULL;
        server->after_pause = NULL;
        *noop = false;
    }

    return e;
};
