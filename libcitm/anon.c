#include "libcitm/libcitm.h"

typedef struct {
    link_t link;  // citm->anons

    scheduler_i *scheduler;
    schedulable_t schedulable;

    imap_server_t *s;
    imap_client_t *c;

    imap_server_read_t sread;
    imap_client_read_t cread;
    imap_server_write_t swrite;
    imap_client_write_t cwrite;

    derr_t e;

    // things we just got done reading
    imap_cmd_t *cmd;
    imap_resp_t *resp;
    // things we need to write
    link_t cmds;
    link_t resps;

    anon_cb cb;
    void *cb_data;

    size_t ntags;

    // remember what we are relaying
    ie_dstr_t *tag;
    dstr_t user;
    dstr_t pass;

    // state flags

    bool writing_up : 1;
    bool writing_dn : 1;
    bool reading_up : 1;
    bool reading_dn : 1;

    bool login_cmd_recvd : 1;
    bool write_up_sent : 1;
    bool login_resp_recvd : 1;
    bool write_dn_sent : 1;
    bool login_success : 1;
    bool login_done : 1;

    bool broken_conn : 1;
    bool canceled : 1;
    bool logged_out : 1;
    bool done : 1;
} anon_t;
DEF_CONTAINER_OF(anon_t, link, link_t)
DEF_CONTAINER_OF(anon_t, schedulable, schedulable_t)
DEF_CONTAINER_OF(anon_t, sread, imap_server_read_t)
DEF_CONTAINER_OF(anon_t, cread, imap_client_read_t)
DEF_CONTAINER_OF(anon_t, swrite, imap_server_write_t)
DEF_CONTAINER_OF(anon_t, cwrite, imap_client_write_t)

static void advance_state(anon_t *anon);

static void scheduled(schedulable_t *s){
    anon_t *anon = CONTAINER_OF(s, anon_t, schedulable);
    advance_state(anon);
}

static void schedule(anon_t *anon){
    anon->scheduler->schedule(anon->scheduler, &anon->schedulable);
}

static void anon_free(anon_t *anon){
    schedulable_cancel(&anon->schedulable);
    link_remove(&anon->link);
    ie_dstr_free(anon->tag);
    // free any pending io
    imap_cmd_free(anon->cmd);
    imap_resp_free(anon->resp);
    link_t *link;
    while((link = link_list_pop_first(&anon->cmds))){
        imap_cmd_free(CONTAINER_OF(link, imap_cmd_t, link));
    }
    while((link = link_list_pop_first(&anon->resps))){
        imap_resp_free(CONTAINER_OF(link, imap_resp_t, link));
    }
    free(anon);
}

static void sawait_cb(
    imap_server_t *s, derr_t e, link_t *reads, link_t *writes
){
    // we only have statically defined reads/writes
    (void)reads;
    (void)writes;
    anon_t *anon = s->data;
    schedule(anon);
    // we never cancel the server, except when we are canceled
    if(anon->canceled){
        DROP_CANCELED_VAR(&e);
    }
    UPGRADE_CANCELED_VAR(&e, E_INTERNAL);
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&anon->e, &e);
}

static void cawait_cb(
    imap_client_t *c, derr_t e, link_t *reads, link_t *writes
){
    // we only have statically defined reads/writes
    (void)reads;
    (void)writes;
    anon_t *anon = c->data;
    schedule(anon);
    // we only cancel the client in the logout and cancel flow
    if(anon->logged_out || anon->canceled){
        DROP_CANCELED_VAR(&e);
    }
    if(is_error(e)){
        // upwards connection failure
        anon->broken_conn = true;
    }
    UPGRADE_CANCELED_VAR(&e, E_INTERNAL);
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&anon->e, &e);
}

static void sread_cb(
    imap_server_t *s, imap_server_read_t *req, imap_cmd_t *cmd
){
    (void)s;
    anon_t *anon = CONTAINER_OF(req, anon_t, sread);
    anon->reading_dn = false;
    anon->cmd = cmd;
    schedule(anon);
}

static void cread_cb(
    imap_client_t *c, imap_client_read_t *req, imap_resp_t *resp
){
    (void)c;
    anon_t *anon = CONTAINER_OF(req, anon_t, cread);
    anon->reading_up = false;
    anon->resp = resp;
    schedule(anon);
}

