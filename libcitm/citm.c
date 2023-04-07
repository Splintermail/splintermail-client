#include <signal.h>
#include <string.h>

#include "libcitm.h"

derr_t citm_init(citm_t *citm, citm_io_i *io, scheduler_i *scheduler){
    derr_t e = E_OK;

    *citm = (citm_t){ .io = io, .scheduler = scheduler };

    PROP_GO(&e, hashmap_init(&citm->preusers), fail);
    PROP_GO(&e, hashmap_init(&citm->users), fail);

    return e;

fail:
    citm_free(citm);
    return e;
}

void citm_free(citm_t *citm){
    hashmap_free(&citm->preusers);
    hashmap_free(&citm->users);
    *citm = (citm_t){0};
}

void citm_cancel(citm_t *citm){
    link_t *link;
    hashmap_elem_t *elem;
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
    elem = hashmap_pop_iter(&trav, &citm->preusers)
    while((elem = hashmap_pop_next(trav))){
        preuser_cancel(elem);
    }
    // cancel users
    elem = hashmap_pop_iter(&trav, &citm->users)
    while((elem = hashmap_pop_next(trav))){
        user_cancel(elem);
    }
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
    citm_t *citm = data;
    user_new(user, servers, clients, kd, xkey_client, &citm->users);
}

// completed anons attach to new or existing preuser, or an existing user
static void citm_anon_cb(
    void *data,
    imap_server_t *imap_dn,
    imap_client_t *imap_up,
    dstr_t user,
    dstr_t pass
){
    citm_t *citm = data;

    // check for existing user
    hash_elem_t *elem = hashmap_gets(&citm->users, &user);
    if(elem){
        // attach to existing user
        user_add_pair(elem, imap_dn, imap_up);
        goto discard_user_pass;
    }

    // check for existing preuser
    elem = hashmap_gets(&citm->preusers, &user);
    if(elem){
        // attach to existing preuser
        preuser_add_pair(elem, imap_dn, imap_up);
        goto discard_user_pass;
    }

    // create a new preuser
    keydir_t *kd;

    derr_t e = E_OK;
    PROP_GO(&e, keydir_new(&kd, ...), fail);

    preuser_new(
        &citm->io,
        user,
        pass,
        kd,
        imap_dn,
        imap_up,
        citm_preuser_cb,
        citm,
        &citm->preusers
    );
    return;

fail:
    DUMP(e);
    DROP_VAR(&e);
    imap_server_free(imap_dn);
    imap_client_free(imap_up);

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
    anon_new(conn_dn, conn_up, citm_anon_cb, citm, &citm->anons);
}

// incoming connections become io_pairs until the upwards connection is made
void citm_new_connection(citm_t *citm, citm_conn_t *conn){
    io_pair_new(citm->io, conn, citm_io_pair_cb, citm, &citm->io_pairs);
}

////////////////



static bool hard_exit = false;
void stop_loop_on_signal(int signum){
    (void) signum;
    LOG_ERROR("caught signal\n");
    if(hard_exit) exit(1);
    hard_exit = true;
    // launch an asynchronous loop abort
    loop_close(&g_loop, E_OK);
}


derr_t citm(
    const char *local_host,
    const char *local_svc,
    const char *key,
    const char *cert,
    const char *remote_host,
    const char *remote_svc,
    const string_builder_t *maildir_root,
    bool indicate_ready
){
    derr_t e = E_OK;

    // init ssl contexts
    ssl_context_t ctx_srv;
    PROP(&e, ssl_context_new_server(&ctx_srv, cert, key) );

    ssl_context_t ctx_cli;
    PROP_GO(&e, ssl_context_new_client(&ctx_cli), cu_ctx_srv);

    imap_pipeline_t pipeline;

    PROP_GO(&e,
        citme_init(&g_citme, maildir_root, &g_imape.engine
    ), cu_ctx_cli);

    PROP_GO(&e, build_pipeline(&pipeline, &g_citme), cu_citme);

    /* After building the pipeline, we must run the pipeline if we want to
       cleanup nicely.  That means that we can't follow the normal cleanup
       pattern, and instead we must initialize all of our variables to zero
       (that is, if we had any variables right here) */

    // add the lspec to the loop
    citm_lspec_t citm_lspec = {
        .remote_host = remote_host,
        .remote_svc = remote_svc,
        .pipeline = &pipeline,
        .citme = &g_citme,
        .ctx_srv = &ctx_srv,
        .ctx_cli = &ctx_cli,
        .lspec = {
            .addr = local_host,
            .svc = local_svc,
            .conn_recvd = conn_recvd,
        },
    };
    PROP_GO(&e, loop_add_listener(&g_loop, &citm_lspec.lspec), fail);

    // install signal handlers before indicating we are launching the loop
    signal(SIGINT, stop_loop_on_signal);
    signal(SIGTERM, stop_loop_on_signal);

    if(indicate_ready){
        LOG_INFO("listener ready\n");
    }else{
        // always indicate on DEBUG-level logs
        LOG_DEBUG("listener ready\n");
    }

fail:
    if(is_error(e)){
        loop_close(&g_loop, e);
        // The loop will pass us this error back after loop_run.
        PASSED(e);
    }

    // run the loop
    PROP_GO(&e, loop_run(&g_loop), cu);

cu:
    free_pipeline(&pipeline);
cu_citme:
    citme_free(&g_citme);
cu_ctx_cli:
    ssl_context_free(&ctx_cli);
cu_ctx_srv:
    ssl_context_free(&ctx_srv);
    return e;
}
