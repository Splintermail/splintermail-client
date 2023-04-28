#include "libcitm.h"

typedef derr_t (*cmd_cb_f)(fetcher_t *fetcher, imap_resp_t **respp);

DSTR_STATIC(prefix, "fetcher");

// we can pipline commands, so we keep a list of what to do when they finish
typedef struct {
    link_t link;
    size_t tag;
    cmd_cb_f cb;
} cmd_cb_t;
DEF_CONTAINER_OF(cmd_cb_t, link, link_t)

static cmd_cb_t *cmd_cb_new(derr_t *e, size_t tag, cmd_cb_f cb){
    if(is_error(*e)) goto fail;

    cmd_cb_t *cmd_cb = DMALLOC_STRUCT_PTR(e, cmd_cb);
    CHECK_GO(e, fail);
    *cmd_cb = (cmd_cb_t){ .tag = tag, .cb = cb };

    return cmd_cb;

fail:
    return NULL;
}

static void cmd_cb_free(cmd_cb_t *cmd_cb){
    if(!cmd_cb) return;
    link_remove(&cmd_cb->link);
    free(cmd_cb);
}

static void fetcher_free_passthru(fetcher_t *fetcher){
    passthru_req_free(STEAL(passthru_req_t, &fetcher->passthru.req));
    passthru_resp_arg_free(
        fetcher->passthru.type,
        STEAL(passthru_resp_arg_u, &fetcher->passthru.arg)
    );
    fetcher->passthru.sent = false;
}

static void fetcher_free_select(fetcher_t *fetcher){
    fetcher->select.needed = false;
    ie_mailbox_free(STEAL(ie_mailbox_t, &fetcher->select.mailbox));
    fetcher->select.examine = false;
}

static void fetcher_free_close(fetcher_t *fetcher){
    fetcher->close.needed = false;
}

static void fetcher_free_unselect(fetcher_t *fetcher){
    fetcher->unselect.sent = false;
    fetcher->unselect.done = false;
}

void fetcher_free(fetcher_t *fetcher){
    if(!fetcher) return;

    // free unfinished state
    fetcher_free_passthru(fetcher);
    fetcher_free_select(fetcher);
    fetcher_free_close(fetcher);
    fetcher_free_unselect(fetcher);

    // free any imap cmds or resps laying around
    imap_resp_free(fetcher->resp);
    link_t *link;
    while((link = link_list_pop_first(&fetcher->cmds))){
        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
        imap_cmd_free(cmd);
    }
    // free any remaining fetcher_cb's
    while((link = link_list_pop_first(&fetcher->cmd_cbs))){
        cmd_cb_t *cmd_cb = CONTAINER_OF(link, cmd_cb_t, link);
        cmd_cb_free(cmd_cb);
    }
}

static void advance_state(fetcher_t *fetcher);

static void scheduled(schedulable_t *s){
    fetcher_t *fetcher = CONTAINER_OF(s, fetcher_t, schedulable);
    advance_state(fetcher);
}

static void schedule(fetcher_t *fetcher){
    if(fetcher->awaited) return;
    fetcher->scheduler->schedule(fetcher->scheduler, &fetcher->schedulable);
}

#define ONCE(x) if(!x && (x = true))

static void cread_cb(
    imap_client_t *c, imap_client_read_t *req, imap_resp_t *resp
){
    (void)req;
    fetcher_t *fetcher = c->data;
    fetcher->reading = false;
    fetcher->resp = resp;
    schedule(fetcher);
}

// returns bool ok
static bool advance_reads(fetcher_t *fetcher){
    if(fetcher->resp) return true;
    ONCE(fetcher->reading){
        imap_client_must_read(fetcher->c, &fetcher->cread, cread_cb);
    }
    return false;
}

static void cwrite_cb(imap_client_t *c, imap_client_write_t *req){
    (void)req;
    fetcher_t *fetcher = c->data;
    fetcher->writing = false;
    schedule(fetcher);
}

// returns bool ok
static bool advance_writes(fetcher_t *fetcher){
    // do we have a write in flight?
    if(fetcher->writing) return false;

    // is there something to write?
    link_t *link;
    if((link = link_list_pop_first(&fetcher->cmds))){
        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
        imap_client_must_write(fetcher->c, &fetcher->cwrite, cmd, cwrite_cb);
        fetcher->writing = true;
        return false;
    }
    return true;
}

