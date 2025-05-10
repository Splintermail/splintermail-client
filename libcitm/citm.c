#include <signal.h>
#include <string.h>

#include "libcitm.h"

bool imap_scheme_parse(dstr_t scheme, imap_security_e *out){
    if(dstr_ieq(scheme, DSTR_LIT("insecure"))){
        *out = IMAP_SEC_INSECURE;
        return true;
    }
    if(dstr_ieq(scheme, DSTR_LIT("starttls"))){
        *out = IMAP_SEC_STARTTLS;
        return true;
    }
    if(dstr_ieq(scheme, DSTR_LIT("tls"))){
        *out = IMAP_SEC_TLS;
        return true;
    }
    *out = 0;
    return false;
}

static void generic_await_cb(
    derr_t e, link_t *reads, link_t *writes, char *kind
){
    if(!link_list_isempty(reads)){
        LOG_FATAL("%x closed with pending reads\n", FS(kind));
    }
    if(!link_list_isempty(writes)){
        LOG_FATAL("%x closed with pending writes\n", FS(kind));
    }
    DROP_CANCELED_VAR(&e);
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
    }
}

static void conn_await_cb(
    stream_i *s, derr_t e, link_t *reads, link_t *writes
){
    citm_conn_t *conn = s->data;
    generic_await_cb(e, reads, writes, "conn");
    link_remove(&conn->link);
    conn->free(conn);
}

static void sawait_cb(
    imap_server_t *s, derr_t e, link_t *reads, link_t *writes
){
    generic_await_cb(e, reads, writes, "imap_server");
    link_remove(&s->link);
    imap_server_free(&s);
}

static void cawait_cb(
    imap_client_t *c, derr_t e, link_t *reads, link_t *writes
){
    generic_await_cb(e, reads, writes, "imap_client");
    link_remove(&c->link);
    imap_client_free(&c);
}

static void citm_close_conn(citm_t *citm, citm_conn_t *conn){
    if(!conn) return;
    if(conn->stream->awaited){
        conn->free(conn);
        return;
    }
    conn->stream->data = conn;
    conn->stream->await(conn->stream, conn_await_cb);
    conn->stream->cancel(conn->stream);
    link_list_append(&citm->closing.conns, &conn->link);
}

static void citm_close_server(citm_t *citm, imap_server_t *s){
    if(!s) return;
    if(s->awaited){
        imap_server_free(&s);
        return;
    }
    imap_server_must_await(s, sawait_cb, NULL);
    imap_server_cancel(s, false);
    link_list_append(&citm->closing.servers, &s->link);
}

static void citm_close_client(citm_t *citm, imap_client_t *c){
    if(!c) return;
    if(c->awaited){
        imap_client_free(&c);
        return;
    }
    imap_client_must_await(c, cawait_cb, NULL);
    imap_client_cancel(c);
    link_list_append(&citm->closing.clients, &c->link);
}

static void citm_close_server_list(citm_t *citm, link_t *list){
    link_t *link;
    while((link = link_list_pop_first(list))){
        imap_server_t *s = CONTAINER_OF(link, imap_server_t, link);
        citm_close_server(citm, s);
    }
}

static void citm_close_client_list(citm_t *citm, link_t *list){
    link_t *link;
    while((link = link_list_pop_first(list))){
        imap_client_t *c = CONTAINER_OF(link, imap_client_t, link);
        citm_close_client(citm, c);
    }
}


typedef struct {
    dstr_t user;
    dstr_t pass;
    link_t servers;
    link_t clients;
    hash_elem_t elem;  // citm_t->holds
} citm_hold_t;
DEF_CONTAINER_OF(citm_hold_t, elem, hash_elem_t)

static void hold_new(
    citm_t *citm,
    imap_server_t *s,
    imap_client_t *c,
    dstr_t user,
    dstr_t pass,
    hashmap_t *out
){
    derr_t e = E_OK;

    citm_hold_t *hold = DMALLOC_STRUCT_PTR(&e, hold);
    CHECK_GO(&e, fail);

    *hold = (citm_hold_t){
        .user = user,
        .pass = pass,
    };
    link_list_append(&hold->servers, &s->link);
    link_list_append(&hold->clients, &c->link);
    hash_elem_t *old = hashmap_sets(out, &hold->user, &hold->elem);
    if(old) LOG_FATAL("hold_new found pre-existing hold\n");
    return;

fail:
    DUMP(e);
    DROP_VAR(&e);
    citm_close_server(citm, s);
    citm_close_client(citm, c);
}

static void hold_cancel(citm_t *citm, hash_elem_t *elem){
    citm_hold_t *hold = CONTAINER_OF(elem, citm_hold_t, elem);
    hash_elem_remove(&hold->elem);
    dstr_free(&hold->user);
    dstr_free0(&hold->pass);
    citm_close_server_list(citm, &hold->servers);
    citm_close_client_list(citm, &hold->servers);
    free(hold);
}

