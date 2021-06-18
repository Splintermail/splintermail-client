#include "citm.h"

static void user_close(user_t *user, derr_t error){
    bool do_close = !user->closed;
    user->closed = true;

    if(!do_close){
        // secondary errors are dropped
        DROP_VAR(&error);
        return;
    }

    // close everything
    link_t *link;
    while((link = link_list_pop_first(&user->sf_pairs))){
        sf_pair_t *sf_pair = CONTAINER_OF(link, sf_pair_t, user_link);
        sf_pair_close(sf_pair, E_OK);
    }

    keysync_close(&user->keysync, E_OK);

    // pass the error to our manager
    user->cb->dying(user->cb, user, error);

    // drop the lifetime reference
    ref_dn(&user->refs);
}

static void user_finalize(refs_t *refs){
    user_t *user = CONTAINER_OF(refs, user_t, refs);

    keysync_free(&user->keysync);
    dirmgr_free(&user->dirmgr);
    keyshare_free(&user->keyshare);
    keypair_free(&user->my_keypair);
    fpr_watcher_free(&user->fpr_watcher);
    dstr_free(&user->name);
    dstr_free(&user->pass);
    refs_free(&user->refs);
    free(user);
}

static derr_t gen_msg_header(
    const dstr_t subj, dstr_t *out, imap_time_t *intdate
){
    derr_t e = E_OK;

    *intdate = (imap_time_t){0};

    // get the current time
    time_t epoch;
    PROP(&e, dtime(&epoch) );
    // c99 doesn't allow for the rentrant localtime_r(), and its not a big deal
    struct tm* tret = localtime(&epoch);
    if(tret == NULL){
        TRACE(&e, "%x: %x\n", FS("localtime"), FE(&errno));
        ORIG(&e, E_OS, "error converting epoch time to time struct");
    }
    struct tm tnow = *tret;
    // print human-readable date to a buffer
    char d[128];
    size_t len;
    len = strftime(d, sizeof(d), "%a, %d %b %Y %H:%M:%S %z", &tnow);
    if(len == 0){
        TRACE(&e, "%x: %x\n", FS("strftime"), FE(&errno));
        ORIG(&e, E_OS, "error formatting time string");
    }

    long int tz;
    PROP(&e, dtimezone(&tz) );

    PROP(&e,
        FMT(out,
            "From: DITM <ditm@localhost>\r\n"
            "To: Local User <email_user@localhost>\r\n"
            "Date: %x\r\n"
            "Subject: %x\r\n"
            "\r\n",
            FS(d),
            FD(&subj)
        )
    );


    // libc defines timezone as negative of what it should be
    long int tz_hour = -(int)(tz/60/60);
    // do mod-math on the positive-signed value
    long int tz_pos = tz * (tz > 0 ? 1 : -1);
    long int tz_min = (int)((tz_pos % 60*60) / 60);

    // imap_time_t is 1-indexed for some values
    *intdate = (imap_time_t){
        .year  = tnow.tm_year,
        .month = tnow.tm_mon + 1,  // imap_time_t is 1-indexed
        .day = tnow.tm_mday,  // struct tm is also 1-indexed
        .hour = tnow.tm_hour,
        .min = tnow.tm_min,
        .sec = tnow.tm_sec,
        .z_hour = (int)tz_hour,
        .z_min = (int)tz_min,
    };

    return e;
}

static derr_t inject_msg(
    user_t *user, const dstr_t content, const imap_time_t intdate
){
    string_builder_t path = sb_append(&user->path, FS("injected_msg.tmp"));
    dirmgr_hold_t *hold = NULL;
    imaildir_t *m = NULL;

    derr_t e = E_OK;

    PROP_GO(&e, dstr_write_path(&path, &content), cu);

    PROP_GO(&e, dirmgr_hold_new(&user->dirmgr, &DSTR_LIT("INBOX"), &hold), cu);
    PROP_GO(&e, dirmgr_hold_get_imaildir(hold, &m), cu);

    PROP_GO(&e,
        imaildir_add_local_file(m,
            &path,
            0,
            content.len,
            intdate,
            (msg_flags_t){0},
            NULL
        ),
    cu);

cu:
    dirmgr_hold_release_imaildir(hold, &m);
    dirmgr_hold_free(hold);
    DROP_CMD( remove_path(&path) );

    return e;
}

