#include "libcitm/libcitm.h"
#include "libimaildir/msg_internal.h"

DEF_CONTAINER_OF(sf_pair_t, schedulable, schedulable_t)
DEF_CONTAINER_OF(sf_pair_t, server_cb, server_cb_i)
DEF_CONTAINER_OF(sf_pair_t, fetcher_cb, fetcher_cb_i)

static void sf_pair_free_append(sf_pair_t *sf_pair){
    passthru_req_free(STEAL(passthru_req_t, &sf_pair->append.req));
    passthru_resp_free(STEAL(passthru_resp_t, &sf_pair->append.resp));
    dirmgr_hold_free(sf_pair->append.hold);
    sf_pair->append.hold = NULL;
    if(sf_pair->append.tmp_id){
        DSTR_VAR(file, 32);
        // this can't actually fail in practice
        DROP_CMD( FMT(&file, "%x", FU(sf_pair->append.tmp_id)) );
        dirmgr_t *dirmgr = sf_pair->kd->dirmgr(sf_pair->kd);
        string_builder_t tmp_path = sb_append(&dirmgr->path, FS("tmp"));
        string_builder_t path = sb_append(&tmp_path, FD(&file));
        DROP_CMD( remove_path(&path) );
        sf_pair->append.tmp_id = 0;
    }
    return;
}

static void sf_pair_free_status(sf_pair_t *sf_pair){
    passthru_resp_free(STEAL(passthru_resp_t, &sf_pair->status.resp));
    return;
}

void sf_pair_free(sf_pair_t **old){
    sf_pair_t *sf_pair = *old;
    if(!sf_pair) return;

    // free state generated at runtime
    sf_pair_free_append(sf_pair);
    sf_pair_free_status(sf_pair);

    server_free(&sf_pair->server);
    fetcher_free(&sf_pair->fetcher);

    schedulable_cancel(&sf_pair->schedulable);
    free(sf_pair);

    *old = NULL;
}

static void advance_state(sf_pair_t *sf_pair);

static void scheduled(schedulable_t *s){
    sf_pair_t *sf_pair = CONTAINER_OF(s, sf_pair_t, schedulable);
    advance_state(sf_pair);
}

static void schedule(sf_pair_t *sf_pair){
    if(sf_pair->awaited) return;
    sf_pair->scheduler->schedule(sf_pair->scheduler, &sf_pair->schedulable);
}

static void sf_pair_fail(sf_pair_t *sf_pair, derr_t e){
    if(sf_pair->failed){
        DROP_VAR(&e);
    }else if(sf_pair->canceled){
        DROP_CANCELED_VAR(&e);
    }else{
        UPGRADE_CANCELED_VAR(&e, E_INTERNAL);
    }
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&sf_pair->e, &e);
}