static void await_cb(
    imap_client_t *c, derr_t e, link_t *reads, link_t *writes
){
    // our reads and writes are static
    (void)reads;
    (void)writes;
    fetcher_t *fetcher = c->data;
    if(is_error(e)){
        if(fetcher->failed){
            DROP_VAR(&e);
        }else if(fetcher->canceled){
            DROP_CANCELED_VAR(&e);
        }else{
            UPGRADE_CANCELED_VAR(&e, E_INTERNAL);
        }
        KEEP_FIRST_IF_NOT_CANCELED_VAR(&fetcher->e, &e);
    }else{
        TRACE_ORIG(&e,
            E_INTERNAL, "imap_client_t exited without error or logout"
        );
    }
    schedule(fetcher);
}

// XXX /////////////////////////

// disconnect from the maildir, this can happen many times for one fetcher_t
static void fetcher_disconnect(fetcher_t *fetcher){
    if(!fetcher->up_active) return;
    dirmgr_close_up(fetcher->dirmgr, &fetcher->up);
    // XXX
    up_free(&fetcher->up);
    fetcher->up_active = false;
    // XXX wtf is this?
    // if there was an unselect in flight, it's now invalid
    // (need_unselected() is written to deal with this)
    fetcher_free_unselect(fetcher);
}

void fetcher_passthru_req(fetcher_t *fetcher, passthru_req_t *passthru_req){
    fetcher->passthru.req = passthru_req;
    schedule(fetcher);
}

void fetcher_select(fetcher_t *fetcher, ie_mailbox_t *m, bool examine){
    fetcher->select.needed = true;
    fetcher->select.mailbox = m;
    fetcher->select.examine = examine;
    schedule(fetcher);
}

void fetcher_unselect(fetcher_t *fetcher){
    // note: fetcher.unselect is a sub state machine; this starts fetcher.close
    fetcher->close.needed = true;
    schedule(fetcher);
}

void fetcher_set_dirmgr(fetcher_t *fetcher, dirmgr_t *dirmgr){
    fetcher->dirmgr = dirmgr;
    schedule(fetcher);
}

static void fetcher_up_schedule(up_cb_i *up_cb){
    fetcher_t *fetcher = CONTAINER_OF(up_cb, fetcher_t, up_cb);
    schedule(fetcher);
}

void fetcher_prep(
    fetcher_t *fetcher,
    scheduler_i *scheduler,
    imap_client_t *c,
    dirmgr_t *dirmgr,
    fetcher_cb_i *cb
){
    derr_t e = E_OK;

    *fetcher = (fetcher_t){
        .cb = cb,
        .scheduler = scheduler,
        .c = c,
        .dirmgr = dirmgr,
        .up_cb = { .selected = fetcher_up_selected },
    };

    schedulable_prep(&fetcher->schedulable, scheduled);


    return e;

fail_refs:
    refs_free(&fetcher->refs);
    return e;
}

void fetcher_start(fetcher_t *fetcher){
    // await our client
    fetcher->c->data = fetcher;
    imap_client_must_await(fetcher->c, await_cb, NULL);
    // start our ENABLE right away
    schedule(fetcher);
}


//  IMAP LOGIC  ///////////////////////////////////////////////////////////////

static ie_dstr_t *mktag(derr_t *e, size_t tag){
    if(is_error(*e)) return NULL;
    DSTR_VAR(buf, 64);
    IF_PROP(e, FMT(&buf, "fetcher%x", FU(tag)) ){
        return NULL;
    }
    return ie_dstr_new2(e, buf);
}

// send a command and store its callback
static void send_cmd(
    derr_t *e,
    fetcher_t *fetcher,
    imap_cmd_type_t type,
    imap_cmd_arg_t arg,
    cmd_cb_f cb
){
    if(is_error(*e)) goto fail;

    size_t tag = ++fetcher->ntags;
    ie_dstr_t *tagstr = mktag(e, tag);
    imap_cmd_t *cmd = imap_cmd_new(&e, tagstr, type, arg);
    cmd = imap_cmd_assert_writable(e, cmd, &fetcher->c->exts);
    CHECK_GO(e, fail);

    cmd_cb_t *cmd_cb = cmd_cb_new(e, tag, cb);
    CHECK_GO(e, fail_cmd);

    link_list_append(&fetcher->cmds, &cmd->link);
    link_list_append(&fetcher->cmd_cbs, &cmd_cb->link);

    return;

fail:
    imap_cmd_arg_free(type, arg);
    return;

fail_cmd:
    imap_cmd_free(cmd);
    return;
}

