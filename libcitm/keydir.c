#include "libcitm/libcitm.h"

#include <string.h>

/* keydir_t is the default keydir_i that uses an actual directory of keys.

   In addition to the responsibilities that the keydir_i implies, the keydir_t
   also holds the fpr_watcher and the dirmgr, decrypts messages, and injects
   local messages into the INBOX for new keys or decryption anomalies. */
typedef struct {
    keydir_i iface;
    hash_elem_t elem;  // citm_t->users

    // login name, backed by external memory
    dstr_t user;
    // the root directory for this user
    string_builder_t path;
    string_builder_t mail_path;
    string_builder_t key_path;

    keypair_t *mykey;
    link_t peers;
    // all_keys holds a copy of mykey and each peer
    link_t all_keys;

    // logic for client-side 'new key' checks
    fpr_watcher_t fpr_watcher;

    // this user's dirmgr
    dirmgr_t dirmgr;

    // how we inject the citm decryption logic into the imaildir_t
    imaildir_hooks_i imaildir_hooks;
} keydir_t;
DEF_CONTAINER_OF(keydir_t, iface, keydir_i)
DEF_CONTAINER_OF(keydir_t, elem, hash_elem_t)
DEF_CONTAINER_OF(keydir_t, imaildir_hooks, imaildir_hooks_i)

static void keydir_free(keydir_t *kd){
    if(!kd) return;
    hash_elem_remove(&kd->elem);
    link_t *link;
    while((link_list_pop_first(&kd->peers))){
        keypair_t *temp = CONTAINER_OF(link, keypair_t, link);
        keypair_free(&temp);
    }
    // this will also free mykey, which is always in all_keys
    while((link_list_pop_first(&kd->all_keys))){
        keypair_t *temp = CONTAINER_OF(link, keypair_t, link);
        keypair_free(&temp);
    }
    fpr_watcher_free(&kd->fpr_watcher);
    dirmgr_free(&kd->dirmgr);
    free(kd);
}

static derr_t gen_msg_header(
    const dstr_t subj, dstr_t *out, imap_time_t *intdate
){
    derr_t e = E_OK;

    time_t epoch;
    e = dtime(&epoch);
    CATCH(e, E_ANY){
        TRACE(&e, "ignoring failure of dtime()\n");
        DUMP(e);
        DROP_VAR(&e);
        epoch = 0;
    }

    char buf[128];
    const char *date_field = get_date_field(buf, sizeof(buf), epoch);
    *intdate = imap_time_now(epoch);

    PROP(&e,
        FMT(out,
            "From: CITM <citm@localhost>\r\n"
            "To: Local User <email_user@localhost>\r\n"
            "Date: %x\r\n"
            "Subject: %x\r\n"
            "\r\n",
            FS(date_field),
            FD(&subj)
        )
    );

    return e;
}

static derr_t inject_msg(
    keydir_t *kd, const dstr_t content, const imap_time_t intdate
){
    string_builder_t path = sb_append(&kd->path, FS("injected_msg.tmp"));
    dirmgr_hold_t *hold = NULL;
    imaildir_t *m = NULL;

    derr_t e = E_OK;

    PROP_GO(&e, dstr_write_path(&path, &content), cu);

    PROP_GO(&e, dirmgr_hold_new(&kd->dirmgr, &DSTR_LIT("INBOX"), &hold), cu);
    PROP_GO(&e, dirmgr_hold_get_imaildir(hold, &m), cu);

    PROP_GO(&e,
        imaildir_add_local_file(m,
            &path,
            0,
            content.len,
            intdate,
            (msg_flags_t){0},
            NULL,
            NULL
        ),
    cu);

cu:
    dirmgr_hold_release_imaildir(hold, &m);
    dirmgr_hold_free(hold);
    DROP_CMD( remove_path(&path) );

    return e;
}

static derr_t inject_new_key_msg(keydir_t *kd, const dstr_t fpr_bin){
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
"If you recently installed Splintermail on a new device or re-installed\r\n"
"it on an old one, no further action is required.\r\n"
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
    PROP_GO(&e, inject_msg(kd, content, intdate), cu);

cu:
    dstr_free(&content);

    return e;
}

static const keypair_t *kd_mykey(keydir_i *iface){
    keydir_t *kd = CONTAINER_OF(iface, keydir_t, iface);
    return kd->mykey;
}

static link_t *kd_peers(keydir_i *iface){
    keydir_t *kd = CONTAINER_OF(iface, keydir_t, iface);
    return &kd->peers;
}

