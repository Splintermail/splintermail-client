#include "libcitm/libcitm.h"

typedef struct {
    hash_elem_t elem;
    scheduler_i *scheduler;
    schedulable_t schedulable;
    keydir_i *kd;

    preuser_cb cb;
    void *cb_data;

    dstr_t user;
    dstr_t pass;

    imap_client_read_t cread;
    imap_client_write_t cwrite;

    citm_connect_i *connect;
    citm_conn_t *conn;
    // "x"key "c"lient
    imap_client_t *xc;
    link_t servers;  // imap_server_t->link
    link_t clients;  // imap_client_t->link

    // what we have read
    imap_resp_t *resp;
    // what we are writing
    link_t cmds;
    size_t ntags;

    derr_t e;

    bool broken_conn : 1;
    bool canceled : 1;

    bool write_started : 1;
    bool write_done : 1;
    bool reading : 1;

    bool initial_sends : 1;
    bool login_done : 1;
    bool sync_done : 1;

    bool need_mykey : 1;
    bool mykey_sent : 1;
    bool mykey_done : 1;
} preuser_t;
DEF_CONTAINER_OF(preuser_t, elem, hash_elem_t);
DEF_CONTAINER_OF(preuser_t, schedulable, schedulable_t);
DEF_CONTAINER_OF(preuser_t, cread, imap_client_read_t);
DEF_CONTAINER_OF(preuser_t, cwrite, imap_client_write_t);

static void advance_state(preuser_t *p);

static void scheduled(schedulable_t *s){
    preuser_t *p = CONTAINER_OF(s, preuser_t, schedulable);
    advance_state(p);
}

static void schedule(preuser_t *p){
    p->scheduler->schedule(p->scheduler, &p->schedulable);
}

static void await_xc(
    imap_client_t *xc, derr_t e, link_t *reads, link_t *writes
){
    // we only have static reads and writes
    (void)reads;
    (void)writes;

    preuser_t *p = xc->data;
    schedule(p);

    if(is_error(p->e) || p->canceled){
        DROP_CANCELED_VAR(&e);
    }else{
        UPGRADE_CANCELED_VAR(&e, E_INTERNAL);
    }
    if(is_error(e)){
        // upwards connection failure
        p->broken_conn = true;
    }
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&p->e, &e);
}

static void sawait_cb(
    imap_server_t *s, derr_t e, link_t *reads, link_t *writes
){
    (void)reads;
    (void)writes;
    preuser_t *p = s->data;
    schedule(p);
    // we only await servers if we are canceling them
    DROP_CANCELED_VAR(&e);
    if(is_error(e)){
        TRACE(&e, "error while canceling server\n");
        DUMP(e);
        DROP_VAR(&e);
    }
}

// a stream_i await_cb, only used in specific cleanup scenarios
static void await_conn(stream_i *s, derr_t e, link_t *reads, link_t *writes){
    preuser_t *p = s->data;
    (void)reads;
    (void)writes;
    DROP_VAR(&e);
    p->conn->free(p->conn);
    p->conn = NULL;
    schedule(p);
}

static void connect_cb(void *data, citm_conn_t *conn, derr_t e){
    preuser_t *p = data;
    schedule(p);

    // done with connect
    p->connect = NULL;
    p->conn = conn;

    if(p->canceled || is_error(p->e)){
        DROP_CANCELED_VAR(&e);
    }
    UPGRADE_CANCELED_VAR(&e, E_INTERNAL);
    if(is_error(e)){
        // upwards connection failure
        /* technically this isn't a broken connection but a failure to connect,
           though since we already have an active connection, distinguishing
           them isn't too significant to the end user */
        p->broken_conn = true;
    }
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&p->e, &e);
}

static void cread_cb(
    imap_client_t *c, imap_client_read_t *req, imap_resp_t *resp
){
    (void)c;
    preuser_t *p = CONTAINER_OF(req, preuser_t, cread);
    p->reading = false;
    p->resp = resp;
    schedule(p);
}

