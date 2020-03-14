#include <networking.h>
#include <libdstr/libdstr.h>

#include "fake_engine.h"

/////// reader-writer thread stuff

void *reader_writer_thread(void *arg){
    reader_writer_context_t *ctx = arg;
    derr_t e = E_OK;

    // allocate ssl context
    ssl_context_t ssl_ctx;
    if(ctx->use_tls){
        PROP_GO(&e, ssl_context_new_client(&ssl_ctx), fail);
    }

    // generate all the buffers we are going to send
    LIST(dstr_t) out_bufs;
    PROP_GO(&e, LIST_NEW(dstr_t, &out_bufs, ctx->writes_per_thread), fail_ssl);
    for(size_t i = 0; i < ctx->writes_per_thread; i++){
        dstr_t temp;
        // allocate the dstr in the list
        PROP_GO(&e, dstr_new(&temp, 64), free_out_bufs);
        // write something into the buffer
        PROP_GO(&e, FMT(&temp, "%x:%x\n", FU(ctx->thread_id), FU(i)), free_temp);
        // add it to the list
        PROP_GO(&e, LIST_APPEND(dstr_t, &out_bufs, temp), free_temp);
        continue;
    free_temp:
        dstr_free(&temp);
        goto free_out_bufs;
    }

    // check if we are the last thread ready
    pthread_mutex_lock(ctx->mutex);
    (*ctx->threads_ready)++;
    // last thread signals the others
    if(*ctx->threads_ready == ctx->num_threads){
        pthread_cond_broadcast(ctx->cond);
    }
    // other threads wait for the last one
    else{
        while(*ctx->threads_ready < ctx->num_threads){
            pthread_cond_wait(ctx->cond, ctx->mutex);
        }
    }
    pthread_mutex_unlock(ctx->mutex);

    // open a connection
    connection_t conn;
    if(ctx->use_tls){
        connection_new_ssl(&conn, &ssl_ctx, "127.0.0.1", ctx->listen_port);
    }else{
        connection_new(&conn, "127.0.0.1", ctx->listen_port);
    }
    // write all of the buffers
    for(size_t i = 0; i < out_bufs.len; i++){
        PROP_GO(&e, connection_write(&conn, &out_bufs.data[i]), close_conn);
    }
    // read all of the buffers into a single place
    dstr_t recvd;
    PROP_GO(&e, dstr_new(&recvd, 8192), close_conn);
    while( dstr_count(&recvd, &DSTR_LIT("\n")) < out_bufs.len){
        PROP_GO(&e, connection_read(&conn, &recvd, NULL), free_recvd);
    }
    // now compare the buffers
    size_t compared = 0;
    for(size_t i = 0; i < out_bufs.len; i++){
        // cmp is the section of leftovers that
        dstr_t cmp = dstr_sub(&recvd, compared,
                              compared + out_bufs.data[i].len);
        if(dstr_cmp(&cmp, &out_bufs.data[i]) != 0)
            ORIG_GO(&e, E_VALUE, "received bad response!", free_recvd);
        compared += out_bufs.data[i].len;
    }

    // done!

free_recvd:
    dstr_free(&recvd);
close_conn:
    connection_close(&conn);
free_out_bufs:
    for(size_t i = 0; i < out_bufs.len; i++){
        dstr_free(&out_bufs.data[i]);
    }
    LIST_FREE(dstr_t, &out_bufs);
fail_ssl:
    if(ctx->use_tls){
        ssl_context_free(&ssl_ctx);
    }
fail:
    ctx->error = e;
    return NULL;
}

static void echo_session_dying(manager_i *mgr, derr_t error){
    tagged_mgr_t *tmgr = CONTAINER_OF(mgr, tagged_mgr_t, mgr);
    echo_session_mgr_t *esm = CONTAINER_OF(tmgr, echo_session_mgr_t, tmgr);
    if(is_error(error)){
        loop_close(esm->s.pipeline->loop, error);
        PASSED(error);
    }
}

static void echo_session_dead(manager_i *mgr){
    tagged_mgr_t *tmgr = CONTAINER_OF(mgr, tagged_mgr_t, mgr);
    echo_session_mgr_t *esm = CONTAINER_OF(tmgr, echo_session_mgr_t, tmgr);
    imap_session_free(&esm->s);

    free(esm);
}

derr_t echo_session_mgr_new(echo_session_mgr_t **out,
        imap_session_alloc_args_t args){
    derr_t e = E_OK;

    echo_session_mgr_t *esm = malloc(sizeof(*esm));
    if(!esm) ORIG(&e, E_NOMEM, "no mem");
    *esm = (echo_session_mgr_t){
        .tmgr={
            .is_cbrw=false,
            .mgr={
                .dying=echo_session_dying,
                .dead=echo_session_dead,
            },
        },
    };

    args.mgr = &esm->tmgr.mgr;

    PROP_GO(&e, imap_session_alloc_accept(&esm->s, &args), fail);

    *out = esm;

    return e;

fail:
    free(esm);
    return e;
}

/////// cb_reader_writer stuff

