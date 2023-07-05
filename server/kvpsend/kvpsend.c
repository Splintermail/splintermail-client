#include "server/kvpsend/kvpsend.h"

#include <stdlib.h>

static void state_cb(kvpsync_send_t *send, bool ok, void *data);
static derr_t full_scan(kvpsend_t *k, xtime_t now);
void configure_sub_timeout(kvpsend_t *k);
static derr_t advance_sender(kvpsend_t *k, sender_t *sender, xtime_t now);
static derr_t advance_all_senders(kvpsend_t *k, xtime_t now);

derr_t sender_init(
    kvpsend_t *k, sender_t *sender, const struct sockaddr *addr, xtime_t now
){
    derr_t e = E_OK;
    *sender = (sender_t){ .k = k, .addr = addr, .deadline = XTIME_MAX };
    PROP(&e, kvpsync_send_init(&sender->send, now, state_cb, k) );
    return e;
}

void sender_free(sender_t *sender){
    kvpsync_send_free(&sender->send);
}

static const void *entry_get_key(const jsw_anode_t *node){
    const entry_t *entry = CONTAINER_OF(node, entry_t, node);
    return &entry->subdomain;
}

derr_t kvpsend_init(
    kvpsend_t *k,
    kvpsend_i *I,
    xtime_t now,
    struct sockaddr_storage *peers,
    size_t npeers
){
    derr_t e = E_OK;

    *k = (kvpsend_t){ .I = I, .nsenders = npeers, .next_timeout = XTIME_MAX };

    jsw_ainit(&k->sorted, jsw_cmp_dstr, entry_get_key);

    for(size_t i = 0; i < k->nsenders; i++){
        PROP_GO(&e,
            sender_init(k, &k->senders[i], ss2sa(&peers[i]), now), fail
        );
    }

    // start with an initial scan
    PROP_GO(&e, full_scan(k, now), fail);

    return e;

fail:
    kvpsend_free(k);
    return e;
}

entry_t *entry_first(jsw_atrav_t *trav, jsw_atree_t *tree){
    jsw_anode_t *node = jsw_atfirst(trav, tree);
    return CONTAINER_OF(node, entry_t, node);
}

entry_t *entry_next(jsw_atrav_t *trav){
    jsw_anode_t *node = jsw_atnext(trav);
    return CONTAINER_OF(node, entry_t, node);
}

entry_t *entry_pop_to_next(jsw_atrav_t *trav){
    jsw_anode_t *node = jsw_pop_atnext(trav);
    return CONTAINER_OF(node, entry_t, node);
}

void kvpsend_free(kvpsend_t *k){
    jsw_atrav_t trav;
    entry_t *entry = entry_first(&trav, &k->sorted);
    while(entry){
        entry_t *next = entry_pop_to_next(&trav);
        delete_entry(k, entry, false);
        entry = next;
    }
    for(size_t i = 0; i < k->nsenders; i++){
        sender_free(&k->senders[i]);
    }
}

static void subscribers_respond_all(kvpsend_t *k, entry_t *entry, dstr_t msg){
    kvpsend_i *I = k->I;
    link_t *link;
    while((link = link_list_pop_first(&entry->subscribers))){
        subscriber_t *sub = CONTAINER_OF(link, subscriber_t, link);
        link_remove(&sub->tlink);
        I->subscriber_respond(I, sub, msg);
    }
    // we may have a new timeout now
    configure_sub_timeout(k);
}

static void subscribers_close_all(kvpsend_t *k, entry_t *entry){
    kvpsend_i *I = k->I;
    link_t *link;
    while((link = link_list_pop_first(&entry->subscribers))){
        subscriber_t *sub = CONTAINER_OF(link, subscriber_t, link);
        link_remove(&sub->tlink);
        I->subscriber_close(I, sub);
    }
    // we may have a new timeout now
    configure_sub_timeout(k);
}

static derr_t entry_new(
    const dstr_t subdomain,
    const dstr_t challenge,
    entry_t **out
){
    derr_t e = E_OK;

    *out = NULL;

    entry_t *entry = NULL;

    if(subdomain.len > sizeof(entry->subdomainbuf)){
        ORIG(&e, E_PARAM, "subdomain is too long");
    }
    if(challenge.len > sizeof(entry->challengebuf)){
        ORIG(&e, E_PARAM, "challenge is too long");
    }

    entry = DMALLOC_STRUCT_PTR(&e, entry);
    CHECK(&e);

    DSTR_WRAP_ARRAY(entry->subdomain, entry->subdomainbuf);
    dstr_append_quiet(&entry->subdomain, &subdomain);

    DSTR_WRAP_ARRAY(entry->challenge, entry->challengebuf);
    dstr_append_quiet(&entry->challenge, &challenge);

    *out = entry;

    return e;
}