// passthru_done is an cmd_cb_f
static derr_t passthru_done(fetcher_t *fetcher, ie_st_resp_t **st_respp){
    derr_t e = E_OK;

    // send out the response
    passthru_resp_t *passthru_resp = passthru_resp_new(&e,
        fetcher->passthru.type,
        STEAL(passthru_resp_arg_u, &fetcher->passthru.arg),
        STEAL(ie_resp_t, strespp)
    );

    // let go of the passthru_req
    fetcher_free_passthru(fetcher);
    CHECK(&e);

    fetcher->cb->passthru_resp(fetcher->cb, passthru_resp);

    return e;
}

static derr_t list_resp(fetcher_t *fetcher, const ie_list_resp_t *list){
    derr_t e = E_OK;

    if(!fetcher->passthru.req || fetcher->passthru.req->type != PASSTHRU_LIST){
        ORIG(&e, E_INTERNAL, "got list response without PASSTHRU_LIST");
    }

    // verify that the separator is actually "/"
    if(list->sep != '/'){
        TRACE(&e, "Got folder separator of %x but only / is supported\n",
                FC(list->sep));
        ORIG(&e, E_RESPONSE, "invalid folder separator");
    }

    // store a copy of the list response
    fetcher->passthru.arg.list = passthru_list_resp_add(&e,
            fetcher->passthru.arg.list, ie_list_resp_copy(&e, list));
    CHECK(&e);

    return e;
}

static derr_t lsub_resp(fetcher_t *fetcher, const ie_list_resp_t *lsub){
    derr_t e = E_OK;

    if(!fetcher->passthru.req || fetcher->passthru.req->type != PASSTHRU_LSUB){
        ORIG(&e, E_INTERNAL, "got lsub response without PASSTHRU_LSUB");
    }

    // verify that the separator is actually "/"
    if(lsub->sep != '/'){
        TRACE(&e, "Got folder separator of %x but only / is supported\n",
                FC(lsub->sep));
        ORIG(&e, E_RESPONSE, "invalid folder separator");
    }

    // store a copy of the lsub response
    fetcher->passthru.arg.lsub = passthru_lsub_resp_add(&e,
            fetcher->passthru.arg.lsub, ie_list_resp_copy(&e, lsub));
    CHECK(&e);

    return e;
}

static derr_t status_resp(fetcher_t *fetcher, const ie_status_resp_t *status){
    derr_t e = E_OK;

    if(!fetcher->passthru.req
            || fetcher->passthru.req->type != PASSTHRU_STATUS){
        ORIG(&e, E_INTERNAL, "got status response without PASSTHRU_STATUS");
    }

    // store a copy of the STATUS response
    fetcher->passthru.arg = (passthru_resp_arg_u){
        .status = ie_status_resp_copy(&e, status)
    };
    CHECK(&e);

    return e;
}

static derr_t send_passthru(fetcher_t *fetcher){
    derr_t e = E_OK;

    passthru_type_e type = fetcher->passthru.req->type;
    imap_cmd_arg_t *oldarg = &fetcher->passthru.req->arg;

    fetcher->passthru.type = type;
    fetcher->passthru.arg = (passthru_resp_arg_u){0};
    imap_cmd_type_t imap_type = 0;  // gcc false postive maybe-uninitialized
    imap_cmd_arg_t imap_arg = {0};
    switch(type){
        case PASSTHRU_LIST:
            // steal the imap command
            imap_arg.list = STEAL(imap_list_cmd_t, &arg->list);
            // set the imap type
            imap_type = IMAP_CMD_LIST;
            // prepare the passthru arg
            fetcher->passthru.arg.list = passthru_list_resp_new(&e);
            CHECK(&e);
            break;

        case PASSTHRU_LSUB:
            imap_arg.lsub = STEAL(imap_list_cmd_t, &arg->lsub);
            imap_type = IMAP_CMD_LSUB;
            fetcher->passthru.arg.lsub = passthru_lsub_resp_new(&e);
            CHECK(&e);
            break;

        case PASSTHRU_STATUS:
            imap_arg.status = STEAL(ie_status_cmd_t, &arg->status);
            imap_type = IMAP_CMD_STATUS;
            fetcher->passthru.arg.status = NULL;
            break;

        case PASSTHRU_CREATE:
            imap_arg.create = STEAL(ie_create_cmd_t, &arg->create);
            imap_type = IMAP_CMD_CREATE;
            break;

        case PASSTHRU_DELETE:
            imap_arg.delete = STEAL(ie_delete_cmd_t, &arg->delete);
            imap_type = IMAP_CMD_DELETE;
            break;

        case PASSTHRU_RENAME:
            imap_arg.rename = STEAL(ie_rename_cmd_t, &arg->rename);
            imap_type = IMAP_CMD_RENAME;
            break;

        case PASSTHRU_SUB:
            imap_arg.sub = STEAL(ie_sub_cmd_t, &arg->sub);
            imap_type = IMAP_CMD_SUB;
            break;

        case PASSTHRU_UNSUB:
            imap_arg.unsub = STEAL(ie_unsub_cmd_t, &arg->unsub);
            imap_type = IMAP_CMD_UNSUB;
            break;

        case PASSTHRU_APPEND:
            imap_arg.append = STEAL(ie_append_cmd_t, &arg->append);
            imap_type = IMAP_CMD_APPEND;
            break;
    }

    send_cmd(&e, fetcher, imap_type, imap_arg, passthru_done);
    CHECk(&e);

    return e;
}