// we will modify the content of the append command directly
static derr_t sf_pair_append_req(sf_pair_t *sf_pair){
    derr_t e = E_OK;

    ie_append_cmd_t *append = sf_pair->append.req->arg.append;

    // step 1: write the unencrypted text to a file for saving
    dirmgr_t *dirmgr = sf_pair->kd->dirmgr(sf_pair->kd);
    sf_pair->append.tmp_id = dirmgr_new_tmp_id(dirmgr);
    DSTR_VAR(file, 32);
    PROP_GO(&e, FMT(&file, "%x", FU(sf_pair->append.tmp_id)), fail);
    string_builder_t tmp_path = sb_append(&dirmgr->path, FS("tmp"));
    string_builder_t path = sb_append(&tmp_path, FD(&file));
    PROP_GO(&e, dstr_write_path(&path, &append->content->dstr), fail);

    // step 2: copy some details from the APPEND command
    sf_pair->append.len = append->content->dstr.len;
    sf_pair->append.flags = msg_flags_from_flags(append->flags);
    if(append->time.year){
        // an explicit intdate was passed in
        sf_pair->append.intdate = append->time;
    }else{
        // use the time right now
        sf_pair->append.intdate = imap_time_now((time_t)-1);
        // also pass that value to the server to ensure that we are synced
        append->time = sf_pair->append.intdate;
    }

    // step 3: start a hold on the mailbox
    PROP_GO(&e,
        dirmgr_hold_new(
            dirmgr, ie_mailbox_name(append->m), &sf_pair->append.hold
        ),
    fail);

    // step 4: encrypt the text to all the keys we know of
    ie_dstr_t *content = ie_dstr_new_empty(&e);
    CHECK_GO(&e, fail);

    encrypter_t ec;
    PROP_GO(&e, encrypter_new(&ec), cu_content);
    link_t *all_keys = sf_pair->kd->all_keys(sf_pair->kd);
    PROP_GO(&e, encrypter_start(&ec, all_keys, &content->dstr), cu_ec);
    PROP_GO(&e,
        encrypter_update(&ec, &append->content->dstr, &content->dstr),
    cu_ec);
    PROP_GO(&e, encrypter_finish(&ec, &content->dstr), cu_ec);

    // step 5: modify the APPEND and relay it to the fetcher
    ie_dstr_free(append->content);
    append->content = STEAL(ie_dstr_t, &content);
    fetcher_passthru_req(
        &sf_pair->fetcher, STEAL(passthru_req_t, &sf_pair->append.req)
    );

cu_ec:
    encrypter_free(&ec);
cu_content:
    ie_dstr_free(content);

fail:
    if(is_error(e)) sf_pair_free_append(sf_pair);

    return e;
}

static derr_t sf_pair_append_resp(sf_pair_t *sf_pair){
    derr_t e = E_OK;
    imaildir_t *m = NULL;

    const ie_st_resp_t *st_resp = sf_pair->append.resp->st_resp;

    if(st_resp->status != IE_ST_OK){
        // just relay and cleanup
        goto relay;
    }

    // snag the uid from the APPENDUID status code
    if(st_resp->code->type != IE_ST_CODE_APPENDUID){
        ORIG_GO(&e, E_RESPONSE, "expected APPENDUID in APPEND response", cu);
    }
    unsigned int uidvld_up = st_resp->code->arg.appenduid.uidvld;
    unsigned int uid_up = st_resp->code->arg.appenduid.uid;

    // get the path to the temporary file
    DSTR_VAR(file, 32);
    PROP_GO(&e, FMT(&file, "%x", FU(sf_pair->append.tmp_id)), cu);
    dirmgr_t *dirmgr = sf_pair->kd->dirmgr(sf_pair->kd);
    string_builder_t tmp_path = sb_append(&dirmgr->path, FS("tmp"));
    string_builder_t path = sb_append(&tmp_path, FD(&file));

    // get the imaildir we would add this file to
    PROP_GO(&e,
        dirmgr_hold_get_imaildir(sf_pair->append.hold, &m),
    cu);

    if(uidvld_up != imaildir_get_uidvld_up(m)){
        // imaildir's uidvld is out-of-date, just delete the temp file
        LOG_WARN("detected APPEND with mismatched UIDVALIDITY\n");
        DROP_CMD( remove_path(&path) );
    }else{
        // add the temp file to the maildir
        void *up_noresync = &sf_pair->fetcher.up;
        PROP_GO(&e,
            imaildir_add_local_file(
                m,
                &path,
                uid_up,
                sf_pair->append.len,
                sf_pair->append.intdate,
                sf_pair->append.flags,
                up_noresync,
                NULL
            ),
        cu);
    }

    // done with temporary file
    sf_pair->append.tmp_id = 0;

relay:
    server_passthru_resp(
        &sf_pair->server, STEAL(passthru_resp_t, &sf_pair->append.resp)
    );

cu:
    dirmgr_hold_release_imaildir(sf_pair->append.hold, &m);
    sf_pair_free_append(sf_pair);

    return e;
}

