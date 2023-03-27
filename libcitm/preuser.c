struct preuser_t {
    hash_elem_t elem;

    dstr_t user;
    dstr_t pass;
    citm_connect_i *connect;
    citm_client_t *conn;
    imap_client_t *xkey_client;
    link_t servers;  // imap_server_t->link
    link_t clients;  // imap_client_t->link

    xkey_i *xkey;

    bool
};

static void advance_state(preuser_t *p){
    derr_t e = E_OK;

    // wait to ready our imap client
    if(!p->imap){
        // wait for connect cb
        if(!p->conn) return;
        // configure our imap client
        // XXX: does imap_client own conn immediately after init?
        PROP_GO(&e,
            imap_client_new(&p->xkey_client, p->scheduler, &p->conn),
        cu);
    }

    // aggressively pipeline commands to reduce user-facing startup times

    if(!p->initial_sends_done){
        ONCE(p->initial_sends_started){
            PROP_GO(&e, send_login(p), cu);
            PROP_GO(&e, send_sync(p), cu);
            PROP_GO(&e, send_done(p), cu);
        }
        PROP_GO(&e, advance_writes(p, &ok), cu);
        if(!ok) return;
        p->initial_sends_done = true;
    }

    if(!p->login_done){
        PROP_GO(&e, advance_reads(p, &ok), cu);
        if(!ok) return;
        PROP_GO(&e, check_login_resp(p, &ok), cu);
        if(!ok) return;
        p->login_done = true;
    }

    if(!p->sync_done){
        PROP_GO(&e, advance_reads(p, &ok), cu);
        if(!ok) return;
        PROP_GO(&e, check_sync_resp(p, &ok), cu);
        if(!ok) return;
        p->sync_done = true;
    }

    // handle the case where mykey was not already present

    if(p->need_mykey && !p->mykey_done){
        ONCE(p->mykey_sent){
            PROP_GO(&e, send_mykey(p), cu);
        }
        PROP_GO(&e, advance_writes(p, &ok), cu);
        if(!ok) return;
        PROP_GO(&e, check_upload_resp(p, &ok), cu);
        p->mykey_done = true;
    }

    // XXX: are we guaranteed to have no pending reads/writes?
    // XXX: what if we read extra responses?  Can we put them back?

    // preuser_t's job is now complete
    preuser_cb cb = p->cb;
    void *data = p->data;
    dstr_t user = p->user;
    dstr_erase(&p->pass);
    dstr_free(&p->pass);
    link_t servers = {0};
    link_t clients = {0};
    link_list_append_list(&servers, &p->servers);
    link_list_append_list(&clients, &p->clients);
    keydir_i *kd = p->kd;
    imap_client_t *xkey_client = p->xkey_client;

    hash_elem_remove(&p->elem);
    schedulable_cancel(&p->schedulable);
    free(p);

    cb(data, user, &servers, &clients, kd, xkey_client);

    return;

cu:
    // XXX: we need to await all read/write callbacks first!

    dstr_free(&p->user);
    dstr_erase(&p->pass);
    dstr_free(&p->pass);
    link_t *link;
    while((link = link_list_pop_first(&p->servers))){
        imap_server_t *server = CONTAINER_OF(link, imap_server_t, link);
        imap_server_free(server);
    }
    while((link = link_list_pop_first(&p->clients))){
        imap_client_t *client = CONTAINER_OF(link, imap_client_t, link);
        imap_client_free(client);
    }
    p->kd->free(p->kd);
    imap_client_free(p->xkey_client);

    hash_elem_remove(&p->elem);
    schedulable_cancel(&p->schedulable);
    free(p);
}

void preuser_new(
    citm_io_t *io,
    dstr_t user,
    dstr_t pass,
    keydir_i *kd,
    imap_server_t *server,
    imap_server_t *client,
    preuser_cb cb,
    void *data,
    hashmap_t *out
);

// when another connection pair is ready but our keysync isn't ready yet
void preuser_add_pair(hash_elem_t *elem, imap_server_t *s, imap_client_t *c);

// elem should already have been removed
void preuser_cancel(hash_elem_t *elem);