static derr_t inject_new_key_msg(user_t *user, const dstr_t fpr_bin){
    dstr_t content = {0};

    derr_t e = E_OK;

    // add the header
    imap_time_t intdate;
    DSTR_STATIC(subj, "New Device Detected");
    PROP_GO(&e, gen_msg_header(subj, &content, &intdate), cu);

    // add the body (wrapped to 72 chars)
    const char *body_fmt_str =
"The Splintermail software running on your device has detected that a new\r\n"
"device has been added to your account.\r\n"
"\r\n"
"The device which was added has the following fingerprint:\r\n"
"\r\n"
"    %x\r\n"
"\r\n"
"If you recently installed Splintermail on a new device or uninstalled it\r\n"
"on an old one, no further action is required.\r\n"
"\r\n"
"If you have not recently installed Splintermail on a new device or\r\n"
"re-installed it on an old one, this may mean that your password has been\r\n"
"compromised.  If you believe that to be true, you should take the\r\n"
"following steps to protect your account:\r\n"
"\r\n"
"  1. Visit your account page at https://splintermail.com\r\n"
"\r\n"
"  2. Change your password.  If somebody had your old password, this will\r\n"
"     prevent them from registering new devices to your account.\r\n"
"\r\n"
"  3. Click the \"delete\" button next to each API Token and Device on\r\n"
"     your account page.  If somebody had your old password, this will\r\n"
"     revoke any access they had previously.\r\n"
"\r\n"
"  4. On each of your devices, update your email client with your new\r\n"
"     password and check your email, which will automatically reregister\r\n"
"     each device.\r\n"
"\r\n"
"Thank you,\r\n"
"\r\n"
"    Your local Splintermail software\r\n"
"\r\n"
"Note: this message was generated by the Splintermail software running on\r\n"
"your own device.  This message did not originate from the mail server,\r\n"
"and deleting this message on this device will not delete it on any other\r\n"
"devices.  This is for your protection, as it guarantees that we at\r\n"
"Splintermail cannot be coerced into adding encryption keys to your\r\n"
"account without your knowledge.\r\n";

    PROP_GO(&e, FMT(&content, body_fmt_str, FX(&fpr_bin)), cu);

    // inject the message
    PROP_GO(&e, inject_msg(user, content, intdate), cu);

cu:
    dstr_free(&content);

    return e;
}

// keysync_cb_i functions

static void user_keysync_dying(keysync_cb_i *keysync_cb, derr_t error){
    user_t *user = CONTAINER_OF(keysync_cb, user_t, keysync_cb);
    // TODO: maybe find a way to express to the user what happened?

    // nothing is valid without the keysync
    user_close(user, error);
}

static void user_keysync_release(keysync_cb_i *keysync_cb){
    user_t *user = CONTAINER_OF(keysync_cb, user_t, keysync_cb);

    // keysync ref
    ref_dn(&user->refs);
}

static derr_t user_keysync_synced(keysync_cb_i *keysync_cb){
    derr_t e = E_OK;

    user_t *user = CONTAINER_OF(keysync_cb, user_t, keysync_cb);

    // ignore multiple calls
    if(user->initial_keysync_complete){
        return e;
    }
    user->initial_keysync_complete = true;

    PROP(&e, fpr_watcher_xkeysync_completed(&user->fpr_watcher) );

    sf_pair_t *sf_pair;
    LINK_FOR_EACH(sf_pair, &user->sf_pairs, sf_pair_t, user_link){
        // respond to each sf_pair that we own
        sf_pair_owner_resp(sf_pair, &user->dirmgr, &user->keyshare);
    }

    return e;
}

static derr_t _write_key(user_t *user, const keypair_t *kp){
    derr_t e = E_OK;

    // get the pem-encoded key
    DSTR_VAR(pem, 4096);
    PROP(&e, keypair_get_public_pem(kp, &pem) );

    // filename is just "HEX_FPR.pem"
    DSTR_VAR(file, 256);
    PROP(&e, FMT(&file, "%x.pem", FX(kp->fingerprint)) );

    string_builder_t path = sb_append(&user->key_path, FD(&file) );
    PROP(&e, dstr_write_path(&path, &pem) );

    return e;
}