static void swrite_cb(imap_server_t *s, imap_server_write_t *req){
    (void)s;
    anon_t *anon = CONTAINER_OF(req, anon_t, swrite);
    anon->writing_dn = false;
    schedule(anon);
}

static void cwrite_cb(imap_client_t *c, imap_client_write_t *req){
    (void)c;
    anon_t *anon = CONTAINER_OF(req, anon_t, cwrite);
    anon->writing_up = false;
    schedule(anon);
}

#define ONCE(x) if(!x && (x = true))

// returns bool ok
static bool advance_writes_up(anon_t *anon){
    // have we finished the last write?
    if(anon->writing_up) return false;

    // should we start a new write?
    link_t *link;
    if((link = link_list_pop_first(&anon->cmds))){
        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
        imap_client_must_write(anon->c, &anon->cwrite, cmd, cwrite_cb);
        anon->writing_up = true;
        return false;
    }

    return true;
}

// returns bool ok
static bool advance_writes_dn(anon_t *anon){
    // have we finished the last write?
    if(anon->writing_dn) return false;

    // should we start a new write?
    link_t *link;
    if((link = link_list_pop_first(&anon->resps))){
        imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);
        imap_server_must_write(anon->s, &anon->swrite, resp, swrite_cb);
        anon->writing_dn = true;
        return false;
    }

    return true;
}

// returns bool ok
static bool advance_reads_dn(anon_t *anon){
    if(anon->cmd) return true;
    ONCE(anon->reading_dn){
        imap_server_must_read(anon->s, &anon->sread, sread_cb);
    }
    return false;
}

// returns bool ok
static bool advance_reads_up(anon_t *anon){
    if(anon->resp) return true;
    ONCE(anon->reading_up){
        imap_client_must_read(anon->c, &anon->cread, cread_cb);
    }
    return false;
}

// allowed prelogin commands:
//   - ERROR,
//   - PLUS_REQ,
//   - NOOP
//   - CAPABILITY
//   - LOGOUT
//   - LOGIN

static derr_t process_login_cmd(anon_t *anon, bool *ok){
    derr_t e = E_OK;
    *ok = false;

    imap_cmd_t *cmd = STEAL(imap_cmd_t, &anon->cmd);
    imap_cmd_arg_t *arg = &cmd->arg;
    ie_dstr_t **tag = &cmd->tag;
    link_t *out = &anon->resps;

    bool insec;

    switch(cmd->type){
        case IMAP_CMD_ERROR:
            PROP_GO(&e, respond_error(tag, &arg->error, out), cu);
            break;

        case IMAP_CMD_PLUS_REQ:
            PROP_GO(&e, respond_plus(out), cu);
            break;

        case IMAP_CMD_CAPA:
            PROP_GO(&e, respond_capas(tag, build_capas_prelogin, out), cu);
            break;

        case IMAP_CMD_NOOP:
            PROP_GO(&e, respond_noop(tag, out), cu);
            break;

        case IMAP_CMD_LOGOUT:
            PROP_GO(&e, respond_logout(tag, out), cu);
            anon->logged_out = true;
            break;

        case IMAP_CMD_LOGIN:
            // LOGIN received!
            anon->tag = STEAL(ie_dstr_t, tag);
            anon->user = STEAL(dstr_t, &arg->login->user->dstr);
            anon->pass = STEAL(dstr_t, &arg->login->pass->dstr);
            *ok = true;
            break;

        case IMAP_CMD_STARTTLS:
            insec = anon->s->conn->security == IMAP_SEC_INSECURE;
            PROP_GO(&e, respond_bad_starttls(tag, insec, out), cu);
            break;

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
        case IMAP_CMD_UNSELECT:
        case IMAP_CMD_IDLE:
        case IMAP_CMD_IDLE_DONE:
            PROP_GO(&e, respond_too_early(tag, out), cu);
            break;

        case IMAP_CMD_AUTH:
        case IMAP_CMD_XKEYSYNC:
        case IMAP_CMD_XKEYSYNC_DONE:
        case IMAP_CMD_XKEYADD:
            PROP_GO(&e, respond_not_supported(tag, out), cu);
            break;
    }

    // if there's something to write, make sure to start that process
    (void)advance_writes_dn(anon);

cu:
    imap_cmd_free(cmd);
    return e;
}