static derr_t cb_reader_writer_send(cb_reader_writer_t *cbrw, event_t **out){
    derr_t e = E_OK;

    *out = NULL;

    if(cbrw->dying) return e;

    // check for completion criteria
    if(cbrw->nrecvd == cbrw->nwrites){
        imap_session_close(&cbrw->s.session, E_OK);
        return e;
    }

    // reset buffers
    cbrw->out.len = 0;
    cbrw->in.len = 0;

    PROP(&e, FMT(&cbrw->out, "cbrw%x:%x/%x\n", FU(cbrw->id),
                FU(cbrw->nrecvd + 1), FU(cbrw->nwrites)) );

    // copy out into a write event (which will be freed, but not by us)
    event_t *ev;
    PROP(&e, fake_engine_get_write_event(cbrw->fake_engine, &cbrw->out, &ev) );

    ev->session = &cbrw->s.session;
    imap_session_ref_up(&cbrw->s);
    *out = ev;

    return e;
}

static void cbrw_session_dying(manager_i *mgr, derr_t error){
    tagged_mgr_t *tmgr = CONTAINER_OF(mgr, tagged_mgr_t, mgr);
    cb_reader_writer_t *cbrw = CONTAINER_OF(tmgr, cb_reader_writer_t, tmgr);
    if(is_error(error)){
        loop_close(cbrw->s.pipeline->loop, error);
        PASSED(error);
    }
}

static void cbrw_session_dead(manager_i *mgr){
    tagged_mgr_t *tmgr = CONTAINER_OF(mgr, tagged_mgr_t, mgr);
    cb_reader_writer_t *cbrw = CONTAINER_OF(tmgr, cb_reader_writer_t, tmgr);
    imap_session_free(&cbrw->s);

    cb_reader_writer_free(cbrw);
}

derr_t cb_reader_writer_init(cb_reader_writer_t *cbrw, size_t id,
        size_t nwrites, engine_t *fake_engine, imap_session_alloc_args_t args){
    derr_t e = E_OK;
    *cbrw = (cb_reader_writer_t){0};

    // allocate the buffers
    PROP_GO(&e, dstr_new(&cbrw->out, 256), fail);
    PROP_GO(&e, dstr_new(&cbrw->in, 256), free_out);

    cbrw->id = id;
    cbrw->nwrites = nwrites;
    cbrw->fake_engine = fake_engine;
    cbrw->tmgr = (tagged_mgr_t){
        .is_cbrw=true,
        .mgr={
            .dying=cbrw_session_dying,
            .dead=cbrw_session_dead,
        },
    };

    args.mgr = &cbrw->tmgr.mgr;

    PROP_GO(&e, imap_session_alloc_connect(&cbrw->s, &args), free_in);

    return e;

free_in:
    dstr_free(&cbrw->in);
free_out:
    dstr_free(&cbrw->out);
fail:
    return e;
}

void cb_reader_writer_free(cb_reader_writer_t *cbrw){
    dstr_free(&cbrw->in);
    dstr_free(&cbrw->out);
}

static derr_t cb_reader_writer_read(cb_reader_writer_t *cbrw, dstr_t *buffer,
        event_t **out){
    derr_t e = E_OK;

    *out = NULL;

    // if we are exiting, just ignore this
    if(cbrw->dying) return e;

    // append the buffer to the input
    PROP(&e, dstr_append(&cbrw->in, buffer) );

    // make sure we have the full line
    if(dstr_count(&cbrw->in, &DSTR_LIT("\n")) == 0){
        return e;
    }

    // compare buffers
    if(dstr_cmp(&cbrw->in, &cbrw->out) != 0){
        TRACE(&e, "cbrw got a bad echo: \"%x\" (expected \"%x\")\n",
                FD(&cbrw->in), FD(&cbrw->out));
        ORIG(&e, E_VALUE, "bad echo");
    }

    cbrw->nrecvd++;

    // send another
    PROP(&e, cb_reader_writer_send(cbrw, out) );
    return e;
}

/////// fake engine stuff

static void fake_engine_pass_event(engine_t *engine, event_t *ev){
    fake_engine_t *fake_engine = CONTAINER_OF(engine, fake_engine_t, engine);
    queue_append(&fake_engine->event_q, &ev->link);
}

derr_t fake_engine_init(fake_engine_t *fake_engine){
    derr_t e = E_OK;
    PROP(&e, queue_init(&fake_engine->event_q) );
    fake_engine->engine.pass_event = fake_engine_pass_event;
    return e;
}

void fake_engine_free(fake_engine_t *fake_engine){
    queue_free(&fake_engine->event_q);
}

static void fake_engine_return_write_event(event_t *ev){
    engine_t *fake_engine = ev->returner_arg;
    ev->ev_type = EV_WRITE_DONE;
    fake_engine->pass_event(fake_engine, ev);
}

derr_t fake_engine_get_write_event(engine_t *engine, dstr_t *text,
        event_t **out){
    derr_t e = E_OK;
    // copy out into a write event (which will be freed, but not by us)
    event_t *ev = malloc(sizeof(*ev));
    if(!ev) ORIG(&e, E_NOMEM, "no mem");

    event_prep(ev, fake_engine_return_write_event, engine);
    ev->ev_type = EV_WRITE;

    PROP_GO(&e, dstr_new(&ev->buffer, text->len), fail);
    PROP_GO(&e, dstr_copy(text, &ev->buffer), fail);

    *out = ev;
    return e;

fail:
    free(ev);
    return e;
}