// enable_done is an cmd_cb_f
static derr_t enable_done(fetcher_t *f, ie_st_resp_t **st_respp){
    derr_t e = E_OK;

    if((*st_respp)->status != IE_ST_OK){
        ORIG(&e, E_PARAM, "enable failed\n");
    }

    fetcher->enable.done = true;

    return e;
}

static derr_t enabled_resp(fetcher_t *fetcher, const ie_dstr_t *enabled){
    derr_t e = E_OK;

    (void)fetcher;

    bool found_condstore = false;
    bool found_qresync = false;

    for(const ie_dstr_t *enbl = enabled; enbl != NULL; enbl = enbl->next){
        DSTR_VAR(buf, 32);
        // ignore long extensions
        if(enbl->dstr.len > buf.size) continue;
        // case-insensitive matching
        PROP(&e, dstr_copy(&enbl->dstr, &buf) );
        dstr_upper(&buf);
        if(dstr_cmp(&buf, extension_token(EXT_CONDSTORE)) == 0){
            found_condstore = true;
        }else if(dstr_cmp(&buf, extension_token(EXT_QRESYNC)) == 0){
            found_qresync = true;
        }
    }

    bool pass = true;
    if(!found_condstore){
        TRACE(&e, "missing extension: CONDRESTORE\n");
        pass = false;
    }
    if(!found_qresync){
        TRACE(&e, "missing extension: QRESYNC\n");
        pass = false;
    }

    if(!pass){
        ORIG(&e, E_RESPONSE, "enable failed for some extension");
    }
    return e;
}

static derr_t send_enable(fetcher_t *fetcher){
    derr_t e = E_OK;

    // issue a command enabling CONDSTORE and QRESYNC
    ie_dstr_t *ecs = ie_dstr_new(&e, extension_token(EXT_CONDSTORE), KEEP_RAW);
    ie_dstr_t *eqr = ie_dstr_new(&e, extension_token(EXT_QRESYNC), KEEP_RAW);
    ie_dstr_t *eall = ie_dstr_add(&e, ecs, eqr);
    imap_cmd_arg_t arg = { .enable=eall };

    // store the callback and send the command
    send_cmd(&e, fetcher, IMAP_CMD_ENABLE, arg, enable_done);
    CHECK(&e);

    return e;
}

//////////////////////////////////////////////////////////////

// capas_done is an cmd_cb_f
static derr_t capas_done(fetcher_t *fetcher, ie_st_resp_t **st_respp){
    derr_t e = E_OK;

    if((*st_respp)->status != IE_ST_OK){
        ORIG(&e, E_PARAM, "capas failed\n");
    }

    if(!fetcher->capas.done){
        ORIG(&e, E_RESPONSE, "never saw capabilities");
    }

    return e;
}