static bool hold_pop(
    hashmap_t *h,
    const dstr_t *constuser,
    link_t *servers,
    link_t *clients,
    dstr_t *user,
    dstr_t *pass
){
    hash_elem_t *elem = hashmap_gets(h, constuser);
    if(!elem) return false;

    citm_hold_t *hold = CONTAINER_OF(elem, citm_hold_t, elem);
    link_list_append_list(servers, &hold->servers);
    link_list_append_list(clients, &hold->clients);
    *user = hold->user;
    *pass = hold->pass;
    free(hold);
    return true;
}

static void hold_add_pair(
    hash_elem_t *elem, imap_server_t *s, imap_client_t *c
){
    citm_hold_t *hold = CONTAINER_OF(elem, citm_hold_t, elem);
    link_list_append(&hold->servers, &s->link);
    link_list_append(&hold->clients, &c->link);
}

derr_t citm_init(
    citm_t *citm,
    citm_io_i *io,
    scheduler_i *scheduler,
    string_builder_t root
){
    derr_t e = E_OK;

    *citm = (citm_t){ .io = io, .scheduler = scheduler, .root = root };

    PROP_GO(&e, hashmap_init(&citm->preusers), fail);
    PROP_GO(&e, hashmap_init(&citm->users), fail);
    PROP_GO(&e, hashmap_init(&citm->holds), fail);

    return e;

fail:
    citm_free(citm);
    return e;
}

void citm_free(citm_t *citm){
    hashmap_free(&citm->preusers);
    hashmap_free(&citm->users);
    hashmap_free(&citm->holds);
    *citm = (citm_t){0};
}

void citm_cancel(citm_t *citm){
    link_t *link;
    hash_elem_t *elem;
    hashmap_trav_t trav;
    FOR_EACH_LINK(citm->io_pairs){
        io_pair_cancel(link);
    }
    FOR_EACH_LINK(citm->anons){
        anon_cancel(link);
    }
    FOR_EACH_ELEM(citm->preusers){
        preuser_cancel(elem);
    }
    FOR_EACH_ELEM(citm->users){
        user_cancel(elem);
    }
    FOR_EACH_ELEM(citm->holds){
        hold_cancel(citm, elem);
    }
}

static void citm_preuser_cb(
    void *data,
    derr_t e,
    dstr_t user,
    link_t *servers,
    link_t *clients,
    keydir_i *kd,
    imap_client_t *xc
);

// after user closes, late server/client pairs become new preusers
static void citm_user_cb(void *data, const dstr_t *constuser){
    derr_t e = E_OK;

    citm_t *citm = data;

    if(citm->canceled) return;

    link_t servers = {0};
    link_t clients = {0};
    dstr_t user = {0};
    dstr_t pass = {0};
    keydir_i *kd = NULL;

    bool ok = hold_pop(
        &citm->holds, constuser, &servers, &clients, &user, &pass
    );
    if(!ok) return;

    PROP_GO(&e, keydir_new(&citm->root, user, &kd), fail);

    PROP_GO(&e,
        preuser_new(
            citm->scheduler,
            citm->io,
            user,
            pass,
            kd,
            CONTAINER_OF(servers.next, imap_server_t, link),
            CONTAINER_OF(clients.next, imap_client_t, link),
            citm_preuser_cb,
            citm,
            &citm->preusers
        ),
    fail);

    // consumed first server and client
    (void)link_list_pop_first(&servers);
    (void)link_list_pop_first(&clients);

    // get the elem if preuser_new succeeded
    hash_elem_t *elem = hashmap_gets(&citm->preusers, &user);
    if(!elem) LOG_FATAL("unable to find newly created preuser\n");

    // put all the s/c pairs we have into this preuser
    while(!link_list_isempty(&servers)){
        preuser_add_pair(
            elem,
            CONTAINER_OF(link_list_pop_first(&servers), imap_server_t, link),
            CONTAINER_OF(link_list_pop_first(&clients), imap_client_t, link)
        );
    }

    return;

fail:
    DUMP(e);
    DROP_VAR(&e);
    dstr_free(&user);
    dstr_free0(&pass);
    if(kd) kd->free(kd);
    citm_close_server_list(citm, &servers);
    citm_close_client_list(citm, &clients);
    return;
}

static bool is_broken_conn(derr_t e){
    return e.type == E_UV_ECONNRESET
        || e.type == E_UV_EPIPE
        || e.type == E_CONN;
}

