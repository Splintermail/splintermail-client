typedef struct {
    duv_scheduler_t *scheduler;
    schedulable_t schedulable;

    imap_server_t *imap_dn;
    imap_client_t *imap_up;

    anon_cb cb;
    void *cb_data;

    link_t link;  // citm->anons

    dstr_t user;
    dstr_t pass;

    // state
    bool writing_up : 1;
    bool writing_dn : 1;
    bool reading_up : 1;
    bool reading_dn : 1;

    bool greet_up_read : 1;
    bool capa_up_sent : 1;
    bool capa_up_checked : 1;

    bool greet_dn_sent : 1;
    bool greet_dn_done : 1;

    bool ready_up : 1;
    bool ready_dn : 1;

    bool login_ready : 1;
    bool login_read_dn : 1;
    bool login_write_up : 1;
    bool login_read_up : 1;
    bool login_write_dn : 1;
    bool login_success : 1;
    bool login_reset : 1;

    bool canceled : 1;
    bool imap_dn_awaited : 1;
    bool imap_up_awaited : 1;
} anon_t;
DEF_CONTAINER_OF(anon_t, link, link_t)
DEF_CONTAINER_OF(anon_t, schedulable, schedulable_t)

static void scheduled(schedulable_t *s){
    anon_t *anon = CONTAINER_OF(s, anon_t, schedulable);
}

static void schedule(anon_t *anon){
    anon->scheduler->schedule(anon->scheduler, &anon->schedulable);
}

static void anon_free(anon_t *anon){
    schedulable_cancel(&anon->schedulable);
    link_remove(&anon->link);
    duv_imap_free(anon->imap_dn);
    duv_imap_free(anon->imap_up);
    dstr_free(&anon->user);
    dstr_free(&anon->pass);
    dstr_free(anon);
}

//#define ONCE(x) for(; !x ; x = true)
#define ONCE(x) if(!x && (x = true))

static void advance_state(anon_t *anon){
    derr_t e = E_OK;

    if(anon->canceled) goto fail;

    // prelogin phase
    if(!anon->prelogin_ready){

        // upwards prelogin logic in parallel
        do {
            // receive upwards greeting
            if(!anon->greet_up_recvd){
                ONCE(anon->greet_up_read) read_up(anon);
                if(anon->reading_up) break;
                // XXX: check the greeting we recvd
                anon->greet_up_read = true;
            }
            // explicitly ask upwards for capabilities if we didn't see them
            if(!anon->capa_up_checked){
                ONCE(anon->capa_up_written) write_capa_up(anon);
                ONCE(anon->capa_up_read) read_up(anon);
                if(anon->reading_up) break;
                // XXX: check capabilities
                anon->capa_up_checked;
            }
        } while(0);
        // downwards prelogin logic in parallel
        do {
            if(!anon->prelogin_dn_ready){
                ONCE(anon->greet_dn_sent) write_greet_dn(anon);
                if(anon->writing_dn) break;
            }
            anon->prelogin_dn_ready = true;
        } while(0);
        // wait for both upwards and downwards prelogin logic to finish
        if(!anon->capa_up_checked || !anon->prelogin_dn_ready) return;
        anon->prelogin_ready = true;
    }

    // allowed prelogin commands:
    //   - ERROR,
    //   - PLUS_REQ,
    //   - NOOP
    //   - CAPABILITY
    //   - LOGOUT
    //   - LOGIN

    // login phase, synchronous
    while(!anon->login_ready){
        if(anon->login_reset){
            // reset login logic
            anon->login_read_dn = false;
            anon->login_write_up = false;
            anon->login_read_up = false;
            anon->login_write_dn = false;
            anon->login_success = false;
            anon->login_reset = false;
        }
        // read LOGIN command from dn
        ONCE(anon->login_read_dn) read_dn(anon);
        if(anon->reading_dn) return;
        // XXX check LOGIN command
        if(anon->login_reset) continue;
        // parrot LOGIN command up
        ONCE(anon->login_write_up) write_login_up(anon);
        if(anon->writing_up) return;
        // read response to LOGIN command
        ONCE(anon->login_read_up) read_up(anon);
        if(anon->reading_up) return;
        // XXX check login response
        if(!anon->login_success){
            ONCE(anon->login_write_dn) write_login_dn_fail(anon);
            if(anon->writing_dn) return;
            login_reset = true;
        }else{
            ONCE(anon->login_write_dn) write_login_dn_pass(anon);
            if(anon->writing_dn) return;
            anon->login_ready = true;
        }
    }

    // success!
    anon_cb cb = anon->cb;
    void *cb_data = anon->cb_data;
    duv_imap_t *imap_dn = STEAL(duv_imap_t, &anon->imap_up);
    duv_imap_t *imap_up = STEAL(duv_imap_t, &anon->imap_dn);
    dstr_t user = STEAL(dstr_t, &anon->user);
    dstr_t pass = STEAL(dstr_t, &anon->pass);
    anon_free(anon);
    anon_cb(cb_data, imap_dn, imap_up, user, pass);

    return;

fail:
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
    }

    duv_imap_cancel(anon->imap_up);
    duv_imap_cancel(anon->imap_dn);

    // wait for our async resources to finish
    if(!anon->imap_dn_awaited) return;
    if(!anon->imap_up_awaited) return;

    anon_free(anon);
}

void anon_new(
    duv_scheduler_t *scheduler,
    citm_conn_t conn_dn,
    citm_conn_t conn_up,
    void *data,
    io_pair_cb cb,
    link_t *list
){
    derr_t e = E_OK;

    duv_imap_t *imap_dn = NULL;
    duv_imap_t *imap_up = NULL;

    anon_t *anon = DMALLOC_STRUCT_PTR(&e, anon);
    CHECK_GO(&e, fail);

    PROP_GO(&e,
        duv_imap_server_new(
            &imap_dn,
            scheduler,
            STEAL(citm_conn_t, &conn_dn),
            anon_imap_cmd_cb,
            anon
        ),
    fail);

    PROP_GO(&e,
        duv_imap_client_new(
            &imap_up,
            scheduler,
            STEAL(citm_conn_t, &conn_up),
            anon_imap_resp_cb,
            anon
        ),
    fail);

    *anon = (anon_t){
        .imap_dn = imap_dn,
        .imap_up = imap_up,
        .scheduler = scheduler,

    };

    schedulable_prep(&anon->schedulable, scheduled);

    // success!

    schedule(anon);
    link_list_append(list, &anon->link);

    return;

fail:
    citm_conn_free(conn_dn);
    citm_conn_free(conn_up);
    duv_imap_free(imap_dn);
    duv_imap_free(imap_up);
    if(anon) free(anon);
    // XXX free things
    DUMP(e);
    DROP_VAR(&e);
}

// citm can cancel us (it should also remove us from its list)
void anon_cancel(link_t *link){
    anon_t *anon = CONTAINER_OF(link, anon_t, link);
    anon->canceled = true;
    schedule(anon);
}