// puke if a needed capability is missing
static derr_t check_capas(const ie_dstr_t *capas){
    derr_t e = E_OK;

    bool found_imap4rev1 = false;
    bool found_enable = false;
    bool found_uidplus = false;
    bool found_condstore = false;
    bool found_qresync = false;
    bool found_unselect = false;
    bool found_idle = false;

    for(const ie_dstr_t *capa = capas; capa != NULL; capa = capa->next){
        // case-insensitive matching
        if(dstr_icmp2(&buf, &DSTR_LIT("IMAP4REV1")) == 0){
            found_imap4rev1 = true;
        }else if(dstr_icmp2(&buf, extension_token(EXT_ENABLE)) == 0){
            found_enable = true;
        }else if(dstr_icmp2(&buf, extension_token(EXT_UIDPLUS)) == 0){
            found_uidplus = true;
        }else if(dstr_icmp2(&buf, extension_token(EXT_CONDSTORE)) == 0){
            found_condstore = true;
        }else if(dstr_icmp2(&buf, extension_token(EXT_QRESYNC)) == 0){
            found_qresync = true;
        }else if(dstr_icmp2(&buf, extension_token(EXT_UNSELECT)) == 0){
            found_unselect = true;
        }else if(dstr_icmp2(&buf, extension_token(EXT_IDLE)) == 0){
            found_idle = true;
        }
    }

    bool pass = true;
    if(!found_imap4rev1){
        TRACE(&e, "missing capability: IMAP4rev1\n");
        pass = false;
    }
    if(!found_enable){
        TRACE(&e, "missing capability: ENABLE\n");
        pass = false;
    }
    if(!found_uidplus){
        TRACE(&e, "missing capability: UIDPLUS\n");
        pass = false;
    }
    if(!found_condstore){
        TRACE(&e, "missing capability: CONDRESTORE\n");
        pass = false;
    }
    if(!found_qresync){
        TRACE(&e, "missing capability: QRESYNC\n");
        pass = false;
    }
    if(!found_unselect){
        TRACE(&e, "missing capability: UNSELECT\n");
        pass = false;
    }
    if(!found_idle){
        TRACE(&e, "missing capability: IDLE\n");
        pass = false;
    }

    if(!pass){
        ORIG(&e, E_RESPONSE, "IMAP server is missing capabilties");
    }
    return e;
}

static derr_t capa_resp(fetcher_t *fetcher, const ie_dstr_t *capa){
    derr_t e = E_OK;

    PROP(&e, check_capas(capa) );

    fetcher->capas.done = true;
    return e;
}

static derr_t send_capas(fetcher_t *fetcher){
    derr_t e = E_OK;

    // issue the capability command
    imap_cmd_arg_t arg = {0};
    send_cmd(&e, fetcher, IMAP_CMD_CAPA, arg, capas_done);
    CHECK(&e);

    return e;
}

// handle untagged OK responses separately from other status type responses
static derr_t untagged_ok(fetcher_t *fetcher, const ie_st_code_t *code,
        const dstr_t *text){
    derr_t e = E_OK;

    // Handle responses where the status code is what defines the behavior
    if(code != NULL){
        if(code->type == IE_ST_CODE_ALERT){
            LOG_ERROR("server ALERT message: %x\n", FD(text));
            return e;
        }
    }

    // otherwise, the fetcher can't actually handle any untagged messages
    // TODO: figure out if there are untagged messages we should handle

    TRACE(&e,
        "unhandled * OK status message with code %x and text '%x'\n",
        FU(code->type), FD_DBG(text)
    );
    ORIG(&e, E_INTERNAL, "unhandled message in fetcher_t");
}

static derr_t tagged_status_type(fetcher_t *fetcher, const ie_st_resp_t *st){
    derr_t e = E_OK;

    // peek at the first command we need a response to
    link_t *link = fetcher->inflight_cmds.next;
    if(link == NULL){
        TRACE(&e, "got tag %x with no commands in flight\n",
                FD(&st->tag->dstr));
        ORIG(&e, E_RESPONSE, "bad status type response");
    }

    // make sure the tag matches
    imap_cmd_cb_t *cb = CONTAINER_OF(link, imap_cmd_cb_t, link);
    if(dstr_cmp(&st->tag->dstr, &cb->tag->dstr) != 0){
        TRACE(&e, "got tag %x but expected %x\n",
                FD(&st->tag->dstr), FD(&cb->tag->dstr));
        ORIG(&e, E_RESPONSE, "bad status type response");
    }

    // do the callback
    link_remove(link);
    PROP_GO(&e, cb->call(cb, st), fail);

    cb->free(cb);

    PROP(&e, advance_state(fetcher) );

    return e;

fail:
    cb->free(cb);
    return e;
}