// key_created must consume or free kp
static derr_t user_keysync_key_created(
    keysync_cb_i *keysync_cb, keypair_t **kp
){
    derr_t e = E_OK;
    user_t *user = CONTAINER_OF(keysync_cb, user_t, keysync_cb);

    // when necessary, always alert the user first
    const dstr_t fpr = *(*kp)->fingerprint;
    bool alert = fpr_watcher_should_alert_on_new_key(&user->fpr_watcher, fpr);
    if(alert) PROP_GO(&e, inject_new_key_msg(user, fpr), cu);
    PROP_GO(&e, fpr_watcher_add_fpr(&user->fpr_watcher, fpr), cu);

    PROP_GO(&e, keyshare_add_key(&user->keyshare, *kp), cu);

    IF_PROP(&e, _write_key(user, *kp) ){
        // just log the error but continue
        TRACE(&e, "failed to write key for future reuse\n");
        DUMP(e);
        DROP_VAR(&e);
    }

cu:
    keypair_free(kp);

    return e;
}

static derr_t _rm_key(user_t *user, const dstr_t *fpr){
    derr_t e = E_OK;

    DSTR_VAR(file, 256);
    PROP(&e, FMT(&file, "%x.pem", FD(fpr)) );

    string_builder_t path = sb_append(&user->key_path, FD(&file) );
    PROP(&e, rm_rf_path(&path) );

    return e;
}

static void user_keysync_key_deleted(
    keysync_cb_i *keysync_cb, const dstr_t *fpr
){
    user_t *user = CONTAINER_OF(keysync_cb, user_t, keysync_cb);

    keyshare_del_key(&user->keyshare, fpr);

    derr_t e = E_OK;
    IF_PROP(&e, _rm_key(user, fpr) ){
        // just log the error but continue
        TRACE(&e, "failed to delete obsolete key from files\n");
        DUMP(e);
        DROP_VAR(&e);
    }
}

// end keysync_cb_i functions

// begin imaildir_hooks_i functions

static derr_t decrypt_msg(
    user_t *user,
    const dstr_t *mailbox,
    const dstr_t *cipher,
    const string_builder_t *path,
    size_t *len
){
    dstr_t copy = {0};
    dstr_t plain = {0};
    decrypter_t dc = {0};
    LIST(dstr_t) recips = {0};
    dstr_t block = {0};

    derr_t e = E_OK;

    // TODO: fix decrypter_t API to support const input strings
    // copy the content, just to work around the stream-only API of decrypter_t
    PROP_GO(&e, dstr_copy(cipher, &copy), cu);

    PROP_GO(&e, dstr_new(&plain, cipher->len), cu);

    PROP_GO(&e, LIST_NEW(dstr_t, &recips, 32), cu);
    PROP_GO(&e, dstr_new(&block, 1024), cu);

    // create the decrypter
    PROP_GO(&e, decrypter_new(&dc), cu);
    PROP_GO(&e, decrypter_start(&dc, user->my_keypair, &recips, &block), cu);

    // decrypt the message
    PROP_GO(&e, decrypter_update(&dc, &copy, &plain), cu);
    PROP_GO(&e, decrypter_finish(&dc, &plain), cu);

    // detect unrecognized fingerprints first
    for(size_t i = 0; i < recips.len; i++){
        const dstr_t recip = recips.data[i];
        bool alert = fpr_watcher_should_alert_on_decrypt(
            &user->fpr_watcher, recip, *mailbox
        );
        if(alert) PROP_GO(&e, inject_new_key_msg(user, recip), cu);
        PROP_GO(&e, fpr_watcher_add_fpr(&user->fpr_watcher, recip), cu);
    }

    if(len) *len = plain.len;

    // write the file to disk
    PROP_GO(&e, dstr_write_path(path, &plain), cu);

cu:
    decrypter_free(&dc);
    dstr_free(&block);
    LIST_FREE(dstr_t, &recips);
    dstr_free(&plain);
    dstr_free(&copy);

    return e;
}