static derr_t sf_pair_status_resp(sf_pair_t *sf_pair){
    derr_t e = E_OK;

    const ie_st_resp_t *st_resp = sf_pair->status.resp->st_resp;

    if(st_resp->status != IE_ST_OK){
        // just relay and cleanup
        goto relay;
    }

    // post-process the STATUS response; the server's values will be wrong
    const ie_status_resp_t *resp = sf_pair->status.resp->arg.status;
    const dstr_t *name = ie_mailbox_name(resp->m);
    ie_status_attr_resp_t new;
    dirmgr_t *dirmgr = sf_pair->kd->dirmgr(sf_pair->kd);
    PROP_GO(&e, dirmgr_process_status_resp(dirmgr, name, resp->sa, &new), cu);

    // modify the response before relaying it downwards
    sf_pair->status.resp->arg.status->sa = new;

relay:
    server_passthru_resp(
        &sf_pair->server, STEAL(passthru_resp_t, &sf_pair->status.resp)
    );

cu:
    sf_pair_free_status(sf_pair);

    return e;
}

static void advance_state(sf_pair_t *sf_pair){
    if(is_error(sf_pair->e)) goto fail;
    if(sf_pair->canceled || sf_pair->failed) goto cu;

    if(sf_pair->append.req){
        PROP_GO(&sf_pair->e, sf_pair_append_req(sf_pair), fail);
    }
    if(sf_pair->append.resp){
        PROP_GO(&sf_pair->e, sf_pair_append_resp(sf_pair), fail);
    }
    if(sf_pair->status.resp){
        PROP_GO(&sf_pair->e, sf_pair_status_resp(sf_pair), fail);
    }
    return;

fail:
    // XXX: tell client?
    sf_pair->failed = true;
    DUMP(sf_pair->e);
    DROP_VAR(&sf_pair->e);

cu:
    server_cancel(&sf_pair->server);
    fetcher_cancel(&sf_pair->fetcher);
    if(!sf_pair->server.awaited) return;
    if(!sf_pair->fetcher.awaited) return;

    schedulable_cancel(&sf_pair->schedulable);
    sf_pair->awaited = true;
    sf_pair->cb(sf_pair, sf_pair->cb_data);
    return;
}

// part of server_cb_i
static void server_cb_done(server_cb_i *cb, derr_t e){
    // printf("---- server_cb_done\n");
    sf_pair_t *sf_pair = CONTAINER_OF(cb, sf_pair_t, server_cb);
    TRACE_PROP(&e);
    sf_pair_fail(sf_pair, e);
    // XXX: what if error is none, and we don't shut down?
    schedule(sf_pair);
}

// part of server_cb_i
static void server_cb_passthru_req(
    server_cb_i *server_cb, passthru_req_t *passthru_req
){
    // printf("---- server_cb_passthru_req\n");
    sf_pair_t *sf_pair = CONTAINER_OF(server_cb, sf_pair_t, server_cb);
    if(passthru_req->type == PASSTHRU_APPEND){
        // intercept APPEND requests, and process them asynchronously
        sf_pair->append.req = passthru_req;
        schedule(sf_pair);
        return;
    }
    fetcher_passthru_req(&sf_pair->fetcher, passthru_req);
}

// part of server_cb_i
static void server_cb_select(
    server_cb_i *server_cb, ie_mailbox_t *m, bool examine
){
    // printf("---- server_cb_select\n");
    sf_pair_t *sf_pair = CONTAINER_OF(server_cb, sf_pair_t, server_cb);
    fetcher_select(&sf_pair->fetcher, m, examine);
}

// part of server_cb_i
static void server_cb_unselect(server_cb_i *server_cb){
    // printf("---- server_cb_unselect\n");
    sf_pair_t *sf_pair = CONTAINER_OF(server_cb, sf_pair_t, server_cb);
    fetcher_unselect(&sf_pair->fetcher);
}