static derr_t untagged_status_type(fetcher_t *fetcher, const ie_st_resp_t *st){
    derr_t e = E_OK;
    switch(st->status){
        case IE_ST_OK:
            // informational message
            PROP(&e, untagged_ok(fetcher, st->code, &st->text->dstr) );
            break;
        case IE_ST_NO:
            // a warning about a command
            // TODO: handle this
            TRACE(&e, "unhandled * NO status message\n");
            ORIG(&e, E_INTERNAL, "unhandled message");
            break;
        case IE_ST_BAD:
            // an error not from a command, or not sure from which command
            // TODO: handle this
            TRACE(&e, "unhandled * BAD status message\n");
            ORIG(&e, E_INTERNAL, "unhandled message");
            break;
        case IE_ST_PREAUTH:
            // only allowed as a greeting
            // TODO: handle this
            TRACE(&e, "unhandled * PREAUTH status message\n");
            ORIG(&e, E_INTERNAL, "unhandled message");
            break;
        case IE_ST_BYE:
            // we are logging out or server is shutting down.
            // TODO: handle this
            TRACE(&e, "unhandled * BYE status message\n");
            ORIG(&e, E_INTERNAL, "unhandled message");
            break;
        default:
            TRACE(&e, "invalid status of unknown type %x\n", FU(st->status));
            ORIG(&e, E_INTERNAL, "bad imap parse");
    }

    return e;
}

static derr_t handle_response(fetcher_t *fetcher){
    derr_t e = E_OK;

    imap_resp_t *resp = fetcher->resp;
    imap_resp_arg_t *arg = &resp->arg;
    size_t got_tag;

    switch(resp->type){

        // status-type responses //

        case IMAP_RESP_STATUS_TYPE:
            // is it one of ours?
            if(match_prefix(resp, prefix, &got_tag)){
                // one of ours
                cmd_cb_t *cmd_cb =
                    CONTAINER_OF(fetcher->cbs.next, cmd_cb_t, link);
                if(!cmd_cb){
                    ORIG(&e, E_RESPONSE, "got unexpected fetcher response");
                }
                if(cmd_cb->tag != got_tag){
                    ORIG(&e,
                        E_RESPONSE,
                        "expected fetcher%x but got fetcher %x",
                        FU(cmd_cb->tag), FU(got_tag)
                    );
                }
                // matched our next tagged response
                PROP(&e, cmd_cb->cb(fetcher, &arg->st_resp) );
                cmd_cb_free(cmd_cb);
                break;
            }
            if(fetcher->up_active){
                // while up is active, we default to sending st_resp it
                PROP(&e, up_st_resp(&fetcher->up, arg->st_resp) );
                break;
            }
            if(match_info(resp)){
                // informational response
                LOG_INFO("informational response: %x\n", FIRESP(resp));
                break;
            }
            ORIG(&e, E_RESPONSE, "unexpected response: %x", cu, FIRESP(resp));
            break;

        // our responses //

        case IMAP_RESP_CAPA:
            PROP(&e, capa_resp(fetcher, arg->capa) );
            break;
        case IMAP_RESP_LIST:
            PROP(&e, list_resp(fetcher, arg->list) );
            break;
        case IMAP_RESP_LSUB:
            PROP(&e, lsub_resp(fetcher, arg->lsub) );
            break;
        case IMAP_RESP_STATUS:
            PROP(&e, status_resp(fetcher, arg->status) );
            break;
        case IMAP_RESP_ENABLED:
            PROP(&e, enabled_resp(fetcher, arg->enabled) );
            break;

        // up_t's responses //

        case IMAP_RESP_FETCH:
            PROP(&e, up_fetch_resp(&fetcher->up, arg->fetch) );
            break;

        case IMAP_RESP_VANISHED:
            PROP(&e, up_vanished_resp(&fetcher->up, arg->vanished) );
            break;

        case IMAP_RESP_EXISTS:
            PROP(&e, up_exists_resp(&fetcher->up, arg->exists) );
            break;

        case IMAP_RESP_PLUS:
            PROP(&e, up_plus_resp(&fetcher->up) );
            break;

        // ignored responses //

        case IMAP_RESP_FLAGS:
        case IMAP_RESP_RECENT:
            break;

        // disallowed responses //

        case IMAP_RESP_EXPUNGE:
        case IMAP_RESP_SEARCH:
        case IMAP_RESP_XKEYSYNC:
            ORIG(&e, E_INTERNAL, "invalid response: %x", FIRESP(resp));
    }

    return e;
}