// mangle subject line of unencrypted messages to show there was a problem
static derr_t mangle_unencrypted(
    const dstr_t *msg,
    const string_builder_t *path,
    size_t *len
){
    derr_t e = E_OK;
    if(len) *len = 0;

    // subject line if none was provided:
    DSTR_STATIC(subj_entire, "Subject: NOT ENCRYPTED: (no subject)");

    // patterns for either either the subject line or end-of-headers
    LIST_PRESET(
        dstr_t,
        subj,
        DSTR_LIT("\nSubject:"),
        DSTR_LIT("\r\n\r\n"),
        DSTR_LIT("\n\n")
    );

    // search for the patterns
    size_t which;
    size_t partial;
    char* pos = dstr_find(msg, &subj, &which, &partial);
    if(!pos){
        /* if we didn't find the end of headers, the message is fucked; just
           leave it alone */
        PROP(&e, dstr_write_path(path, msg) );
        if(len) *len = msg->len;
        return e;
    }

    dstr_t copy;
    PROP(&e, dstr_new(&copy, msg->len + subj_entire.len) );

    // if we found the end-of-headers but not the subject, insert fake subject
    if(which == 1 || which == 2){
        // if we didn't find a subject, insert one
        size_t headers_end = (uintptr_t)(pos - msg->data);
        // write to the end of headers
        dstr_t pre = dstr_sub2(*msg, 0, headers_end);
        PROP_GO(&e, dstr_append(&copy, &pre), cu);
        // get the native len break
        dstr_t nlb = dstr_sub2(
            subj.data[which], 0, subj.data[which].len / 2
        );
        PROP_GO(&e, dstr_append(&copy, &nlb), cu);
        // write the missing subject line
        PROP_GO(&e, dstr_append(&copy, &subj_entire), cu);
        // write the rest of the msg
        dstr_t post = dstr_sub2(*msg, headers_end, msg->len);
        PROP_GO(&e, dstr_append(&copy, &post), cu);
    }
    // if we found the subject line, mangle it
    else if(which == 0){
        size_t subj_end = (uintptr_t)(pos - msg->data) + subj.data[which].len;
        // write to the end of "Subject:"
        dstr_t pre = dstr_sub2(*msg, 0, subj_end);
        PROP_GO(&e, dstr_append(&copy, &pre), cu);
        // write the warning
        PROP_GO(&e, dstr_append(&copy, &DSTR_LIT(" NOT ENCRYPTED:")), cu);
        // write the rest of the message
        dstr_t post = dstr_sub2(*msg, subj_end, msg->len);
        PROP_GO(&e, dstr_append(&copy, &post), cu);
    }

    PROP_GO(&e, dstr_write_path(path, &copy), cu);

    if(len) *len = copy.len;

cu:
    dstr_free(&copy);
    return e;
}

// when decrypt_msg fails, we give the user the broken message
static derr_t mangle_corrupted(
    const dstr_t *msg,
    const string_builder_t *path,
    size_t *len
){
    derr_t e = E_OK;
    if(len) *len = 0;

    // get the current time
    time_t epoch = time(NULL);
    // c99 doesn't allow for the rentrant localtime_r(), and its not a big deal
    struct tm* tret = localtime(&epoch);
    if(tret == NULL){
        TRACE(&e, "%x: %x\n", FS("localtime"), FE(&errno));
        ORIG(&e, E_INTERNAL, "error converting epoch time to time struct");
    }
    struct tm tnow = *tret;
    // print human-readable date to a buffer
    char d[128];
    size_t dlen;
    dlen = strftime(d, sizeof(d), "%a, %d %b %Y %H:%M:%S %z", &tnow);
    if(dlen == 0){
        TRACE(&e, "%x: %x\n", FS("strftime"), FE(&errno));
        ORIG(&e, E_INTERNAL, "error formatting time string");
    }

    DSTR_STATIC(
        fmtstr,
        "From: CITM <citm@localhost>\r\n"
        "To: Local User <email_user@localhost>\r\n"
        "Date: %x\r\n"
        "Subject: CITM failed to decrypt message\r\n"
        "\r\n"
        "The following message appears to be corrupted"
        " and cannot be decrypted:\r\n"
        "\r\n"
    );

    dstr_t copy;
    PROP(&e, dstr_new(&copy, msg->len + (fmtstr.len - 2) + dlen) );

    // dump headers to message
    PROP_GO(&e, FMT(&copy, fmtstr.data, FS(d)), cu);

    // dump original message as the body
    PROP_GO(&e, dstr_append(&copy, msg), cu);

    // write file
    PROP_GO(&e, dstr_write_path(path, &copy), cu);

    if(len) *len = copy.len;

cu:
    dstr_free(&copy);
    return e;
}