// part of fetcher_cb_i
static void fetcher_cb_done(fetcher_cb_i *cb, derr_t e){
    // printf("---- fetcher_cb_done\n");
    sf_pair_t *sf_pair = CONTAINER_OF(cb, sf_pair_t, fetcher_cb);
    TRACE_PROP(&e);
    sf_pair_fail(sf_pair, e);
    // XXX: what if error is none, and we don't shut down?
    schedule(sf_pair);
}

// part of fetcher_cb_i
static void fetcher_cb_passthru_resp(
    fetcher_cb_i *fetcher_cb, passthru_resp_t *passthru_resp
){
    // printf("---- fetcher_cb_passthru_resp\n");
    sf_pair_t *sf_pair = CONTAINER_OF(fetcher_cb, sf_pair_t, fetcher_cb);
    if(passthru_resp->type == PASSTHRU_APPEND){
        // intercept APPEND responses and process them asynchronously
        sf_pair->append.resp = passthru_resp;
        schedule(sf_pair);
        return;
    }
    if(passthru_resp->type == PASSTHRU_STATUS){
        // intercept STATUS responses and process them asynchronously
        sf_pair->status.resp = passthru_resp;
        schedule(sf_pair);
        return;
    }
    server_passthru_resp(&sf_pair->server, passthru_resp);
}

// part of the fetcher_cb_i
static void fetcher_cb_selected(fetcher_cb_i *fetcher_cb){
    // printf("---- fetcher_cb_selected\n");
    sf_pair_t *sf_pair = CONTAINER_OF(fetcher_cb, sf_pair_t, fetcher_cb);
    server_selected(&sf_pair->server);
}

// part of the fetcher_cb_i
static void fetcher_cb_unselected(fetcher_cb_i *fetcher_cb){
    // printf("---- fetcher_cb_unselected\n");
    sf_pair_t *sf_pair = CONTAINER_OF(fetcher_cb, sf_pair_t, fetcher_cb);
    server_unselected(&sf_pair->server);
}

derr_t sf_pair_new(
    scheduler_i *scheduler,
    keydir_i *kd,
    imap_server_t *s,
    imap_client_t *c,
    sf_pair_cb cb,
    void *data,
    sf_pair_t **out
){
    derr_t e = E_OK;

    *out = NULL;

    sf_pair_t *sf_pair = DMALLOC_STRUCT_PTR(&e, sf_pair);
    CHECK_GO(&e, fail);

    *sf_pair = (sf_pair_t){
        .scheduler = scheduler,
        .kd = kd,
        .cb = cb,
        .cb_data = data,
        .server_cb = {
            .done = server_cb_done,
            .passthru_req = server_cb_passthru_req,
            .select = server_cb_select,
            .unselect = server_cb_unselect,
        },
        .fetcher_cb = {
            .done = fetcher_cb_done,
            .passthru_resp = fetcher_cb_passthru_resp,
            .selected = fetcher_cb_selected,
            .unselected = fetcher_cb_unselected,
        },
    };

    server_prep(
        &sf_pair->server,
        scheduler,
        STEAL(imap_server_t, &s),
        sf_pair->kd->dirmgr(sf_pair->kd),
        &sf_pair->server_cb
    );

    fetcher_prep(
        &sf_pair->fetcher,
        scheduler,
        STEAL(imap_client_t, &c),
        sf_pair->kd->dirmgr(sf_pair->kd),
        &sf_pair->fetcher_cb
    );

    schedulable_prep(&sf_pair->schedulable, scheduled);

    *out = sf_pair;

    return e;

fail:
    imap_server_free(&s);
    imap_client_free(&c);
    if(sf_pair) free(sf_pair);
    return e;
}

void sf_pair_start(sf_pair_t *sf_pair){
    server_start(&sf_pair->server);
    fetcher_start(&sf_pair->fetcher);
}

void sf_pair_cancel(sf_pair_t *sf_pair){
    sf_pair->canceled = true;
    schedule(sf_pair);
}
