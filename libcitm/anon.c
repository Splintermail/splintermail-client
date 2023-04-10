#include "libcitm/libcitm.h"

typedef struct {
    link_t link;  // citm->anons

    scheduler_i *scheduler;
    schedulable_t schedulable;

    imap_server_t *imap_dn;
    imap_client_t *imap_up;

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

    bool write_up_started : 1;
    bool write_up_done : 1;
    bool write_dn_started : 1;
    bool write_dn_done : 1;
    bool reading_up : 1;
    bool reading_dn : 1;

    bool login_cmd_recvd : 1;
    bool write_up_sent : 1;
    bool login_resp_recvd : 1;
    bool write_dn_sent : 1;
    bool login_success : 1;
    bool login_done : 1;

    bool canceled : 1;
    bool failed : 1;
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
    imap_server_free(&anon->imap_dn);
    imap_client_free(&anon->imap_up);
    ie_dstr_free(anon->tag);
    dstr_free(&anon->user);
    dstr_free0(&anon->pass);
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
    if(anon->failed) DROP_VAR(&e);
    if(anon->canceled || anon->failed){
        DROP_CANCELED_VAR(&e);
    }else{
        UPGRADE_CANCELED_VAR(&e, E_INTERNAL);
    }
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
    if(anon->failed) DROP_VAR(&e);
    if(anon->canceled || anon->failed || anon->logged_out){
        DROP_CANCELED_VAR(&e);
    }else{
        UPGRADE_CANCELED_VAR(&e, E_INTERNAL);
    }
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
    anon->write_dn_done = true;
    schedule(anon);
}

static void cwrite_cb(imap_client_t *c, imap_client_write_t *req){
    (void)c;
    anon_t *anon = CONTAINER_OF(req, anon_t, cwrite);
    anon->write_up_done = true;
    schedule(anon);
}

#define ONCE(x) if(!x && (x = true))

// returns bool ok
static bool advance_writes_up(anon_t *anon){
    // have we finished the last write?
    if(anon->write_up_started){
        if(!anon->write_up_done) return false;
        anon->write_up_started = false;
        anon->write_up_done = false;
    }

    // should we start a new write?
    link_t *link;
    if((link = link_list_pop_first(&anon->cmds))){
        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
        imap_client_must_write(anon->imap_up, &anon->cwrite, cmd, cwrite_cb);
        anon->write_up_started = true;
        return false;
    }

    return true;
}

// returns bool ok
static bool advance_writes_dn(anon_t *anon){
    // have we finished the last write?
    if(anon->write_dn_started){
        if(!anon->write_dn_done) return false;
        anon->write_dn_started = false;
        anon->write_dn_done = false;
    }

    // should we start a new write?
    link_t *link;
    if((link = link_list_pop_first(&anon->resps))){
        imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);
        imap_server_must_write(anon->imap_dn, &anon->swrite, resp, swrite_cb);
        anon->write_dn_started = true;
        return false;
    }

    return true;
}

// returns bool ok
static bool advance_reads_dn(anon_t *anon){
    if(anon->cmd) return true;
    ONCE(anon->reading_dn){
        imap_server_must_read(anon->imap_dn, &anon->sread, sread_cb);
    }
    return false;
}

// returns bool ok
static bool advance_reads_up(anon_t *anon){
    if(anon->resp) return true;
    ONCE(anon->reading_up){
        imap_client_must_read(anon->imap_up, &anon->cread, cread_cb);
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
            insec = anon->imap_dn->conn == IMAP_SEC_INSECURE;
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

static derr_t process_login_resp(anon_t *anon, bool *ok){
    derr_t e = E_OK;
    *ok = false;

    imap_resp_t *resp = STEAL(imap_resp_t, &anon->resp);
    ie_st_resp_t *st;
    if((st = match_tagged(resp, DSTR_LIT("anon"), anon->ntags))){
        // LOGIN tagged response
        if(st->status == IE_ST_OK){
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
            E_RESPONSE,
            "unexpected LOGIN response: %x\n",
            cu,
            FIRESP(resp)
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
    cmd = imap_cmd_assert_writable(e, cmd, &anon->imap_up->exts);
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

    if(anon->canceled || anon->failed) goto cu;
    if(is_error(anon->e)) goto fail;

    // always finish writes before proceeding
    bool ok_up = advance_writes_up(anon);
    bool ok_dn = advance_writes_dn(anon);
    if(!ok_up || !ok_dn) return;

    if(anon->logged_out){
        // finished all writes, shut down the server
        imap_server_logged_out(anon->imap_dn);
        imap_client_cancel(anon->imap_up);
        // wait for the server to finish successfully
        if(!anon->imap_dn->awaited) return;
        goto cu;
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

    // XXX: check capabilities?

    // success is complete
    anon_cb cb = anon->cb;
    void *cb_data = anon->cb_data;
    imap_server_t *imap_dn = STEAL(imap_server_t, &anon->imap_dn);
    imap_client_t *imap_up = STEAL(imap_client_t, &anon->imap_up);
    dstr_t user = STEAL(dstr_t, &anon->user);
    dstr_t pass = STEAL(dstr_t, &anon->pass);
    anon_free(anon);

    imap_server_must_await(imap_dn, NULL, NULL);
    imap_client_must_await(imap_up, NULL, NULL);

    cb(cb_data, imap_dn, imap_up, user, pass);

    return;

fail:
    anon->failed = true;
    // XXX: tell the client why we're closing?
    DUMP(anon->e);
    DROP_VAR(&anon->e);

cu:
    imap_server_cancel(anon->imap_dn);
    imap_client_cancel(anon->imap_up);

    // wait for our async resources to finish
    if(!anon->imap_dn->awaited) return;
    if(!anon->imap_up->awaited) return;

    anon_free(anon);
}

void anon_new(
    scheduler_i *scheduler,
    citm_conn_t *conn_dn,
    citm_conn_t *conn_up,
    anon_cb cb,
    void *cb_data,
    link_t *list
){
    derr_t e = E_OK;

    imap_server_t *imap_dn = NULL;
    imap_client_t *imap_up = NULL;

    anon_t *anon = DMALLOC_STRUCT_PTR(&e, anon);
    CHECK_GO(&e, fail);

    PROP_GO(&e, imap_server_new(&imap_dn, scheduler, conn_dn), fail);
    PROP_GO(&e, imap_client_new(&imap_up, scheduler, conn_up), fail);

    // success!

    imap_server_must_await(imap_dn, sawait_cb, NULL);
    imap_client_must_await(imap_up, cawait_cb, NULL);
    imap_dn->data = anon;
    imap_up->data = anon;

    *anon = (anon_t){
        .imap_dn = imap_dn,
        .imap_up = imap_up,
        .scheduler = scheduler,
        .cb = cb,
        .cb_data = cb_data,
    };

    schedulable_prep(&anon->schedulable, scheduled);

    schedule(anon);
    link_list_append(list, &anon->link);

    return;

fail:
    conn_dn->close(conn_dn);
    conn_up->close(conn_up);
    imap_server_free(&imap_dn);
    imap_client_free(&imap_up);
    if(anon) free(anon);
    // XXX: tell the client what happened?
    DUMP(e);
    DROP_VAR(&e);
}

// citm can cancel us (it should also remove us from its list)
void anon_cancel(link_t *link){
    anon_t *anon = CONTAINER_OF(link, anon_t, link);
    anon->canceled = true;
    schedule(anon);
}