static void cwrite_cb(imap_client_t *c, imap_client_write_t *req){
    (void)c;
    preuser_t *p = CONTAINER_OF(req, preuser_t, cwrite);
    p->write_done = true;
    schedule(p);
}

#define ONCE(x) if(!x && (x = true))

// returns bool ok
static bool advance_writes(preuser_t *p){
    // have we finished the last write?
    if(p->write_started){
        if(!p->write_done) return false;
        p->write_started = false;
        p->write_done = false;
    }

    // should we start a new write?
    link_t *link;
    if((link = link_list_pop_first(&p->cmds))){
        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
        imap_client_must_write(p->xc, &p->cwrite, cmd, cwrite_cb);
        p->write_started = true;
        return false;
    }

    return true;
}

// returns bool ok
static bool advance_reads(preuser_t *p){
    if(p->resp) return true;
    ONCE(p->reading) imap_client_must_read(p->xc, &p->cread, cread_cb);
    return false;
}

static ie_dstr_t *mktag(derr_t *e, preuser_t *p){
    if(is_error(*e)) return NULL;
    DSTR_VAR(buf, 64);
    IF_PROP(e, FMT(&buf, "preuser%x", FU(++p->ntags)) ){
        return NULL;
    }
    return ie_dstr_new2(e, buf);
}

static void queue_write(derr_t *e, preuser_t *p, imap_cmd_t *cmd){
    cmd = imap_cmd_assert_writable(e, cmd, &p->xc->exts);
    if(is_error(*e)) return;
    link_list_append(&p->cmds, &cmd->link);
    (void)advance_writes(p);
}

static derr_t send_login(preuser_t *p){
    derr_t e = E_OK;

    // initial commands

    ie_dstr_t *tag = mktag(&e, p);
    ie_dstr_t *user = ie_dstr_new2(&e, p->user);
    ie_dstr_t *pass = ie_dstr_new2(&e, p->pass);
    ie_login_cmd_t *login = ie_login_cmd_new(&e, user, pass);
    imap_cmd_arg_t arg = { .login = login };
    imap_cmd_t *cmd = imap_cmd_new(&e, tag, IMAP_CMD_LOGIN, arg);
    queue_write(&e, p, cmd);
    CHECK(&e);

    return e;
}

static derr_t send_sync(preuser_t *p){
    derr_t e = E_OK;

    ie_dstr_t *tag = mktag(&e, p);
    imap_cmd_t *cmd = xkeysync_cmd(&e, tag, p->kd);
    queue_write(&e, p, cmd);
    CHECK(&e);

    return e;
}

static derr_t send_done(preuser_t *p){
    derr_t e = E_OK;

    imap_cmd_arg_t arg = {0};
    imap_cmd_t *cmd = imap_cmd_new(&e, NULL, IMAP_CMD_XKEYSYNC_DONE, arg);
    queue_write(&e, p, cmd);
    CHECK(&e);

    return e;
}

static derr_t send_mykey(preuser_t *p){
    derr_t e = E_OK;

    ie_dstr_t *tag = mktag(&e, p);
    imap_cmd_t *cmd = xkeyadd_cmd(&e, tag, p->kd->mykey(p->kd));
    queue_write(&e, p, cmd);
    CHECK(&e);

    return e;
}

typedef derr_t (*check_f)(preuser_t *p, imap_resp_t **respp, bool *ok);

static derr_t check_login(preuser_t *p, imap_resp_t **respp, bool *ok){
    derr_t e = E_OK;
    (void)p;

    imap_resp_t *resp = *respp;

    ie_st_resp_t *st;
    if(!(st = match_tagged(resp, DSTR_LIT("preuser"), 1))) return e;

    if(st->status != IE_ST_OK){
        // we have no recourse
        ORIG(&e, E_RESPONSE, "preuser_t failed to login: %x", FIRESP(resp));
    }

    *ok = true;

    return e;
}