static bool entry_ok(kvpsend_t *k, entry_t *entry){
    // at least one recv is ok and all ok recvs have acked this entry
    return k->okmask && (entry->readymask & k->okmask) == k->okmask;
}

static void entry_now_ready(kvpsend_t *k, entry_t *entry){
    subscribers_respond_all(k, entry, DSTR_LIT("k"));
    entry->ready = true;
}

// a sender transitions states ok->notok or notok->ok
static void state_cb(kvpsync_send_t *send, bool ok, void *data){
    sender_t *sender = CONTAINER_OF(send, sender_t, send);
    kvpsend_t *k = data;
    ptrdiff_t i = sender - k->senders;
    unsigned int bit = 1 << i;
    unsigned int oldmask = k->okmask;
    if(ok){
        // a kpvsync pair becomes ok
        k->okmask |= bit;
    }else{
        // a kpvsync pair becomes not ok
        k->okmask &= ~bit;
    }

    // detect noop
    if(k->okmask == oldmask) return;

    /* note that the notok->ok transition can only cause entries to be ready
       when there was no ok kvpsyncs before, since adding to the okmask can not
       correct for any previously missing readymask bits */
    if(ok && oldmask) return;

    // check if any cbs can now be triggered

    jsw_atrav_t trav;
    entry_t *entry = entry_first(&trav, &k->sorted);
    /* no need to ever clear bit on any readymasks, since the cb only happens
       the first time a recv acks the entry (not again after resyncs) */
    for(; entry; entry = entry_next(&trav)){
        if(entry->ready || !entry_ok(k, entry)) continue;
        entry_now_ready(k, entry);
    }
}

static void add_key_cb(kvpsync_send_t *send, void *data){
    entry_t *entry = data;
    sender_t *sender = CONTAINER_OF(send, sender_t, send);
    kvpsend_t *k = sender->k;
    ptrdiff_t i = sender - k->senders;
    entry->readymask |= 1 << i;
    if(!entry->ready && entry_ok(k, entry)){
        entry_now_ready(k, entry);
    }
};

// crashes on E_NOMEM
static void kvpsync_send_add_key_all(
    kvpsend_t *k, entry_t *entry, xtime_t now
){
    for(size_t i = 0; i < k->nsenders; i++){
        kvpsync_send_add_key(
            &k->senders[i].send,
            now,
            entry->subdomain,
            entry->challenge,
            add_key_cb,
            entry
        );
    }
}

static void kvpsync_send_delete_key_all(kvpsend_t *k, entry_t *entry){
    for(size_t i = 0; i < k->nsenders; i++){
        kvpsync_send_delete_key(&k->senders[i].send, entry->subdomain);
    }
}

static void change_entry(
    kvpsend_t *k, entry_t *entry, const dstr_t challenge, xtime_t now
){
    // empty old subscribers
    subscribers_close_all(k, entry);
    // delete this key from the senders
    kvpsync_send_delete_key_all(k, entry);
    // reset the entry
    entry->ready = false;
    entry->readymask = 0;
    // modify challenge text
    #if SMSQL_CHALLENGE_SIZE != KVPSYNC_MAX_LEN
    #error "SMSQL_CHALLENGE_SIZE does not match KVPSYNC_MAX_LEN"
    #endif
    entry->challenge.len = 0;
    dstr_append_quiet(&entry->challenge, &challenge);
    // add new version to senders
    kvpsync_send_add_key_all(k, entry, now);
}

void delete_entry(kvpsend_t *k, entry_t *entry, bool in_sorted){
    if(in_sorted){
        jsw_anode_t *node = jsw_aerase(&k->sorted, &entry->subdomain);
        if(node != &entry->node){
            LOG_FATAL(
                "erased the wrong entry for \"%x\"\n", FD(entry->subdomain)
            );
        }
    }
    kvpsync_send_delete_key_all(k, entry);
    subscribers_close_all(k, entry);
    free(entry);
}

static entry_t *pop_entry(link_t *entries){
    link_t *link = link_list_pop_first(entries);
    return CONTAINER_OF(link, entry_t, link);
}

static derr_t entry_new_from_challenges(
    challenge_iter_t challenges, entry_t **out
){
    derr_t e = E_OK;
    PROP(&e, entry_new(challenges.subdomain, challenges.challenge, out) );
    return e;
}

