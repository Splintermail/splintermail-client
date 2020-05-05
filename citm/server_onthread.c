#include "citm.h"

// forward declarations
static derr_t do_select(server_t *server, imap_cmd_t *select_cmd);
static derr_t send_ok(server_t *server, const ie_dstr_t *tag,
        const dstr_t *msg);

static inline imap_cmd_t *steal_cmd(imap_cmd_t **cmd){
    imap_cmd_t *temp = *cmd;
    *cmd = NULL;
    return temp;
}

static inline ie_dstr_t *steal_tag(ie_dstr_t **tag){
    ie_dstr_t *temp = *tag;
    *tag = NULL;
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

// a pause for when you want to select but another folder is still open
typedef struct {
    imap_cmd_t *select_cmd;
    server_t *server;
    pause_t pause;
} select_pause_t;
DEF_CONTAINER_OF(select_pause_t, pause, pause_t);

static void select_pause_free(select_pause_t *select_pause){
    imap_cmd_free(select_pause->select_cmd);
    free(select_pause);
}

static bool select_pause_ready(pause_t *pause){
    select_pause_t *select_pause = CONTAINER_OF(pause, select_pause_t, pause);
    return select_pause->server->maildir_has_ref == false;
}

static derr_t select_pause_run(pause_t **pause){
    derr_t e = E_OK;

    select_pause_t *select_pause = CONTAINER_OF(*pause, select_pause_t, pause);

    PROP_GO(&e, do_select(select_pause->server, select_pause->select_cmd), cu);

cu:
    select_pause_free(select_pause);
    *pause = NULL;
    return e;
}

static void select_pause_cancel(pause_t **pause){
    select_pause_t *select_pause = CONTAINER_OF(*pause, select_pause_t, pause);
    select_pause_free(select_pause);
    *pause = NULL;
}

static derr_t select_pause_new(pause_t **out, server_t *server,
        imap_cmd_t *select_cmd){
    derr_t e = E_OK;

    *out = NULL;

    select_pause_t *select_pause = malloc(sizeof(*select_pause));
    if(!select_pause) ORIG(&e, E_NOMEM, "no mem");
    *select_pause = (select_pause_t){
        .server = server,
        .select_cmd = select_cmd,
        .pause = {
            .ready = select_pause_ready,
            .run = select_pause_run,
            .cancel = select_pause_cancel,
        }
    };

    *out = &select_pause->pause;

    return e;
}

// a pause for when you close a maildir_dn and have to wait for it to shut down
typedef struct {
    ie_dstr_t *tag;
    server_t *server;
    derr_t (*after)(server_t*, ie_dstr_t* tag);
    pause_t pause;
} close_pause_t;
DEF_CONTAINER_OF(close_pause_t, pause, pause_t);

static void close_pause_free(close_pause_t *close_pause){
    ie_dstr_free(close_pause->tag);
    free(close_pause);
}

static bool close_pause_ready(pause_t *pause){
    close_pause_t *close_pause = CONTAINER_OF(pause, close_pause_t, pause);
    return close_pause->server->maildir_has_ref == false;
}

static derr_t close_pause_run(pause_t **pause){
    derr_t e = E_OK;

    close_pause_t *close_pause = CONTAINER_OF(*pause, close_pause_t, pause);
    server_t *server = close_pause->server;

    if(close_pause->after){
        ie_dstr_t *tag = close_pause->tag;
        close_pause->tag = NULL;
        PROP_GO(&e, close_pause->after(server, tag), cu);
    }

cu:
    close_pause_free(close_pause);
    *pause = NULL;
    return e;
}

static void close_pause_cancel(pause_t **pause){
    close_pause_t *close_pause = CONTAINER_OF(*pause, close_pause_t, pause);
    close_pause_free(close_pause);
    *pause = NULL;
}

static derr_t close_pause_new(pause_t **out, server_t *server,
        const ie_dstr_t *tag, derr_t (*after)(server_t*, ie_dstr_t*)){
    derr_t e = E_OK;

    *out = NULL;

    close_pause_t *close_pause = malloc(sizeof(*close_pause));
    if(!close_pause) ORIG(&e, E_NOMEM, "no mem");
    *close_pause = (close_pause_t){
        .server = server,
        .after = after,
        .pause = {
            .ready = close_pause_ready,
            .run = close_pause_run,
            .cancel = close_pause_cancel,
        }
    };

    close_pause->tag = ie_dstr_copy(&e, tag);
    CHECK_GO(&e, fail);

    *out = &close_pause->pause;

    return e;

fail:
    close_pause_free(close_pause);
    return e;
}


static void server_imap_ev_returner(event_t *ev){
    imap_session_t *s = CONTAINER_OF(ev->session, imap_session_t, session);
    imap_session_ref_down(s);

    imap_event_t *imap_ev = CONTAINER_OF(ev, imap_event_t, ev);
    imap_resp_free(imap_ev->arg.resp);
    free(imap_ev);
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

    /* we don't keep the server alive to free in-flight event_t's, so don't set
       the returner_arg to point to the server (which may get freed) */
    event_returner_t returner =
        final ? final_event_returner : server_imap_ev_returner;
    event_prep(&imap_ev->ev, returner, NULL);
    imap_ev->ev.session = &server->s.session;
    imap_ev->ev.ev_type = EV_WRITE;
    imap_session_ref_up(&server->s);

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

    if(dstr_cmp(&login->user->dstr, &DSTR_LIT("a")) != 0){
        PROP(&e, send_no(server, tag, &DSTR_LIT("wrong user")) );
        return e;
    }

    // TODO: this is like 12 different levels of wrong
    if(dstr_cmp(&login->pass->dstr, &DSTR_LIT("b")) != 0){
        PROP(&e, send_no(server, tag, &DSTR_LIT("wrong pass")) );
        return e;
    }

    server->imap_state = AUTHENTICATED;

    PROP(&e, send_ok(server, tag, &DSTR_LIT("logged in")) );

    return e;
}

static bool mbx_name_is_sanitary(const dstr_t *name){
    if(name->len == 0){
        return true;
    }

    // all names should appear as relative paths
    if(name->data[0] == '/'){
        return false;
    }

    // no ".." ever
    if(dstr_count(name, &DSTR_LIT("..")) > 0){
        return false;
    }

    // no null bytes ever
    if(dstr_count(name, &DSTR_LIT("\0")) > 0){
        return false;
    }

    return true;
}

static dstr_t read_nonwild(dstr_t pattern, dstr_t *nonwild){
    size_t i = 0;
    for(i = 0; i < pattern.len; i++){
        if(pattern.data[i] == '*' || pattern.data[i] == '%'){
            break;
        }
    }
    if(i == 0){
        *nonwild = (dstr_t){0};
        return pattern;
    }
    *nonwild = dstr_sub(&pattern, 0, i);
    return dstr_sub(&pattern, i, pattern.len);
}

// read_wild is only safe to call if you know a wildcard is coming
static dstr_t read_wild(dstr_t pattern, bool *asterisk){
    *asterisk = false;
    size_t i;
    for(i = 0; i < pattern.len; i++){
        if(pattern.data[i] == '*') *asterisk = true;
        if(pattern.data[i] != '*' && pattern.data[i] != '%'){
            break;
        }
    }
    return dstr_sub(&pattern, i, pattern.len);
}

static bool match_nonwild(dstr_t name, dstr_t nonwild, dstr_t pattern){
    // make sure there's enough name for the nonwild
    if(name.len < nonwild.len){
        return false;
    }

    // make sure the non-wild matches
    if(nonwild.len > 0){
        dstr_t should_match = dstr_sub(&name, 0, nonwild.len);
        if(dstr_cmp(&nonwild, &should_match) != 0){
            return false;
        }
        // consume the matched part of the name
        name = dstr_sub(&name, nonwild.len, name.len);
    }

    // was that all there was?
    if(pattern.len == 0){
        return name.len == 0;
    }

    // get the wild-card part of the pattern
    bool asterisk;
    pattern = read_wild(pattern, &asterisk);
    // then get the nonwild that follows it
    pattern = read_nonwild(pattern, &nonwild);

    // consume the name byte-by-byte looking for a match to the next nonwild
    while(name.len > 0){
        // can we consume the first (nonmatching) character in the name?
        if(!asterisk && name.data[0] == '/'){
            return false;
        }
        // consume a character
        name = dstr_sub(&name, 1, name.len);
        // try to recurse
        if(match_nonwild(name, nonwild, pattern)) return true;
    }
    return false;
}

static bool mbx_name_matches(dstr_t name, dstr_t pattern){
    // start with the initial non-wild
    dstr_t nonwild;
    pattern = read_nonwild(pattern, &nonwild);
    return match_nonwild(name, nonwild, pattern);
}

typedef struct {
    // accumulated LIST response
    jsw_atree_t folders;
    dstr_t pattern;
} lister_t;

// handle_list_mbx is a for_each_mbx_hook_t
static derr_t handle_list_mbx(const dstr_t *name, bool has_ctn,
        bool has_children, void *data){
    derr_t e = E_OK;

    lister_t *lister = data;

    // skip non-matching responses
    if(!mbx_name_matches(*name, lister->pattern)){
        return e;
    }

    // build a LIST response for this mailbox
    ie_mailbox_t *m = ie_mailbox_new_maybeinbox(&e, name);

    ie_mflags_t *mf = ie_mflags_new(&e);
    if(!has_ctn){
        mf = ie_mflags_set_selectable(&e, mf, IE_SELECTABLE_NOSELECT);
    }
    if(has_children){
        mf = ie_mflags_add_noinf(&e, mf);
    }

    ie_list_resp_t *list = ie_list_resp_new(&e, mf, '/', m);
    CHECK(&e);

    // append this response to the atree
    jsw_ainsert(&lister->folders, &list->node);

    return e;
}

static derr_t list_cmd(server_t *server, const ie_dstr_t *tag,
        const ie_list_cmd_t *list_cmd){
    derr_t e = E_OK;

    const dstr_t *ref_name = ie_mailbox_name(list_cmd->m);

    // verify reference name is sanitary
    if(!mbx_name_is_sanitary(ref_name)){
        PROP(&e, send_no(server, tag, &DSTR_LIT("bad reference name")) );
        return e;
    }

    lister_t lister = {
        .pattern = list_cmd->pattern->dstr,
    };

    jsw_ainit(&lister.folders, ie_list_resp_cmp, ie_list_resp_get);

    // build a list of LIST responses, one for each folder
    PROP_GO(&e, dirmgr_do_for_each_mbx(server->dirmgr, ref_name,
                handle_list_mbx, &lister), fail);

    // send the LIST responses in sorted order
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, &lister.folders);
    while(node){
        // get the response from this node
        ie_list_resp_t *list_resp = CONTAINER_OF(node, ie_list_resp_t, node);

        // pop this node now, since send_resp will free this response on errors
        node = jsw_pop_atnext(&trav);

        imap_resp_arg_t arg = {.list = list_resp};
        imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_LIST, arg);
        send_resp(&e, server, resp);
    }
    CHECK(&e);

    PROP(&e, send_ok(server, tag, &DSTR_LIT("now you have all the things")) );

    return e;