static derr_t check_sync(preuser_t *p, imap_resp_t **respp, bool *ok){
    derr_t e = E_OK;

    // check for XKEYSYNC or PLUS responses
    bool mykey_deleted;
    PROP(&e, check_xkeysync(respp, p->kd, &mykey_deleted) );
    if(mykey_deleted) p->need_mykey = true;
    if(!*respp) return e;

    imap_resp_t *resp = *respp;

    // check for the end of the XKEYSYNC response
    ie_st_resp_t *st;
    if(!(st = match_tagged(resp, DSTR_LIT("preuser"), 2))) return e;

    if(st->status != IE_ST_OK){
        // we have no recourse
        ORIG_GO(&e, E_RESPONSE, "preuser_t xkeysync failed: %x", stay, FIRESP(resp));
    stay:
        return e;
    }

    *ok = true;

    return e;
}

static derr_t check_upload(preuser_t *p, imap_resp_t **respp, bool *ok){
    derr_t e = E_OK;

    imap_resp_t *resp = *respp;

    ie_st_resp_t *st;
    if(!(st = match_tagged(resp, DSTR_LIT("preuser"), p->ntags))) return e;

    if(st->status != IE_ST_OK){
        // we have no recourse
        ORIG(&e,
            E_RESPONSE, "preuser_t failed to upload mykey: %x", FIRESP(resp)
        );
    }

    *ok = true;

    return e;
}

// *ok means "state machine can proceed", not "let's address this resp later"
static derr_t check_resp(preuser_t *p, bool *ok, check_f check_fn){
    derr_t e = E_OK;
    *ok = false;

    imap_resp_t *resp = STEAL(imap_resp_t, &p->resp);

    PROP_GO(&e, check_fn(p, &resp, ok), cu);
    // did the check_fn set ok or consume the output?
    if(*ok || !resp) goto cu;

    // check for informational response
    ie_st_resp_t *st;
    if((st = match_info(resp))){
        LOG_INFO("informational response: %x\n", FIRESP(resp));
        goto cu;
    }

    ORIG_GO(&e, E_RESPONSE, "unexpected response: %x", cu, FIRESP(resp));

cu:
    imap_resp_free(resp);
    return e;
}