static derr_t check_capas(ie_st_resp_t *st){
    derr_t e = E_OK;

    if(!st->code || st->code->type != IE_ST_CODE_CAPA){
        ORIG(&e, E_RESPONSE, "no CAPABILITY response with login");
    }

    bool enable = false;
    bool unselect = false;
    bool idle = false;
    bool uidplus = false;
    bool qresync = false;
    bool condstore = false;
    bool xkey = false;
    ie_dstr_t *ptr = st->code->arg.capa;
    for(; ptr; ptr = ptr->next){
        dstr_t s = ptr->dstr;
        if(dstr_ieq(s, DSTR_LIT("enable"))){ enable = true; continue; }
        if(dstr_ieq(s, DSTR_LIT("unselect"))){ unselect = true; continue; }
        if(dstr_ieq(s, DSTR_LIT("idle"))){ idle = true; continue; }
        if(dstr_ieq(s, DSTR_LIT("uidplus"))){ uidplus = true; continue; }
        if(dstr_ieq(s, DSTR_LIT("qresync"))){ qresync = true; continue; }
        if(dstr_ieq(s, DSTR_LIT("condstore"))){ condstore = true; continue; }
        if(dstr_ieq(s, DSTR_LIT("xkey"))){ xkey = true; continue; }
    }
    bool ok = true;
    if(!enable){ ok = false; TRACE(&e, "missing ENABLE capability\n"); }
    if(!unselect){ ok = false; TRACE(&e, "missing UNSELECT capability\n"); }
    if(!idle){ ok = false; TRACE(&e, "missing IDLE capability\n"); }
    if(!uidplus){ ok = false; TRACE(&e, "missing UIDPLUS capability\n"); }
    if(!qresync){ ok = false; TRACE(&e, "missing QRESYNC capability\n"); }
    if(!condstore){ ok = false; TRACE(&e, "missing CONDSTORE capability\n"); }
    if(!xkey){ ok = false; TRACE(&e, "missing XKEY capability\n"); }

    if(!ok){
        ORIG(&e, E_RESPONSE, "missing one or more capabilities");
    }

    return e;
}

static derr_t process_login_resp(anon_t *anon, bool *ok){
    derr_t e = E_OK;
    *ok = false;

    imap_resp_t *resp = STEAL(imap_resp_t, &anon->resp);
    ie_st_resp_t *st;
    if((st = match_tagged(resp, DSTR_LIT("anon"), anon->ntags))){
        // LOGIN tagged response
        if(st->status == IE_ST_OK){
            PROP_GO(&e, check_capas(st), cu);
            anon->login_success = true;
        }else{
            // this user and pass not useful
            anon->login_success = false;
            dstr_free(&anon->user);
            dstr_free0(&anon->pass);
        }
        *ok = true;
        goto cu;
    }else if((st = match_info(resp))){
        // informational response
        LOG_INFO("informational response: %x\n", FIRESP(resp));
    }else{
        ORIG_GO(&e,
            E_RESPONSE, "unexpected LOGIN response: %x", cu, FIRESP(resp)
        );
    }

cu:
    imap_resp_free(resp);
    return e;
}

static ie_dstr_t *mktag(derr_t *e, anon_t *anon){
    if(is_error(*e)) return NULL;
    DSTR_VAR(buf, 64);
    IF_PROP(e, FMT(&buf, "anon%x", FU(++anon->ntags)) ){
        return NULL;
    }
    return ie_dstr_new2(e, buf);
}

static void queue_write_up(derr_t *e, anon_t *anon, imap_cmd_t *cmd){
    cmd = imap_cmd_assert_writable(e, cmd, &anon->c->exts);
    if(is_error(*e)) return;
    link_list_append(&anon->cmds, &cmd->link);
    (void)advance_writes_up(anon);
}

// write a LOGIN command upwards
static derr_t write_up(anon_t *anon){
    derr_t e = E_OK;

    ie_dstr_t *tag = mktag(&e, anon);
    ie_dstr_t *user = ie_dstr_new2(&e, anon->user);
    ie_dstr_t *pass = ie_dstr_new2(&e, anon->pass);
    ie_login_cmd_t *login = ie_login_cmd_new(&e, user, pass);
    imap_cmd_arg_t arg = { .login = login };
    imap_cmd_t *cmd = imap_cmd_new(&e, tag, IMAP_CMD_LOGIN, arg);
    queue_write_up(&e, anon, cmd);
    CHECK(&e);

    return e;
}