// inject the citm logic into the the imaildir_hooks_i
static derr_t imaildir_hooks_process_msg(
    imaildir_hooks_i *hooks,
    const dstr_t *mailbox,
    const string_builder_t *path,
    const dstr_t *content,
    size_t *len,
    bool *ignore
){
    derr_t e = E_OK;
    user_t *user = CONTAINER_OF(hooks, user_t, imaildir_hooks);
    *len = 0;
    *ignore = false;

    // detect if the message is even encrypted
    DSTR_STATIC(enc_header, "-----BEGIN SPLINTERMAIL MESSAGE-----");
    bool encrypted = dstr_beginswith(content, &enc_header);
    if(encrypted){
        // do the decryption
        derr_t e2 = decrypt_msg(user, mailbox, content, path, len);
        CATCH(e2, E_NOT4ME){
            LOG_INFO("detected NOT4ME message\n");
            DROP_VAR(&e2);
            *ignore = true;
        }else CATCH(e2, E_SSL, E_PARAM){
            // decryption errors, pass the broken message to the user
            DROP_VAR(&e2);
            PROP(&e, mangle_corrupted(content, path, len) );
        }else PROP_VAR(&e, &e2);
    }else{
        // message is not even encrypted
        PROP(&e, mangle_unencrypted(content, path, len) );
    }

    return e;
}

// end imaildir_hooks_i functions

static derr_t _load_or_gen_mykey(
    const string_builder_t *key_path, keypair_t **out
){
    derr_t e = E_OK;

    PROP(&e, mkdirs_path(key_path, 0700) );

    string_builder_t mykey_path = sb_append(key_path, FS("mykey.pem"));

    bool have_key;
    PROP(&e, exists_path(&mykey_path, &have_key) );

    if(have_key){
        IF_PROP(&e, keypair_load_path(out, &mykey_path) ){
            // we must have hit an error reading the key
            TRACE(&e, "failed to load mykey...\n");
            DUMP(e);
            DROP_VAR(&e);
            LOG_ERROR("Failed to load mykey, generating a new one.\n");
            // delete the broken key
            PROP(&e, rm_rf_path(&mykey_path) );
        }else{
            // key was loaded successfully
            return e;
        }
    }

    PROP(&e, gen_key_path(4096, &mykey_path) );
    PROP(&e, keypair_load_path(out, &mykey_path) );

    return e;
}

// add_key_to_keyshare is a for_each_file_hook2_t
static derr_t add_key_to_keyshare(
    const string_builder_t* base,
    const dstr_t* file,
    bool isdir,
    void* userdata
){
    derr_t e = E_OK;
    keyshare_t *keyshare = userdata;

    if(isdir) return e;
    if(!dstr_endswith(file, &DSTR_LIT(".pem"))) return e;

    string_builder_t path = sb_append(base, FD(file));
    keypair_t *keypair;
    IF_PROP(&e, keypair_load_path(&keypair, &path) ){
        // we must have hit an error reading the key
        TRACE(&e,
            "deleting broken key '%x' after failure:\n",
            FSB(&path, &DSTR_LIT("/"))
        );
        DUMP(e);
        DROP_VAR(&e);
        // delete the broken key and exit
        PROP(&e, rm_rf_path(&path) );
        return e;
    }

    PROP_GO(&e, keyshare_add_key(keyshare, keypair), cu);

cu:
    keypair_free(&keypair);

    return e;
}