// we either need to consume the resp or free it
static derr_t handle_one_response(fetcher_t *fetcher, imap_resp_t *resp){
    derr_t e = E_OK;

    const imap_resp_arg_t *arg = &resp->arg;

    switch(resp->type){
        case IMAP_RESP_STATUS_TYPE:
            // tagged responses are handled by callbacks
            if(arg->status_type->tag){
                PROP_GO(&e, tagged_status_type(fetcher, arg->status_type),
                        cu_resp);
            }else{
                PROP_GO(&e, untagged_status_type(fetcher, arg->status_type),
                        cu_resp);
            }
            break;
        case IMAP_RESP_CAPA:
            PROP_GO(&e, capa_resp(fetcher, arg->capa), cu_resp);
            break;
        case IMAP_RESP_LIST:
            PROP_GO(&e, list_resp(fetcher, arg->list), cu_resp);
            break;
        case IMAP_RESP_LSUB:
            PROP_GO(&e, lsub_resp(fetcher, arg->lsub), cu_resp);
            break;
        case IMAP_RESP_STATUS:
            PROP_GO(&e, status_resp(fetcher, arg->status), cu_resp);
            break;
        case IMAP_RESP_ENABLED:
            PROP_GO(&e, enabled_resp(fetcher, arg->enabled), cu_resp);
            break;

        case IMAP_RESP_PLUS:
        case IMAP_RESP_FLAGS:
        case IMAP_RESP_SEARCH:
        case IMAP_RESP_EXISTS:
        case IMAP_RESP_EXPUNGE:
        case IMAP_RESP_RECENT:
        case IMAP_RESP_FETCH:
        case IMAP_RESP_VANISHED:
        case IMAP_RESP_XKEYSYNC:
            ORIG_GO(&e, E_INTERNAL, "Invalid responses", cu_resp);
    }

cu_resp:
    imap_resp_free(resp);
    return e;
}

/* we can inject commands into the stream of commands requested by the up_t,
   so we have to have a way to sort the responses that come back that belong
   to the fetcher_t vs the responses that we need to forward to the up_t */
static bool fetcher_intercept_resp(fetcher_t *fetcher,
        const imap_resp_t *resp){
    (void)fetcher;
    const imap_resp_arg_t *arg = &resp->arg;

    switch(resp->type){
        case IMAP_RESP_STATUS_TYPE:
            if(arg->status_type->tag){
                // intercept tagged responses based on the tag
                DSTR_STATIC(prefix, "fetcher");
                dstr_t subtag = dstr_sub(
                    &arg->status_type->tag->dstr, 0, prefix.len
                );
                return dstr_cmp(&prefix, &subtag) == 0;
            }else{
                // TODO: are there any other cases to consider?
                return false;
            }
            break;
        case IMAP_RESP_CAPA:
        case IMAP_RESP_LIST:
        case IMAP_RESP_LSUB:
        case IMAP_RESP_STATUS:
        case IMAP_RESP_ENABLED:
            return true;

        case IMAP_RESP_PLUS:
        case IMAP_RESP_FLAGS:
        case IMAP_RESP_SEARCH:
        case IMAP_RESP_EXISTS:
        case IMAP_RESP_EXPUNGE:
        case IMAP_RESP_RECENT:
        case IMAP_RESP_FETCH:
        case IMAP_RESP_VANISHED:
        case IMAP_RESP_XKEYSYNC:
            return false;
    }
    return false;
}

static derr_t handle_all_responses(fetcher_t *fetcher){
    derr_t e = E_OK;

    link_t *link;

    // unhandled responses
    while((link = link_list_pop_first(&fetcher->unhandled_resps))){
        imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);

        // detect if we need to just pass the command to the up_t
        /* TODO: it seems there is a synchronization problem here... is it
                 possible to have more commands in flight that might not belong
                 to the up_t, but which might come over the wire after we have
                 created the up_t but before the select response comes in? */
        if(fetcher->up_active && !fetcher_intercept_resp(fetcher, resp)){
            PROP(&e, up_resp(&fetcher->up, resp) );
            continue;
        }

        PROP(&e, handle_one_response(fetcher, resp) );
    }

    return e;
}

static derr_t advance_passthru(fetcher_t *fetcher){
    derr_t e = E_OK;

    if(!fetcher->passthru.req) return e;

    bool ok;

    if(!fetcher->passthru.sent){
        /* make sure we are either disconnected from the mailbox or that we
           have blocked IDLE commands so it is safe to send passthru command */
        if(fetcher->up_active){
            PROP(&e, up_idle_block(&fetcher->up, &ok) );
            if(!ok) return e;
        }
        PROP(&e, send_passthru(fetcher) );
        fetcher->passthru.sent = true;
        // we only needed the idle_block for that moment
        PROP(&e, up_idle_unblock(&fetcher->up) );
    }

    // final steps happen in passthru_done callback

    return e;
}

