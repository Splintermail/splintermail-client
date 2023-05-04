#include <signal.h>
#include <string.h>

#include "libcitm.h"

typedef struct {
    dstr_t user;
    dstr_t pass;
    link_t servers;
    link_t clients;
    hash_elem_t elem;  // citm_t->holds
} citm_hold_t;
DEF_CONTAINER_OF(citm_hold_t, elem, hash_elem_t)

static void hold_new(
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
    // XXX: tell client why?
    imap_server_must_free(&s);
    imap_client_must_free(&c);
}

static void hold_cancel(hash_elem_t *elem){
    citm_hold_t *hold = CONTAINER_OF(elem, citm_hold_t, elem);
    hash_elem_remove(&hold->elem);
    dstr_free(&hold->user);
    dstr_free0(&hold->pass);
    // XXX: tell clients why?
    imap_server_must_free_list(&hold->servers);
    imap_client_must_free_list(&hold->clients);
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
    // cancel io_pairs
    while((link = link_list_pop_first(&citm->io_pairs))){
        io_pair_cancel(link);
    }
    // cancel anons
    while((link = link_list_pop_first(&citm->anons))){
        anon_cancel(link);
    }
    // cancel preusers
    elem = hashmap_pop_iter(&trav, &citm->preusers);
    while((elem = hashmap_pop_next(&trav))){
        preuser_cancel(elem);
    }
    // cancel users
    elem = hashmap_pop_iter(&trav, &citm->users);
    while((elem = hashmap_pop_next(&trav))){
        user_cancel(elem);
    }
    // cancel holds
    elem = hashmap_pop_iter(&trav, &citm->holds);
    while((elem = hashmap_pop_next(&trav))){
        hold_cancel(elem);
    }
}

static void citm_preuser_cb(
    void *data,
    dstr_t user,
    link_t *servers,
    link_t *clients,
    keydir_i *iface,
    imap_client_t *xkey_client
);

// after user closes, late server/client pairs become new preusers
static void citm_user_cb(void *data, const dstr_t *constuser){
    derr_t e = E_OK;

    citm_t *citm = data;

    link_t servers = {0};
    link_t clients = {0};
    dstr_t user = {0};
    dstr_t pass = {0};

    bool ok = hold_pop(
        &citm->holds, constuser, &servers, &clients, &user, &pass
    );
    if(!ok) return;

    keydir_i *kd;
    PROP_GO(&e, keydir_new(&citm->root, user, &kd), cu);

    preuser_new(
        citm->scheduler,
        citm->io,
        STEAL(dstr_t, &user),
        STEAL(dstr_t, &pass),
        kd,
        CONTAINER_OF(link_list_pop_first(&servers), imap_server_t, link),
        CONTAINER_OF(link_list_pop_first(&clients), imap_client_t, link),
        citm_preuser_cb,
        citm,
        &citm->preusers
    );

    // get the elem if preuser_new succeeded
    hash_elem_t *elem = hashmap_gets(&citm->preusers, &user);
    if(!elem) goto cu;

    // put all the s/c pairs we have into this preuser
    while(!link_list_isempty(&servers)){
        preuser_add_pair(
            elem,
            CONTAINER_OF(link_list_pop_first(&servers), imap_server_t, link),
            CONTAINER_OF(link_list_pop_first(&clients), imap_client_t, link)
        );
    }

cu:
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
    }
    dstr_free(&user);
    dstr_free0(&pass);
    // XXX: tell clients why?
    imap_server_must_free_list(&servers);
    imap_client_must_free_list(&clients);
    return;
}

// completed preusers transition to new users
static void citm_preuser_cb(
    void *data,
    dstr_t user,
    link_t *servers,
    link_t *clients,
    keydir_i *kd,
    imap_client_t *xkey_client
){
    derr_t e = E_OK;

    citm_t *citm = data;

    PROP_GO(&e, keydir_keysync_completed(kd), fail);

    user_new(
        citm->scheduler,
        user,
        servers,
        clients,
        kd,
        xkey_client,
        citm_user_cb,
        citm,
        &citm->users
    );

    return;

fail:
    DUMP(e);
    DROP_VAR(&e);
    // XXX: tell clients why?
    imap_server_must_free_list(servers);
    imap_client_must_free_list(clients);
}

// completed anons attach to new or existing preuser, or an existing user
static void citm_anon_cb(
    void *data,
    imap_server_t *s,
    imap_client_t *c,
    dstr_t user,
    dstr_t pass
){
    citm_t *citm = data;

    // check for existing user
    hash_elem_t *elem = hashmap_gets(&citm->users, &user);
    if(elem){
        // try attaching to existing user
        bool ok = user_add_pair(elem, s, c);
        if(ok) goto discard_user_pass;
        // user is shutting down, wait on this server/client pair
        elem = hashmap_gets(&citm->holds, &user);
        if(elem){
            // attach to existing hold
            hold_add_pair(elem, s, c);
            goto discard_user_pass;
        }
        // start a new hold
        hold_new(s, c, user, pass, &citm->holds);
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
    derr_t e = E_OK;
    keydir_i *kd;
    PROP_GO(&e, keydir_new(&citm->root, user, &kd), fail);

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
    );
    return;

fail:
    DUMP(e);
    DROP_VAR(&e);
    imap_server_free(&s);
    imap_client_free(&c);

discard_user_pass:
    dstr_free(&user);
    dstr_zeroize(&pass);
    dstr_free(&pass);
    return;
}

// completed io_pairs become anon's until login is complete
static void citm_io_pair_cb(
    void *data, citm_conn_t *conn_dn, citm_conn_t *conn_up
){
    citm_t *citm = data;
    anon_new(
        citm->scheduler, conn_dn, conn_up, citm_anon_cb, citm, &citm->anons
    );
}

// incoming connections become io_pairs until the upwards connection is made
void citm_on_imap_connection(citm_t *citm, citm_conn_t *conn){
    io_pair_new(citm->io, conn, citm_io_pair_cb, citm, &citm->io_pairs);
}