fail:
    // free all the LIST responses
    while((node = jsw_apop(&lister.folders))){
        ie_list_resp_t *list_resp = CONTAINER_OF(node, ie_list_resp_t, node);
        ie_list_resp_free(list_resp);

    }
    return e;
}

// we either need to consume *select_cmd or free it
static derr_t do_select(server_t *server, imap_cmd_t *select_cmd){
    derr_t e = E_OK;

    const dstr_t *dir_name = ie_mailbox_name(select_cmd->arg.select->m);

    server->maildir_has_ref = true;
    actor_ref_up(&server->actor);
    PROP_GO(&e, dirmgr_open_dn(server->dirmgr, dir_name, &server->conn_dn,
                &server->maildir_dn), fail_maildir);

    // pass this SELECT command to the maildir_dn
    PROP_GO(&e, server->maildir_dn->cmd(server->maildir_dn,
                select_cmd), fail_maildir);

    server->imap_state = SELECTED;

    return e;

fail_maildir:
    dirmgr_close_dn(server->dirmgr, server->maildir_dn);
    server->maildir_has_ref = false;
    actor_ref_dn(&server->actor);

    imap_cmd_free(select_cmd);
    return e;
}

// this runs after the maildir_dn has finished closing
// (this is a close_pause_t->after() callback)
static derr_t do_close(server_t *server, ie_dstr_t *tag){
    derr_t e = E_OK;

    // build text
    DSTR_STATIC(msg, "get offa my lawn!");
    ie_dstr_t *text = ie_dstr_new(&e, &msg, KEEP_RAW);

    // build response
    ie_st_resp_t *st_resp = ie_st_resp_new(&e, tag, IE_ST_OK, NULL, text);
    imap_resp_arg_t arg = {.status_type=st_resp};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);

    send_resp(&e, server, resp);
    CHECK(&e);

    server->imap_state = AUTHENTICATED;

    return e;
}