static derr_t _write_key(keydir_t *kd, const dstr_t pem, const dstr_t bin_fpr){
    derr_t e = E_OK;

    // filename is just "HEX_FPR.pem"
    DSTR_VAR(file, 256);
    PROP(&e, FMT(&file, "%x.pem", FX(&bin_fpr)) );

    string_builder_t path = sb_append(&kd->key_path, FD(&file) );
    PROP(&e, dstr_write_path(&path, &pem) );

    return e;
}

static derr_t kd_add_key(keydir_i *iface, const dstr_t pem){
    derr_t e = E_OK;
    keydir_t *kd = CONTAINER_OF(iface, keydir_t, iface);

    keypair_t *kp = NULL;

    PROP(&e, keypair_from_pubkey_pem(&kp, pem) );

    // when necessary, always alert the user first
    const dstr_t fpr = *kp->fingerprint;
    bool alert = fpr_watcher_should_alert_on_new_key(&kd->fpr_watcher, fpr);
    if(alert) PROP_GO(&e, inject_new_key_msg(kd, fpr), fail);
    PROP_GO(&e, fpr_watcher_add_fpr(&kd->fpr_watcher, fpr), fail);

    PROP_GO(&e, _write_key(kd, pem, fpr), fail);

    keypair_t *cpy = NULL;
    PROP_GO(&e, keypair_copy(kp, &cpy), fail);

    link_list_append(&kd->peers, &kp->link);
    link_list_append(&kd->all_keys, &cpy->link);

    return e;

fail:
    keypair_free(&kp);
    return e;
}

static derr_t _rm_key(keydir_t *kd, const dstr_t bin_fpr){
    derr_t e = E_OK;

    DSTR_VAR(file, 256);
    PROP(&e, FMT(&file, "%x.pem", FX(&bin_fpr)) );

    string_builder_t path = sb_append(&kd->key_path, FD(&file) );
    PROP(&e, rm_rf_path(&path) );

    return e;
}

static void kd_delete_key(keydir_i *iface, const dstr_t bin_fpr){
    keydir_t *kd = CONTAINER_OF(iface, keydir_t, iface);
    link_t *heads[] = {&kd->peers, &kd->all_keys};
    for(size_t i = 0; i < sizeof(heads)/sizeof(*heads); i++){
        keypair_t *kp, *temp;
        LINK_FOR_EACH_SAFE(kp, temp, heads[i], keypair_t, link){
            if(!dstr_eq(*kp->fingerprint, bin_fpr)) continue;
            link_remove(&kp->link);
            keypair_free(&kp);
            break;
        }
    }

    derr_t e = E_OK;
    IF_PROP(&e, _rm_key(kd, bin_fpr) ){
        // just log the error but continue
        TRACE(&e, "failed to delete obsolete key from files\n");
        DUMP(e);
        DROP_VAR(&e);
    }
}

static void kd_free(keydir_i *iface){
    keydir_t *kd = CONTAINER_OF(iface, keydir_t, iface);
    keydir_free(kd);
}

static dirmgr_t *kd_dirmgr(keydir_i *iface){
    keydir_t *kd = CONTAINER_OF(iface, keydir_t, iface);
    return &kd->dirmgr;
}

static link_t *kd_all_keys(keydir_i *iface){
    keydir_t *kd = CONTAINER_OF(iface, keydir_t, iface);
    return &kd->all_keys;
}

static derr_t kd_mailbox_synced(keydir_i *iface, const dstr_t mailbox){
    derr_t e = E_OK;
    keydir_t *kd = CONTAINER_OF(iface, keydir_t, iface);

    PROP(&e, fpr_watcher_mailbox_synced(&kd->fpr_watcher, mailbox) );

    return e;
}

/* not part of the interface because it is only called by the citm object
   between the preuser-to-user transition */
derr_t keydir_keysync_completed(keydir_i *iface){
    derr_t e = E_OK;
    keydir_t *kd = CONTAINER_OF(iface, keydir_t, iface);

    PROP(&e, fpr_watcher_xkeysync_completed(&kd->fpr_watcher) );

    return e;
}

// begin imaildir_hooks_i functions

