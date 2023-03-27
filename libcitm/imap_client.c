static derr_t advance_reads(imap_client_t *c, bool *ok){
    derr_t e = E_OK;
    *ok = false;

    while(link_list_isemtpy(&c->resps)){
        ONCE(c->read_started){
            stream_must_read(c->stream, &c->read_req, c->rbuf, read_cb);
        }
        if(!c->read_done) return e;
        c->read_started = false;
        c->read_done = false;
        PROP(&e, imap_resp_read(&c->reader, c->read_buf, &c->resps) );
    }

    *ok = true;
    return e;
}

static derr_t advance_writes(imap_client_t *c, bool *ok){
    derr_t e = E_OK;
    *ok = false;

    while(!link_list_isempty(&c->cmds)){
        // work on writing the first cmd
        imap_cmd_t *cmd = CONTAINER_OF(c->cmds.next, imap_cmd_t, link);
        ONCE(c->write_started){
            size_t want;
            PROP(&e,
                imap_cmd_write(cmd, &c->wbuf, &c->write_skip, &want, c->exts)
            );
            c->wrote_all = (want == 0);
            stream_must_write(c->stream, &c->write_req, c->wbuf, 1, write_cb);
        }
        if(!c->write_done) return e;
        // finished a write to the wire
        c->write_started = false;
        c->write_done = false;
        if(c->wrote_all){
            // finished writing a whole cmd
            link_remove(&cmd->link);
            imap_cmd_free(cmd);
            c->write_skip = 0;
        }
    }

    *ok = true;
    return e;
}

static derr_t check_starttls_resp(imap_client_t *c){
    // XXX: half-written code, doesn't clean up resp very well
    imap_resp_t *resp = CONTAINER_OF(
        link_list_pop_first(&c->resps, imap_resp_t, link)
    );
    if(resp != IMAP_RESP_STATUS_TYPE){
        imap_resp_free(resp);
    }
    // expect exactly one response
    if(!link_list_isempty(&c->resps)){
        ORIG_GO(&e, E_RESPONSE, "too many responses to STARTTLS", cu);
    }
}

static void advance_state(imap_client_t *c){
    derr_t e = E_OK;
    bool ok;

    // wait for greeting
    if(!c->greet_done){
        if(!c->greet_recvd){
            PROP_GO(&e, advance_reads(c, &ok), cu);
            if(!ok) return;
            c->greet_recvd = true;
        }
        // XXX: process greeting
        c->greet_done = true;
    }

    // do starttls if necessary
    if(c->security == IMAP_STARTTLS && !c->starttls){
        ONCE(c->starttls_sent) PROP_GO(&e, send_starttls(c), cu);
        PROP_GO(&e, advance_writes(c, ok), cu);
        if(!ok) return;
        /* the client initiates the ssl connection, so we can safely read the
           starttls response using the normal greedy stream reading logic */
        PROP_GO(&e, advance_reads(c, &ok), cu);
        if(!ok) return;
        PROP_GO(&e, check_starttls_resp(c), cu);
        c->starttls = true;
    }

    // now act as a blind relay

    // process read requests
    while(!link_list_isempty(&c->reads)){
        PROP_GO(&e, advance_reads(c, &ok) );
        if(!ok) break;
        // fulfill one read
        imap_client_read_t *req = CONTAINER_OF(
            link_list_pop_first(&c->reads), imap_client_read_t, link
        );
        imap_resp_t *resp = CONTAINER_OF(
            link_list_pop_first(&c->resps), imap_resp_t, link
        );
        req->cb(c, req, resp, true);
    }

    // process write requests
    while(!link_list_isempty(&c->writes)){
        // peek at the first req
        imap_client_write_t *req = CONTAINER_OF(
            c->writes.next, imap_client_write_t, link
        );
        if(req->cmd){
            imap_cmd_t *cmd = STEAL(imap_cmd_t, &req->cmd);
            link_list_append(&c->cmds, cmd);
        }
        PROP_GO(&e, advance_writes(c, &ok), cu);
        if(!ok) break;
        // finished this write request
        link_remove(&req->link);
        req->cb(c, req, true);
    }

    return;

cu:

}