derr_t user_new(
    user_t **out,
    user_cb_i *cb,
    imap_pipeline_t *p,       // passthru to the keysync
    ssl_context_t *ctx_cli,   // passthru to the keysync
    const char *remote_host,  // passthru to the keysync
    const char *remote_svc,   // passthru to the keysync
    engine_t *engine,         // passthru to the keysync
    const dstr_t *name,
    const dstr_t *pass,
    const string_builder_t *root
){
    derr_t e = E_OK;

    *out = NULL;

    user_t *user = malloc(sizeof(*user));
    if(!user) ORIG(&e, E_NOMEM, "nomem");
    *user = (user_t){
        .cb = cb,
        .keysync_cb = {
            .dying = user_keysync_dying,
            .release = user_keysync_release,
            .synced = user_keysync_synced,
            .key_created = user_keysync_key_created,
            .key_deleted = user_keysync_key_deleted,
        },
        .imaildir_hooks = {
            .process_msg = imaildir_hooks_process_msg,
        },
    };

    link_init(&user->sf_pairs);

    // start with a lifetime ref
    PROP_GO(&e, refs_init(&user->refs, 1, user_finalize), fail_malloc);

    PROP_GO(&e, dstr_copy(name, &user->name), fail_refs);
    PROP_GO(&e, dstr_copy(pass, &user->pass), fail_name);

    user->path = sb_append(root, FD(&user->name));
    user->mail_path = sb_append(&user->path, FS("mail"));
    user->key_path = sb_append(&user->path, FS("keys"));

    // string_builder_t sb = sb_append(&user->path, FS("fingerprints"));
    // PFMT("initializing watcher: %x\n", FSB(&sb, &DSTR_LIT("/")));
    PROP_GO(&e,
        fpr_watcher_init(
            &user->fpr_watcher,
            sb_append(&user->path, FS("fingerprints"))
        ),
    fail_pass);

    // load or create my_keypair
    PROP_GO(&e,
        _load_or_gen_mykey(&user->key_path, &user->my_keypair),
    fail_fpr_watcher);

    // populate keyshare
    PROP_GO(&e, keyshare_init(&user->keyshare), fail_keypair);
    PROP_GO(&e,
        for_each_file_in_dir2(
            &user->key_path,
            add_key_to_keyshare,
            &user->keyshare
        ),
    fail_keyshare);

    // init keysync
    PROP_GO(&e,
        keysync_init(
            &user->keysync,
            &user->keysync_cb,
            p,
            ctx_cli,
            remote_host,
            remote_svc,
            engine,
            &user->name,
            &user->pass,
            user->my_keypair,
            &user->keyshare
        ),
    fail_keyshare);

    /* the dirmgr does not need the whole keyshare; it's the sf_pair that does
       the encryption */
    PROP_GO(&e,
        dirmgr_init(&user->dirmgr, user->mail_path, &user->imaildir_hooks),
    fail_keyshare);

    *out = user;

    // start the keysync immediately
    keysync_start(&user->keysync);
    // keysync ref
    ref_up(&user->refs);

    return e;

fail_keyshare:
    keyshare_free(&user->keyshare);
fail_keypair:
    keypair_free(&user->my_keypair);
fail_fpr_watcher:
    fpr_watcher_free(&user->fpr_watcher);
fail_pass:
    dstr_free(&user->pass);
fail_name:
    dstr_free(&user->name);
fail_refs:
    refs_free(&user->refs);
fail_malloc:
    free(user);
    return e;
}


void user_add_sf_pair(user_t *user, sf_pair_t *sf_pair){
    link_remove(&sf_pair->user_link);
    link_list_append(&user->sf_pairs, &sf_pair->user_link);
    user->npairs++;
    // ref up for sf_pair
    ref_up(&user->refs);

    sf_pair->owner = user;

    /* if the keysync has finished its initial sync, we can respond to the
       sf_pair immediately.  If not, then we will respond to all of them when
       we have finished the initial sync */
    if(user->initial_keysync_complete){
        sf_pair_owner_resp(sf_pair, &user->dirmgr, &user->keyshare);
    }
}


// this gets called by the citme's sf_pair_cb
void user_remove_sf_pair(user_t *user, sf_pair_t *sf_pair){

    if(!user->closed){
        link_remove(&sf_pair->user_link);
    }

    if(--user->npairs == 0){
        user_close(user, E_OK);
    }

    // don't ref_dn until sf_pair is dead (happens in citm_engine.c)
}

derr_t user_mailbox_synced(user_t *user, const dstr_t mailbox){
    derr_t e = E_OK;

    PROP(&e, fpr_watcher_mailbox_synced(&user->fpr_watcher, mailbox) );

    return e;
}