static derr_t decrypt_msg(
    keydir_t *kd,
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
    PROP_GO(&e, decrypter_start(&dc, kd->mykey, &recips, &block), cu);

    // decrypt the message
    PROP_GO(&e, decrypter_update(&dc, &copy, &plain), cu);
    PROP_GO(&e, decrypter_finish(&dc, &plain), cu);

    // detect unrecognized fingerprints first
    for(size_t i = 0; i < recips.len; i++){
        const dstr_t recip = recips.data[i];
        bool alert = fpr_watcher_should_alert_on_decrypt(
            &kd->fpr_watcher, recip, *mailbox
        );
        if(alert) PROP_GO(&e, inject_new_key_msg(kd, recip), cu);
        PROP_GO(&e, fpr_watcher_add_fpr(&kd->fpr_watcher, recip), cu);
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
    char *pos;
    bool beginswith = false;
    if(dstr_beginswith(msg, &DSTR_LIT("Subject:"))){
        // special case; handle a Subject on the first line
        pos = msg->data;
        which = 0;
        beginswith = true;
    }else{
        pos = dstr_find(msg, &subj, &which, &partial);
        if(!pos){
            /* if we didn't find the end of headers, the message is fucked;
               just leave it alone */
            PROP(&e, dstr_write_path(path, msg) );
            if(len) *len = msg->len;
            return e;
        }
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
        if(beginswith){
            // special case: account for the '\n' not present in the pattern
            subj_end -= 1;
        }
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

    char buf[128];
    const char *date_field = get_date_field(buf, sizeof(buf), (time_t)-1);
    size_t dlen = strlen(date_field);

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
    PROP_GO(&e, FMT(&copy, fmtstr.data, FS(date_field)), cu);

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
    bool *not4me
){
    derr_t e = E_OK;
    keydir_t *kd = CONTAINER_OF(hooks, keydir_t, imaildir_hooks);
    *len = 0;
    *not4me = false;

    // detect if the message is even encrypted
    DSTR_STATIC(enc_header, "-----BEGIN SPLINTERMAIL MESSAGE-----");
    bool encrypted = dstr_beginswith(content, &enc_header);
    if(encrypted){
        // do the decryption
        derr_t e2 = decrypt_msg(kd, mailbox, content, path, len);
        CATCH(e2, E_NOT4ME){
            LOG_INFO("detected NOT4ME message\n");
            DROP_VAR(&e2);
            *not4me = true;
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
        IF_PROP(&e, keypair_load_private_path(out, &mykey_path) ){
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
    PROP(&e, keypair_load_private_path(out, &mykey_path) );

    return e;
}

// add_peer_key is a for_each_file_hook_t
static derr_t add_peer_key(
    const string_builder_t* base,
    const dstr_t* file,
    bool isdir,
    void* userdata
){
    derr_t e = E_OK;
    keydir_t *kd = userdata;

    if(isdir) return e;
    if(!dstr_endswith(file, &DSTR_LIT(".pem"))) return e;
    // skip mykey, which is handled elsewhere
    if(!dstr_eq(*file, DSTR_LIT("mykey.pem"))) return e;

    string_builder_t path = sb_append(base, FD(file));
    keypair_t *kp;
    IF_PROP(&e, keypair_load_public_path(&kp, &path) ){
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

    keypair_t *copy;
    PROP_GO(&e, keypair_copy(kp, &copy), fail);

    link_list_append(&kd->peers, &kp->link);
    link_list_append(&kd->all_keys, &copy->link);

    return e;

fail:
    keypair_free(&kp);

    return e;
}

derr_t keydir_new(
    const string_builder_t *root,
    const dstr_t user,
    keydir_i **out
){
    derr_t e = E_OK;
    *out = NULL;

    keydir_t *kd = DMALLOC_STRUCT_PTR(&e, kd);
    CHECK(&e);
    *kd = (keydir_t){
        .user = user,
        .iface = {
            // nice, mockable interface
            .mykey = kd_mykey,
            .peers = kd_peers,
            .add_key = kd_add_key,
            .delete_key = kd_delete_key,
            .free = kd_free,
            // sloppy, no-point-in-mocking interface
            .dirmgr = kd_dirmgr,
            .all_keys = kd_all_keys,
            .mailbox_synced = kd_mailbox_synced,
        },
        .imaildir_hooks = {
            .process_msg = imaildir_hooks_process_msg,
        },
    };
    kd->path = sb_append(root, FD(&kd->user));
    kd->mail_path = sb_append(&kd->path, FS("mail"));
    kd->key_path = sb_append(&kd->path, FS("keys"));

    string_builder_t fpr_path = sb_append(&kd->path, FS("fingerprints"));
    PROP_GO(&e, fpr_watcher_init( &kd->fpr_watcher, fpr_path), fail);

    // load or create my_keypair
    PROP_GO(&e, _load_or_gen_mykey(&kd->key_path, &kd->mykey), fail);
    // no need for keypair_cpy; kd->mykey itself can be in in kd->all_keys
    link_list_append(&kd->all_keys, &kd->mykey->link);

    // populate peers and all_keys
    PROP_GO(&e, for_each_file_in_dir(&kd->key_path, add_peer_key, kd), fail);

    // init the dirmgr
    PROP_GO(&e,
        dirmgr_init(&kd->dirmgr, kd->mail_path, &kd->imaildir_hooks),
    fail);

    *out = &kd->iface;

    return e;

fail:
    keydir_free(kd);
    return e;
}

// XXX: check e2e coverage, maybe add unit test