// write a LOGIN response downwards
static derr_t write_dn(anon_t *anon, bool success){
    derr_t e = E_OK;

    if(success){
        PROP(&e, RESP_OK(&anon->tag, "oh hey, I know you!", &anon->resps) );
    }else{
        PROP(&e, RESP_NO(&anon->tag, "nice try, imposter!", &anon->resps) );
    }

    (void)advance_writes_dn(anon);

    return e;
}

static void reset(anon_t *anon){
    anon->login_cmd_recvd = false;
    anon->write_up_sent = false;
    anon->login_resp_recvd = false;
    anon->write_dn_sent = false;
}

static void advance_state(anon_t *anon){
    bool ok;

    if(anon->canceled || is_error(anon->e)) goto fail;

    // always finish writes before proceeding
    bool ok_up = advance_writes_up(anon);
    bool ok_dn = advance_writes_dn(anon);
    if(!ok_up || !ok_dn) return;

    if(anon->logged_out){
        // finished all writes, shut down the server
        imap_server_logged_out(anon->s);
        imap_client_cancel(anon->c);
        // wait for the server and client to finish successfully
        if(!anon->s->awaited) return;
        if(!anon->c->awaited) return;
        imap_server_free(&anon->s);
        imap_client_free(&anon->c);
        goto done;
    }

    while(!anon->done){
        // recv a LOGIN cmd from below
        while(!anon->login_cmd_recvd){
            ok = advance_reads_dn(anon);
            if(!ok) return;
            PROP_GO(&anon->e, process_login_cmd(anon, &ok), fail);
            if(!ok) continue;
            // have login cmd
            anon->login_cmd_recvd = true;
        }
        // send our LOGIN cmd upwards
        ONCE(anon->write_up_sent) PROP_GO(&anon->e, write_up(anon), fail);
        // read LOGIN response from above
        while(!anon->login_resp_recvd){
            ok = advance_reads_up(anon);
            if(!ok) return;
            PROP_GO(&anon->e, process_login_resp(anon, &ok), fail);
            if(!ok) continue;
            anon->login_resp_recvd = true;
        }
        // send LOGIN response downwards
        ONCE(anon->write_dn_sent){
            PROP_GO(&anon->e, write_dn(anon, anon->login_success), fail);
            // come back when we finish writing
            return;
        }
        anon->done = anon->login_success;
        reset(anon);
    }

    // success
    goto done;

fail:
    imap_server_cancel(anon->s, anon->broken_conn && !anon->canceled);
    imap_client_cancel(anon->c);
    if(!anon->s->awaited) return;
    if(!anon->c->awaited) return;
    imap_server_free(&anon->s);
    imap_client_free(&anon->c);

done:
    anon_cb cb = anon->cb;
    void *cb_data = anon->cb_data;
    imap_server_t *s = STEAL(imap_server_t, &anon->s);
    imap_client_t *c = STEAL(imap_client_t, &anon->c);
    dstr_t user = STEAL(dstr_t, &anon->user);
    dstr_t pass = STEAL(dstr_t, &anon->pass);
    derr_t e = anon->e;
    if(!is_error(e) && anon->canceled){
        e.type = E_CANCELED;
    }
    anon_free(anon);

    // s and c may be NULL after LOGOUT
    if(s) imap_server_unawait(s);
    if(c) imap_client_unawait(c);

    cb(cb_data, e, s, c, user, pass);
}

// no args are consumed on failure
derr_t anon_new(
    scheduler_i *scheduler,
    imap_server_t *s,
    imap_client_t *c,
    anon_cb cb,
    void *cb_data,
    link_t *list
){
    derr_t e = E_OK;

    anon_t *anon = DMALLOC_STRUCT_PTR(&e, anon);
    CHECK(&e);

    // success!

    imap_server_must_await(s, sawait_cb, NULL);
    imap_client_must_await(c, cawait_cb, NULL);
    s->data = anon;
    c->data = anon;

    *anon = (anon_t){
        .s = s,
        .c = c,
        .scheduler = scheduler,
        .cb = cb,
        .cb_data = cb_data,
    };

    schedulable_prep(&anon->schedulable, scheduled);

    schedule(anon);
    link_list_append(list, &anon->link);

    return e;
}

// citm can cancel us (it should also remove us from its list)
void anon_cancel(link_t *link){
    anon_t *anon = CONTAINER_OF(link, anon_t, link);
    anon->canceled = true;
    schedule(anon);
}