static derr_t launch_second_half_of_test(session_cb_data_t *cb_data,
        engine_t *fake_engine, engine_t *upstream){
    derr_t e = E_OK;
    // make NUM_THREAD connections
    for(size_t i = 0; i < cb_data->num_threads; i++){

        // prepare a cb_reader_writer
        cb_reader_writer_t *cbrw = &cb_data->cb_reader_writers[i];
        PROP(&e, cb_reader_writer_init(cbrw, i, cb_data->writes_per_thread,
                    fake_engine, cb_data->session_connect_args) );

        // start the session
        imap_session_start(&cbrw->s);

        // get the first message
        event_t *ev_new;
        PROP(&e, cb_reader_writer_send(cbrw, &ev_new) );

        // pass the write message
        if(ev_new != NULL){
            upstream->pass_event(upstream, ev_new);
            cb_data->nwrites++;
        }
    }

    return e;
}

static derr_t handle_read(session_cb_data_t *cb_data, event_t *ev,
        engine_t *fake_engine, engine_t *upstream){
    derr_t e = E_OK;

    if(ev->buffer.len == 0){
        // done with this session
        imap_session_close(ev->session, E_OK);
        // was that the last session?
        cb_data->nEOF++;
        if(cb_data->nEOF == cb_data->num_threads){
            PROP(&e, launch_second_half_of_test(cb_data, fake_engine,
                        upstream) );
        }else if(cb_data->nEOF == cb_data->num_threads*2){
            // test is over
            loop_close(&cb_data->test_ctx->loop, E_OK);
        }
        return e;
    }

    // is this session a cb_reader_writer session?
    imap_session_t *s = CONTAINER_OF(ev->session, imap_session_t, session);
    manager_i *mgr = s->mgr;
    tagged_mgr_t *tmgr = CONTAINER_OF(mgr, tagged_mgr_t, mgr);

    if(tmgr->is_cbrw){
        cb_reader_writer_t *cbrw = CONTAINER_OF(tmgr, cb_reader_writer_t, tmgr);
        event_t *ev_new;
        PROP(&e, cb_reader_writer_read(cbrw, &ev->buffer, &ev_new) );
        if(ev_new){
            // pass the write
            upstream->pass_event(upstream, ev_new);
            cb_data->nwrites++;
        }
    }
    // otherwise, echo back the message
    else{
        event_t *ev_new;
        PROP(&e, fake_engine_get_write_event(fake_engine, &ev->buffer,
                    &ev_new) );
        ev_new->session = ev->session;
        imap_session_t *s = CONTAINER_OF(ev->session, imap_session_t, session);
        imap_session_ref_up(s);
        // pass the write
        upstream->pass_event(upstream, ev_new);
        cb_data->nwrites++;
    }

    return e;
}

static void handle_write_done(session_cb_data_t *cb_data, event_t *ev){
    // downref session
    imap_session_t *s = CONTAINER_OF(ev->session, imap_session_t, session);
    imap_session_ref_down(s);
    // free event
    dstr_free(&ev->buffer);
    free(ev);
    cb_data->nwrites--;
}

static bool quit_ready(session_cb_data_t *cb_data){
    return cb_data->nwrites == 0;
}

derr_t fake_engine_run(fake_engine_t *fe, engine_t *upstream,
        session_cb_data_t *cb_data, loop_t *loop){
    derr_t e = E_OK;
    // process incoming events from the upstream engine
    event_t *quit_ev = NULL;
    link_t *link;
    while((link = queue_pop_first(&fe->event_q, true))){
        event_t *ev = CONTAINER_OF(link, event_t, link);
        switch(ev->ev_type){
            case EV_READ:
                //LOG_ERROR("got read\n");
                // check for EOF
                IF_PROP(&e, handle_read(cb_data, ev, &fe->engine, upstream) ){
                    loop_close(loop, e);
                    PASSED(e);
                }
                // return buffer
                ev->returner(ev);
                break;
            case EV_QUIT_DOWN:
                // check if we need to wait for write events to be returned
                if(quit_ready(cb_data)){
                    ev->ev_type = EV_QUIT_UP;
                    upstream->pass_event(upstream, ev);
                    return e;
                }else{
                    quit_ev = ev;
                }
                break;
            case EV_WRITE_DONE:
                handle_write_done(cb_data, ev);
                // check for quitting condition
                if(quit_ev && quit_ready(cb_data)){
                    quit_ev->ev_type = EV_QUIT_UP;
                    upstream->pass_event(upstream, quit_ev);
                    return e;
                }
                break;
            // other events should not happen
            case EV_READ_DONE:
            case EV_WRITE:
            case EV_QUIT_UP:
            default:
                ORIG(&e, E_VALUE,
                        "unexpected event type from upstream engine\n");
        }
    }
    ORIG(&e, E_VALUE, "unexpected exit from fake_engine");
}