static void advance_state(preuser_t *p){
    bool ok;

    if(p->canceled || is_error(p->e)) goto cu;

    // wait to ready our imap client
    if(!p->xc){
        // wait for connect cb
        if(!p->conn) return;
        // configure our imap client
        citm_conn_t *conn = STEAL(citm_conn_t, &p->conn);
        PROP_GO(&p->e, imap_client_new(&p->xc, p->scheduler, conn), cu);
        imap_client_must_await(p->xc, await_xc, NULL);
        p->xc->data = p;
    }

    // finish any pending writes
    ok = advance_writes(p);
    if(!ok) return;

    // aggressively pipeline commands to reduce user-facing startup times

    ONCE(p->initial_sends){
        PROP_GO(&p->e, send_login(p), cu);
        PROP_GO(&p->e, send_sync(p), cu);
        PROP_GO(&p->e, send_done(p), cu);
        (void)advance_writes(p);
    }

    while(!p->login_done){
        ok = advance_reads(p);
        if(!ok) return;
        PROP_GO(&p->e, check_resp(p, &ok, check_login), cu);
        if(!ok) continue;
        p->login_done = true;
    }

    while(!p->sync_done){
        ok = advance_reads(p);
        if(!ok) return;
        PROP_GO(&p->e, check_resp(p, &ok, check_sync), cu);
        if(!ok) continue;
        p->sync_done = true;
    }

    // handle the case where mykey was not already present

    if(p->need_mykey){
        ONCE(p->mykey_sent){
            PROP_GO(&p->e, send_mykey(p), cu);
            (void)advance_writes(p);
        }
        while(!p->mykey_done){
            ok = advance_reads(p);
            if(!ok) return;
            PROP_GO(&p->e, check_resp(p, &ok, check_upload), cu);
            if(!ok) continue;
            p->mykey_done = true;
        }
    }

    // preuser_t's job is now complete

    // assert that we are done with IO
    if(p->write_started){
        ORIG_GO(&p->e, E_INTERNAL, "preuser is not done writing!", cu);
    }
    if(p->reading){
        ORIG_GO(&p->e, E_INTERNAL, "preuser is not done reading!", cu);
    }

    // success (fallthru)

cu:
    // cleanup intermediate states in failure cases
    if(is_error(p->e) || p->canceled){
        if(p->connect){
            p->connect->cancel(p->connect);
            return;
        }
        if(p->conn){
            stream_i *s = p->conn->stream;
            s->data = p;
            s->await(s, await_conn);
            s->cancel(s);
            return;
        }
        // close xc ourselves, in case it has any pending io of ours
        if(p->xc){
            imap_client_cancel(p->xc);
            if(!p->xc->awaited) return;
            imap_client_free(&p->xc);
        }
        // close servers ourselves, in case it's due to a broken conn
        imap_server_t *s, *temp;
        LINK_FOR_EACH_SAFE(s, temp, &p->servers, imap_server_t, link){
            if(!s->awaited){
                s->data = p;
                imap_server_must_await(s, sawait_cb, NULL);
                imap_server_cancel(s, p->broken_conn && !p->canceled);
                continue;
            }
            imap_server_free(&s);
        }
        if(!link_list_isempty(&p->servers)) return;
    }

    imap_client_t *xc = STEAL(imap_client_t, &p->xc);
    if(xc) imap_client_unawait(xc);

    // in failure cases, we may have a response or some commands laying around
    imap_resp_free(STEAL(imap_resp_t, &p->resp));
    link_t *link;
    while((link = link_list_pop_first(&p->cmds))){
        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
        imap_cmd_free(cmd);
    }

    dstr_t user = STEAL(dstr_t, &p->user);
    dstr_free0(&p->pass);
    link_t servers = {0};
    link_t clients = {0};
    link_list_append_list(&servers, &p->servers);
    link_list_append_list(&clients, &p->clients);
    keydir_i *kd = STEAL(keydir_i, &p->kd);

    preuser_cb cb = p->cb;
    void *cb_data = p->cb_data;

    derr_t e = p->e;
    if(!is_error(e) && p->canceled){
        e.type = E_CANCELED;
    }

    hash_elem_remove(&p->elem);
    schedulable_cancel(&p->schedulable);
    free(p);

    cb(cb_data, e, user, &servers, &clients, kd, xc);

    return;
}

// no args are consumed on failure
derr_t preuser_new(
    scheduler_i *scheduler,
    citm_io_i *io,
    dstr_t user,
    dstr_t pass,
    keydir_i *kd,
    imap_server_t *server,
    imap_client_t *client,
    preuser_cb cb,
    void *cb_data,
    hashmap_t *out
){
    derr_t e = E_OK;

    preuser_t *p = DMALLOC_STRUCT_PTR(&e, p);
    CHECK(&e);

    citm_connect_i *connect;
    PROP_GO(&e, io->connect_imap(io, connect_cb, p, &connect), fail);

    // success

    *p = (preuser_t){
        .scheduler = scheduler,
        .connect = connect,
        .user = user,
        .pass = pass,
        .kd = kd,
        .cb = cb,
        .cb_data = cb_data,
    };

    link_list_append(&p->servers, &server->link);
    link_list_append(&p->clients, &client->link);

    schedulable_prep(&p->schedulable, scheduled);

    hash_elem_t *old = hashmap_sets(out, &p->user, &p->elem);
    if(old) LOG_FATAL("preuser found existing user %x\n", FD_DBG(p->user));

    return e;

fail:
    free(p);
    return e;
}

// when another connection pair is ready but our keysync isn't ready yet
void preuser_add_pair(hash_elem_t *elem, imap_server_t *s, imap_client_t *c){
    preuser_t *p = CONTAINER_OF(elem, preuser_t, elem);
    link_list_append(&p->servers, &s->link);
    link_list_append(&p->clients, &c->link);
    if(p->canceled) schedule(p);
}

// elem should already have been removed
void preuser_cancel(hash_elem_t *elem){
    preuser_t *p = CONTAINER_OF(elem, preuser_t, elem);
    p->canceled = true;
    schedule(p);
}