// completed preusers transition to new users
static void citm_preuser_cb(
    void *data,
    derr_t e,
    dstr_t user,
    link_t *servers,
    link_t *clients,
    keydir_i *kd,
    imap_client_t *xc
){
    citm_t *citm = data;

    if(e.type == E_CANCELED) goto free;

    // don't pollute logs with broken connections
    if(is_broken_conn(e)){
        DUMP_DEBUG(e);
        goto free;
    }

    if(is_error(e)) goto fail;

    if(citm->canceled) goto free;

    PROP_GO(&e, keydir_keysync_completed(kd), fail);

    PROP_GO(&e,
        user_new(
            citm->scheduler,
            user,
            servers,
            clients,
            kd,
            xc,
            citm_user_cb,
            citm,
            &citm->users
        ),
    fail);

    return;

fail:
    DUMP(e);
free:
    DROP_VAR(&e);
    citm_close_server_list(citm, servers);
    citm_close_client_list(citm, clients);
    kd->free(kd);
    dstr_free(&user);
    citm_close_client(citm, xc);
}

// completed anons attach to new or existing preuser, or an existing user
static void citm_anon_cb(
    void *data,
    derr_t e,
    imap_server_t *s,
    imap_client_t *c,
    dstr_t user,
    dstr_t pass
){
    citm_t *citm = data;
    keydir_i *kd = NULL;

    if(e.type == E_CANCELED) goto free;

    // don't pollute logs with broken connections
    if(is_broken_conn(e)){
        DUMP_DEBUG(e);
        goto free;
    }

    if(is_error(e)) goto fail;

    if(citm->canceled || !s || !c) goto free;

    // check for existing user
    hash_elem_t *elem = hashmap_gets(&citm->users, &user);
    if(elem){
        // try attaching to existing user
        bool ok;
        PROP_GO(&e, user_add_pair(elem, s, c, &ok), fail);
        if(ok) goto discard_user_pass;
        // user is shutting down, this server/client pair must wait
        elem = hashmap_gets(&citm->holds, &user);
        if(elem){
            // attach to existing hold
            hold_add_pair(elem, s, c);
            goto discard_user_pass;
        }
        // start a new hold
        hold_new(citm, s, c, user, pass, &citm->holds);
        return;
    }

    // check for existing preuser
    elem = hashmap_gets(&citm->preusers, &user);
    if(elem){
        // attach to existing preuser
        preuser_add_pair(elem, s, c);
        goto discard_user_pass;
    }

    // create a new preuser
    PROP_GO(&e, keydir_new(&citm->root, user, &kd), fail);

    PROP_GO(&e,
        preuser_new(
            citm->scheduler,
            citm->io,
            user,
            pass,
            kd,
            s,
            c,
            citm_preuser_cb,
            citm,
            &citm->preusers
        ),
    fail);

    return;

fail:
    DUMP(e);

free:
    DROP_VAR(&e);
    citm_close_server(citm, s);
    citm_close_client(citm, c);
    if(kd) kd->free(kd);

discard_user_pass:
    dstr_free(&user);
    dstr_zeroize(&pass);
    dstr_free(&pass);
    return;
}

// completed io_pairs become anon's until login is complete
static void citm_io_pair_cb(
    void *data, derr_t e, citm_conn_t *conn_dn, citm_conn_t *conn_up
){
    imap_client_t *c = NULL;
    imap_server_t *s = NULL;

    citm_t *citm = data;

    if(e.type == E_CANCELED) goto free;

    // don't pollute logs with broken connections
    if(is_broken_conn(e)){
        DUMP_DEBUG(e);
        goto free;
    }

    if(is_error(e)) goto fail;

    if(citm->canceled) goto free;

    /* convert conn's to imap's here so anon_new can easily fulfill the "no
       args are consumed on failure" promise */

    PROP_GO(&e, imap_server_new(&s, citm->scheduler, conn_dn), fail);
    conn_dn = NULL;

    PROP_GO(&e, imap_client_new(&c, citm->scheduler, conn_up), fail);
    conn_up = NULL;

    PROP_GO(&e,
        anon_new(citm->scheduler, s, c, citm_anon_cb, citm, &citm->anons),
    fail);

    return;

fail:
    DUMP(e);
free:
    DROP_VAR(&e);
    citm_close_conn(citm, conn_dn);
    citm_close_conn(citm, conn_up);
    citm_close_server(citm, s);
    citm_close_client(citm, c);
}

// incoming connections become io_pairs until the upwards connection is made
void citm_on_imap_connection(citm_t *citm, citm_conn_t *conn){
    derr_t e = E_OK;

    PROP_GO(&e,
        io_pair_new(
            citm->scheduler,
            citm->io,
            conn,
            citm_io_pair_cb,
            citm,
            &citm->io_pairs
        ),
    fail);

    return;

fail:
    DUMP(e);
    DROP_VAR(&e);
    citm_close_conn(citm, conn);
}