static derr_t full_scan(kvpsend_t *k, xtime_t now){
    derr_t e = E_OK;

    kvpsend_i *I = k->I;

    jsw_atrav_t entries;
    challenge_iter_t challenges;

    // hold new entries aside while we iteratet through data structures
    link_t new = {0};

    entry_t *entry = entry_first(&entries, &k->sorted);
    PROP(&e, I->challenges_first(I, &challenges) );

    while(entry && challenges.ok){
        int cmp = dstr_cmp2(entry->subdomain, challenges.subdomain);
        if(cmp < 0){
            // extra entry (entry comes before challenge)
            entry_t *next = entry_pop_to_next(&entries);
            delete_entry(k, entry, false);
            entry = next;
            continue;
        }
        if(cmp > 0){
            // missing entry (challenge comes before entry)
            entry_t *temp;
            PROP_GO(&e, entry_new_from_challenges(challenges, &temp), cu);
            link_list_append(&new, &temp->link);
            PROP_GO(&e, I->challenges_next(I, &challenges), cu);
            continue;
        }
        // subdomains match, no adding or deleting entries
        if(!dstr_eq(entry->challenge, challenges.challenge)){
            // subdomains match but challenges do not
            change_entry(k, entry, challenges.challenge, now);
        }
        entry = entry_next(&entries);
        PROP_GO(&e, I->challenges_next(I, &challenges), cu);
        continue;
    }
    while(entry){
        // extra entries at the end
        entry_t *next = entry_pop_to_next(&entries);
        delete_entry(k, entry, false);
        entry = next;
    }
    while(challenges.ok){
        // missing entries for challenge
        PROP_GO(&e, entry_new_from_challenges(challenges, &entry), cu);
        link_list_append(&new, &entry->link);
        PROP_GO(&e, I->challenges_next(I, &challenges), cu);
    }

    // add new entries to data structures
    while((entry = pop_entry(&new))){
        jsw_ainsert(&k->sorted, &entry->node);
        kvpsync_send_add_key_all(k, entry, now);
    }

cu:
    I->challenges_free(I, &challenges);
    while((entry = pop_entry(&new))){
        free(entry);
    }

    return e;
}

// failures here indicate application-level failure
derr_t subscriber_read_cb(
    kvpsend_t *k,
    subscriber_t *sub,
    dstr_t inst_uuid,
    dstr_t req_challenge,
    xtime_t now
){
    derr_t e = E_OK;

    kvpsend_i *I = k->I;

    // query the database

    DSTR_VAR(subdomain, SMSQL_SUBDOMAIN_SIZE);
    DSTR_VAR(db_challenge, SMSQL_CHALLENGE_SIZE);
    bool subdomain_ok, challenge_ok;
    PROP_GO(&e,
        I->get_installation_challenge(I,
            inst_uuid,
            &subdomain,
            &subdomain_ok,
            &db_challenge,
            &challenge_ok
        ),
    fail_sub);

    if(!subdomain_ok){
        LOG_WARN("invalid inst_uuid (%x)\n", FX(inst_uuid));
        I->subscriber_close(I, sub);
        return e;
    }

    // synchronize internal state

    jsw_anode_t *node = jsw_afind(&k->sorted, &subdomain, NULL);
    entry_t *entry = CONTAINER_OF(node, entry_t, node);

    bool changed = false;
    if(!challenge_ok){
        if(entry){
            // detected deleted entry
            delete_entry(k, entry, true);
            changed = true;
        }
    }else if(!entry){
        // no matching entry, create a new one
        PROP_GO(&e, entry_new(subdomain, db_challenge, &entry), fail_sub);
        jsw_ainsert(&k->sorted, &entry->node);
        kvpsync_send_add_key_all(k, entry, now);
        changed = true;
    }else if(!dstr_eq(entry->challenge, db_challenge)){
        // changed entry detected
        change_entry(k, entry, db_challenge, now);
        changed = true;
    }
    if(changed){
        PROP_GO(&e, advance_all_senders(k, now), fail_sub);
    }

    // deal with subscriber

    if(!challenge_ok || !dstr_eq(req_challenge, db_challenge)){
        LOG_WARN("requested challenge does not match database\n");
        I->subscriber_close(I, sub);
    }else if(entry->ready){
        // entry is already ready
        I->subscriber_respond(I, sub, DSTR_LIT("k"));
    }else{
        // subscriber must wait for ready signal
        sub->deadline = now + SUBSCRIBER_TIMEOUT;
        link_list_append(&entry->subscribers, &sub->link);
        link_list_append(&k->allsubs, &sub->tlink);
        // we may have a new timeout now
        configure_sub_timeout(k);
    }

    return e;

fail_sub:
    // if we failed without making a decision on this subscriber
    I->subscriber_close(I, sub);
    return e;
}