static derr_t need_unselected(fetcher_t *fetcher, bool *ok){
    derr_t e = E_OK;

    if(!fetcher->up_active){
        *ok = true;
        return e;
    }

    PROP(&e, up_unselect(&fetcher->up, &fetcher->cmds, ok) );
    if(!*ok) return e;

    // done with our up_t
    dirmgr_close_up(&fetcher->up);
    fetcher->up_active = false;

    return e;
}

static derr_t advance_select(fetcher_t *fetcher){
    derr_t e = E_OK;

    if(!fetcher->select.needed) return e;

    bool ok;

    // make sure we have nothing selected first
    PROP(&e, need_unselected(fetcher, &ok) );
    if(!ok) return e;

    const dstr_t *dir_name = ie_mailbox_name(fetcher->select.mailbox);
    PROP(&e,
        dirmgr_open_up(
            &fetcher->dirmgr,
            dir_name,
            &fetcher->up,
            &fetcher->up_cb,
            &fetcher->c->exts
        )
    );
    fetcher->up_active = true;

    // tell the server_t we connected to the imaildir_t
    fetcher->cb->selected(fetcher->cb);

    fetcher_free_select(fetcher);

    return e;
}

static derr_t advance_close(fetcher_t *fetcher){
    derr_t e = E_OK;

    if(!fetcher->close.needed) return e;

    bool ok;

    PROP(&e, need_unselected(fetcher, &ok) );
    if(!ok) return e;

    fetcher->cb->unselected(fetcher->cb);
    fetcher_free_close(fetcher);

    return e;
}

static void advance_state(fetcher_t *fetcher){
    if(is_error(fetcher->e)) goto fail;
    if(fetcher->failed || fetcher->canceled) goto cu;

    // send one ENABLE command, and fail asynchronously if it fails
    ONCE(fetcher->enable_sent) PROP_GO(&e, send_enable(fetcher), fail);

    // read any incoming responses
    bool ok = advance_reads(fetcher);
    if(ok){
        PROP_GO(&e, handle_all_responses(fetcher), fail);
        // handled that response...
        imap_resp_free(&fetcher->resp);
        // ... get started reading another
        (void)advance_reads(fetcher);
    }

    /* these remaining operations originate with the server_t and the server_t
       must make sure only one can be active at a time */
    PROP_GO(&e, advance_passthru(fetcher), fail);
    PROP_GO(&e, advance_select(fetcher), fail);
    PROP_GO(&e, advance_close(fetcher), fail);

    // always let the up_t do work
    if(fetcher->up_active){
        PROP_GO(&e, up_advance_state(&fetcher->up, &fetcher->cmds), fail);
    }

    // write anything we can
    (void)advance_writes(fetcher);

    return e;

fail:
    fetcher->failed = true;

cu:
    // XXX: disconnect softer, so our up_t can finish relaying any cmds
    if(fetcher->up_active){
        // hard disconnect
        dirmgr_close_up(fetcher->dirmgr, &fetcher->up);
        fetcher->up_active = false;
    }
    imap_client_cancel(fetcher->c);
    if(!fetcher->c->awaited) return;

    schedulable_cancel(&fetcher->schedulable);

    derr_t e = fetcher->e;
    if(!is_error(e) && fetcher->canceled){
        e.type = E_CANCELED;
    }
    fetcher->awaited = true;

    // XXX: reconsider
    /* Note that we do not free ourselves before the await cb like a stream_i
       would because of the legacy pattern where the fetcher_cb_i might cause
       the sf_pair_i to pass an event back into our struct after we have
       exited.  The sf_pair/server/fetcher/up/dn were written with a two-step
       shutdown in mind: a synchronous dying event that cause everybody to stop
       calling callbacks, and a release event where cleanup was done.  The
       stream mechanics are cleaner, because there's just one event at a time,
       but since I'm not fully rewriting the sf_pair/server/fetcher/up/dn, to
       acommodate the fact that server/fetcher can trigger each other after
       one is totally done means that only the sf_pair can know when it is
       safe to free us.  Rewriting the sf_pair to only propagate callbacks in
       advance_state would be sufficient to solve this problem... I think... */

    fetcher->cb->done(fetcher->cb, e);
}