// this may or may not have to wait for the3 maildir_dn to close before it runs
// (this is a close_pause_t->after() callback)
static derr_t do_logout(server_t *server, ie_dstr_t *tag){
    derr_t e = E_OK;

    PROP_GO(&e, send_bye(server, &DSTR_LIT("goodbye, my love...")), cu_tag);

    // send a message which will close the session upon WRITE_DONE
    DSTR_STATIC(final_msg, "I'm gonna be strong, I can make it through this");
    PROP_GO(&e, send_st_resp(server, tag, &final_msg, IE_ST_OK, true), cu_tag);

cu_tag:
    ie_dstr_free(tag);
    return e;
}

bool server_more_work(actor_t *actor){
    server_t *server = CONTAINER_OF(actor, server_t, actor);
    return !server->greeting_sent
        || !link_list_isempty(&server->ts.unhandled_cmds)
        || !link_list_isempty(&server->ts.maildir_resps)
        || (server->pause && server->pause->ready(server->pause))
        || (server->maildir_dn
                && server->maildir_dn->more_work(server->maildir_dn));
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
                PROP_GO(&e, close_pause_new(&server->pause, server, tag,
                            do_logout), cu_cmd);
            }else{
                PROP_GO(&e, do_logout(server, steal_tag(&cmd->tag)), cu_cmd);
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

        case IMAP_CMD_LIST:
            PROP_GO(&e, assert_state(server, AUTHENTICATED, tag, &state_ok),
                    cu_cmd);
            if(state_ok){
                PROP(&e, list_cmd(server, tag, arg->list) );
            }
            break;

        case IMAP_CMD_SELECT:
            if(server->imap_state == AUTHENTICATED){
                PROP_GO(&e, do_select(server, steal_cmd(&cmd)), cu_cmd);
            }else if(server->imap_state == SELECTED){
                // close the maildir_dn
                server_close_maildir_onthread(server);
                // SELECT the new one after this one closes
                PROP_GO(&e, select_pause_new(&server->pause, server,
                            steal_cmd(&cmd)), cu_cmd);
            }else{
                PROP_GO(&e, send_invalid_state_resp(server, tag), cu_cmd);
            }
            break;

        case IMAP_CMD_CLOSE:
            PROP_GO(&e, assert_state(server, SELECTED, tag, &state_ok),
                    cu_cmd);
            if(state_ok){
                // close the maildir_dn
                server_close_maildir_onthread(server);
                // call do_close() after the maildir_dn finishes closing
                PROP_GO(&e, close_pause_new(&server->pause, server, tag,
                            do_close), cu_cmd);
            }
            break;

        case IMAP_CMD_EXAMINE:
        case IMAP_CMD_STATUS:
        case IMAP_CMD_APPEND:
        case IMAP_CMD_CHECK:
        case IMAP_CMD_EXPUNGE:
        case IMAP_CMD_SEARCH:
        case IMAP_CMD_FETCH:
        case IMAP_CMD_STORE:
        case IMAP_CMD_COPY:
        // there's no intention of supporting these in sm_serve
        case IMAP_CMD_CREATE:
        case IMAP_CMD_DELETE:
        case IMAP_CMD_RENAME:
        case IMAP_CMD_SUB:
        case IMAP_CMD_UNSUB:
        case IMAP_CMD_LSUB:
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

derr_t server_do_work(actor_t *actor){
    derr_t e = E_OK;

    server_t *server = CONTAINER_OF(actor, server_t, actor);

    // send greeting
    if(!server->greeting_sent){
        PROP(&e, send_greeting(server));
        server->greeting_sent = true;
    }

    // if the maildir_dn needs on-thread work... do it.
    while(server->maildir_dn
            && server->maildir_dn->more_work(server->maildir_dn)){
        PROP(&e, server->maildir_dn->do_work(server->maildir_dn) );
    }

    // unhandled client commands from the client
    while(!server->ts.closed && !server->await_tag && !server->pause){
        // pop a command
        uv_mutex_lock(&server->ts.mutex);
        link_t *link = link_list_pop_first(&server->ts.unhandled_cmds);
        uv_mutex_unlock(&server->ts.mutex);

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
    }

    // responses from the maildir_dn
    while(!server->ts.closed){
        // pop a response
        uv_mutex_lock(&server->ts.mutex);
        link_t *link = link_list_pop_first(&server->ts.maildir_resps);
        uv_mutex_unlock(&server->ts.mutex);

        if(!link) break;

        imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);

        PROP(&e, handle_one_maildir_resp(server, resp) );
    }

    // handle delayed actions
    if(!server->ts.closed
            && server->pause && server->pause->ready(server->pause)){
        PROP(&e, server->pause->run(&server->pause) );
    }

    return e;
};