void healthcheck_read_cb(kvpsend_t *k, subscriber_t *sub){
    kvpsend_i *I = k->I;

    DSTR_VAR(buf, MAX_PEERS+1);
    for(size_t i = 0; i < k->nsenders; i++){
        bool ok = k->okmask & (1 << i);
        dstr_append_char(&buf, ok ? 'y' : 'n');
    }
    dstr_append_char(&buf, '\n');

    I->subscriber_respond(I, sub, buf);
}

derr_t sender_send_cb(kvpsend_t *k, sender_t *sender, xtime_t now){
    derr_t e = E_OK;

    sender->inwrite = false;
    PROP(&e, advance_sender(k, sender, now) );

    return e;
}

derr_t sender_timer_cb(kvpsend_t *k, sender_t *sender, xtime_t now){
    derr_t e = E_OK;

    sender->deadline = XTIME_MAX;
    PROP(&e, advance_sender(k, sender, now) );

    return e;
}

static void clear_timer(sender_t *sender){
    if(sender->deadline == XTIME_MAX) return;
    kvpsend_i *I = sender->k->I;
    I->sender_timer_stop(I, sender);
    sender->deadline = XTIME_MAX;
}

static derr_t advance_sender(kvpsend_t *k, sender_t *sender, xtime_t now){
    derr_t e = E_OK;

    kvpsend_i *I = k->I;

    // nothing to do while our write packet is in flight
    if(sender->inwrite) return e;
    kvpsync_run_t run = kvpsync_send_run(&sender->send, now);
    if(run.pkt){
        clear_timer(sender);
        // send a packet
        sender->inwrite = true;
        PROP(&e, I->sender_send_pkt(I, sender, run.pkt) );
    }else if(run.deadline != sender->deadline){
        clear_timer(sender);
        // wait for the next packet to be ready
        I->sender_timer_start(I, sender, run.deadline);
        sender->deadline = run.deadline;
    }

    return e;
}

static derr_t advance_all_senders(kvpsend_t *k, xtime_t now){
    derr_t e = E_OK;
    for(size_t i = 0; i < k->nsenders; i++){
        PROP(&e, advance_sender(k, &k->senders[i], now) );
    }
    return e;
}

derr_t sender_recv_cb(
    kvpsend_t *k, sender_t *sender, kvp_ack_t ack, xtime_t now
){
    derr_t e = E_OK;

    kvpsync_send_handle_ack(&sender->send, ack, now);
    PROP(&e, advance_sender(k, sender, now) );

    return e;
}

// failures here indicate application-level failure
derr_t scan_timer_cb(kvpsend_t *k, xtime_t now){
    derr_t e = E_OK;

    PROP(&e, full_scan(k, now) );

    PROP(&e, advance_all_senders(k, now) );

    // set up the next full scan
    kvpsend_i *I = k->I;
    I->scan_timer_start(I, now + FULL_SCAN_PERIOD);

    return e;
}

void configure_sub_timeout(kvpsend_t *k){
    kvpsend_i *I = k->I;
    xtime_t new_deadline;
    if(link_list_isempty(&k->allsubs)){
        new_deadline = XTIME_MAX;
    }else{
        subscriber_t *sub = CONTAINER_OF(k->allsubs.next, subscriber_t, tlink);
        new_deadline = sub->deadline;
    }
    // do we need to change the timeout deadline?
    if(new_deadline == k->next_timeout) return;
    // do we need to clear an old timer?
    if(k->next_timeout != XTIME_MAX){
        I->timeout_timer_stop(I);
    }
    // do we need to set a new timer?
    if(new_deadline != XTIME_MAX){
        I->timeout_timer_start(I, new_deadline);
    }
    // remember what we set
    k->next_timeout = new_deadline;
}

// failures here indicate application-level failure
derr_t timeout_timer_cb(kvpsend_t *k, xtime_t now){
    derr_t e = E_OK;

    k->next_timeout = XTIME_MAX;

    // discard any timed-out subscribers
    kvpsend_i *I = k->I;
    while(!link_list_isempty(&k->allsubs)){
        subscriber_t *sub = CONTAINER_OF(k->allsubs.next, subscriber_t, tlink);
        if(sub->deadline > now) break;
        // this subscriber timed out
        link_remove(&sub->link);
        link_remove(&sub->tlink);
        I->subscriber_respond(I, sub, DSTR_LIT("t"));
    }

    configure_sub_timeout(k);

    return e;
}

// failures here indicate application-level failure
derr_t initial_actions(kvpsend_t *k, xtime_t now){
    derr_t e = E_OK;

    for(size_t i = 0; i < k->nsenders; i++){
        PROP(&e, advance_sender(k, &k->senders[i], now) );
    }

    // set up the next full scan
    kvpsend_i *I = k->I;
    I->scan_timer_start(I, now + FULL_SCAN_PERIOD);

    return e;
}
