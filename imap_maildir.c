#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>

#include "imap_maildir.h"
#include "logger.h"
#include "fileops.h"
#include "uv_util.h"
#include "maildir_name.h"
#include "imap_util.h"

#include "win_compat.h"

#define HOSTNAME_COMPONENT_MAX_LEN 32

// forward declarations
static derr_t conn_up_resp(maildir_i*, imap_resp_t*);
// static derr_t maildir_cmd(maildir_i*, imap_cmd_t*);
static bool conn_up_synced(maildir_i*);
static bool conn_up_selected(maildir_i*);
static derr_t conn_up_unselect(maildir_i *maildir);


// static derr_t maildir_resp_not_allowed(maildir_i* maildir, imap_resp_t* resp){
//     (void)maildir;
//     (void)resp;
//     derr_t e = E_OK;
//     ORIG(&e, E_INTERNAL, "response not allowed from a downwards connection");
// }
static derr_t maildir_cmd_not_allowed(maildir_i* maildir, imap_cmd_t* cmd){
    (void)maildir;
    (void)cmd;
    derr_t e = E_OK;
    ORIG(&e, E_INTERNAL, "command not allowed from an upwards connection");
}

// up_t is all the state we have for an upwards connection
typedef struct {
    imaildir_t *m;
    // the interfaced provided to us
    maildir_conn_up_i *conn;
    // the interface we provide
    maildir_i maildir;
    // this connection's state
    bool selected;
    bool synced;
    bool close_sent;
    // a tool for tracking the highestmodseq we have actually synced
    himodseq_calc_t hmsc;
    // a seq_set_builder_t
    seq_set_builder_t uids_to_download;
    // current tag
    size_t tag;
    link_t cbs;  // imap_cmd_cb_t->link
    link_t link;  // imaildir_t->access.ups
} up_t;
DEF_CONTAINER_OF(up_t, maildir, maildir_i);
DEF_CONTAINER_OF(up_t, link, link_t);

// dn_t is all the state we have for a downwards connection
typedef struct {
    /* TODO: support downwards connections
        The structures needed will be:
          - maildir_conn_dn_i
          - maildir_i
          - a view of the mailbox
          - some concept of callbacks for responding to commands
          ...
          - NOT a list of uncommitted changes; that list should be maildir-wide
    */
} dn_t;

static derr_t up_new(up_t **out, maildir_conn_up_i *conn, imaildir_t *m){
    derr_t e = E_OK;
    *out = NULL;

    up_t *up = malloc(sizeof(*up));
    if(!up) ORIG(&e, E_NOMEM, "nomem");
    *up = (up_t){
        .m = m,
        .conn = conn,
        .maildir = {
            .resp = conn_up_resp,
            .cmd = maildir_cmd_not_allowed,
            .synced = conn_up_synced,
            .selected = conn_up_selected,
            .unselect = conn_up_unselect,
        },
    };

    // start with the himodseqvalue in the persistent cache
    hmsc_prep(&up->hmsc, m->content.log->get_himodseq_up(m->content.log));

    // TODO: walk through the m->content.msgs looking for unfilled msg_base_t's
    seq_set_builder_prep(&up->uids_to_download);

    link_init(&up->cbs);
    link_init(&up->link);

    *out = up;

    return e;
};

// up_free is meant to be called right after imaildir_unregister_up()
static void up_free(up_t **up){
    if(*up == NULL) return;
    /* it's not allowed to remove the up_t from imaildir.access.ups here, due
       to race conditions in the cleanup sequence */

    // cancel all callbacks
    link_t *link;
    while((link = link_list_pop_first(&(*up)->cbs))){
        imap_cmd_cb_t *cb = CONTAINER_OF(link, imap_cmd_cb_t, link);
        cb->free(cb);
    }

    // free anything in the sequence_set_builder
    seq_set_builder_free(&(*up)->uids_to_download);

    // release the interface
    (*up)->conn->release((*up)->conn, E_OK);

    // free memory
    free(*up);
    *up = NULL;
}

/*
static derr_t fopen_by_uid(maildir_i *maildir, unsigned int uid,
        const char *mode, FILE **out){
    derr_t e = E_OK;

    acc_t *acc = CONTAINER_OF(maildir, acc_t, maildir);
    imaildir_t *m = acc->m;

    uv_rwlock_rdlock(&m->content.lock);

    // try to get the hashmap
    hash_elem_t *h = hashmap_getu(&m->content.msgs, uid);
    if(h == NULL){
        ORIG_GO(&e, E_PARAM, "no matching uid", done);
    }
    msg_base_t *base = CONTAINER_OF(h, msg_base_t, h);

    // where is the message located?
    string_builder_t dir = SUB(&m->path, base->subdir);
    string_builder_t path = sb_append(&dir, FD(&base->filename));

    PROP_GO(&e, fopen_path(&path, mode, out), done);

done:
    uv_rwlock_rdunlock(&m->content.lock);
    return e;
}
*/

// // check for "cur" and "new" and "tmp" subdirs, "true" means "all present"
// static derr_t ctn_check(const string_builder_t *path, bool *ret){
//     derr_t e = E_OK;
//     char *ctn[3] = {"cur", "tmp", "new"};
//     *ret = true;
//     for(size_t i = 0; i < 3; i++){
//         string_builder_t subdir_path = sb_append(path, FS(ctn[i]));
//         bool temp;
//         PROP(&e, dir_rw_access_path(&subdir_path, false, &temp) );
//         *ret &= temp;
//     }
//     return e;
// }

typedef struct {
    imaildir_t *m;
    subdir_type_e subdir;
} add_msg_arg_t;

// not safe to call after maildir_init due to race conditions
static derr_t add_msg_to_maildir(const string_builder_t *base,
        const dstr_t *name, bool is_dir, void *data){
    derr_t e = E_OK;
    (void)base;

    add_msg_arg_t *arg = data;

    // ignore directories
    if(is_dir) return e;

    // extract uid and metadata from filename
    unsigned int uid;
    size_t len;

    derr_t e2 = maildir_name_parse(name, NULL, &uid, &len, NULL, NULL);
    CATCH(e2, E_PARAM){
        // TODO: Don't ignore bad filenames; add them as "need to be sync'd"
        DROP_VAR(&e2);
        return e;
    }else PROP(&e, e2);

    // grab the metadata we loaded from the persistent cache
    jsw_anode_t *node = jsw_afind(&arg->m->content.msgs, &uid, NULL);
    if(node == NULL){
        // TODO: handle this better
        ORIG(&e, E_INTERNAL, "UID on file not in cache");
    }

    msg_base_t *msg_base = CONTAINER_OF(node, msg_base_t, node);

    if(msg_base->filled){
        // TODO: handle this better
        ORIG(&e, E_INTERNAL, "duplicate UID on file");
    }

    PROP(&e, msg_base_fill(msg_base, len, arg->subdir, name) );

    return e;
}

// not safe to call after maildir_init due to race conditions
static derr_t populate_msgs(imaildir_t *m){
    derr_t e = E_OK;

    // check /cur and /new
    subdir_type_e subdirs[] = {SUBDIR_CUR, SUBDIR_NEW};

    // add every file we have
    for(size_t i = 0; i < sizeof(subdirs)/sizeof(*subdirs); i++){
        subdir_type_e subdir = subdirs[i];
        string_builder_t subpath = SUB(&m->path, subdir);

        add_msg_arg_t arg = {.m=m, .subdir=subdir};

        PROP(&e, for_each_file_in_dir2(&subpath, add_msg_to_maildir, &arg) );
    }

    // detect uids that are unfilled
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, &m->content.msgs);
    while(node != NULL){
        msg_base_t *base = CONTAINER_OF(node, msg_base_t, node);
        if(base->filled == true){
            node = jsw_atnext(&trav);
            continue;
        }

        // advance the node, popping base->node from msgs
        node = jsw_pop_atnext(&trav);

        // place the base in m->content.msgs_empty
        jsw_ainsert(&m->content.msgs_empty, &base->node);
    }

    return e;
}

static derr_t imaildir_print_msgs(imaildir_t *m){
    derr_t e = E_OK;

    PROP(&e, PFMT("msgs:\n") );
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, &m->content.msgs);
    for(; node != NULL; node = jsw_atnext(&trav)){
        msg_base_t *base = CONTAINER_OF(node, msg_base_t, node);
        DSTR_VAR(buffer, 1024);
        PROP(&e, msg_base_write(base, &buffer) );
        PROP(&e, PFMT("    %x\n", FD(&buffer)) );
    }
    PROP(&e, PFMT("----\n") );

    PROP(&e, PFMT("unfilled:\n") );
    node = jsw_atfirst(&trav, &m->content.msgs_empty);
    for(; node != NULL; node = jsw_atnext(&trav)){
        msg_base_t *base = CONTAINER_OF(node, msg_base_t, node);
        DSTR_VAR(buffer, 1024);
        PROP(&e, msg_base_write(base, &buffer) );
        PROP(&e, PFMT("    %x\n", FD(&buffer)) );
    }
    PROP(&e, PFMT("----\n") );

    PROP(&e, PFMT("expunged:\n") );
    node = jsw_atfirst(&trav, &m->content.expunged);
    for(; node != NULL; node = jsw_atnext(&trav)){
        msg_expunge_t *expunge = CONTAINER_OF(node, msg_expunge_t, node);
        DSTR_VAR(buffer, 1024);
        PROP(&e, msg_expunge_write(expunge, &buffer) );
        PROP(&e, PFMT("    %x\n", FD(&buffer)) );
    }
    PROP(&e, PFMT("----\n") );

    return e;
}

// for maildir.content.msgs
static const void *msg_base_jsw_get(const jsw_anode_t *node){
    const msg_base_t *base = CONTAINER_OF(node, msg_base_t, node);
    return (void*)base;
}
static int msg_base_jsw_cmp_uid(const void *a, const void *b){
    const msg_base_t *basea = a;
    const msg_base_t *baseb = b;
    return JSW_NUM_CMP(basea->ref.uid, baseb->ref.uid);
}

// for maildir.content.expunged
static const void *msg_expunge_jsw_get(const jsw_anode_t *node){
    const msg_expunge_t *expunge = CONTAINER_OF(node, msg_expunge_t, node);
    return (void*)expunge;
}
static int msg_expunge_jsw_cmp_uid(const void *a, const void *b){
    const msg_expunge_t *expungea = a;
    const msg_expunge_t *expungeb = b;
    return JSW_NUM_CMP(expungea->uid, expungeb->uid);
}

// for maildir.content.mods
static const void *msg_mod_jsw_get(const jsw_anode_t *node){
    const msg_mod_t *mod = CONTAINER_OF(node, msg_mod_t, node);
    return (void*)mod;
}
static int msg_mod_jsw_cmp_modseq(const void *a, const void *b){
    const msg_mod_t *moda = a;
    const msg_mod_t *modb = b;
    return JSW_NUM_CMP(moda->modseq, modb->modseq);
}

derr_t imaildir_init(imaildir_t *m, string_builder_t path, const dstr_t *name,
        dirmgr_i *dirmgr, const keypair_t *keypair){
    derr_t e = E_OK;

    *m = (imaildir_t){
        .path = path,
        .name = name,
        .dirmgr = dirmgr,
        .keypair = keypair,
        // TODO: finish setting values
        // .uid_validity = ???
        // .mflags = ???
    };

    link_init(&m->content.unreconciled);
    link_init(&m->access.ups);

    // // check for cur/new/tmp folders, and assign /NOSELECT accordingly
    // bool ctn_present;
    // PROP(&e, ctn_check(&m->path, &ctn_present) );
    // m->mflags = (ie_mflags_t){
    //     .selectable=ctn_present ? IE_SELECTABLE_NONE : IE_SELECTABLE_NOSELECT,
    // };

    // initialize locks
    int ret = uv_rwlock_init(&m->content.lock);
    if(ret < 0){
        TRACE(&e, "uv_rwlock_init: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "error initializing rwlock");
    }

    ret = uv_mutex_init(&m->access.mutex);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing mutex",
                fail_content_lock);
    }

    // init msgs
    jsw_ainit(&m->content.msgs, msg_base_jsw_cmp_uid, msg_base_jsw_get);
    jsw_ainit(&m->content.msgs_empty, msg_base_jsw_cmp_uid, msg_base_jsw_get);

    // init expunged
    jsw_ainit(&m->content.expunged, msg_expunge_jsw_cmp_uid,
            msg_expunge_jsw_get);

    // init mods
    jsw_ainit(&m->content.mods, msg_mod_jsw_cmp_modseq, msg_mod_jsw_get);

    // any remaining failures must result in a call to imaildir_free()

    PROP_GO(&e, imaildir_log_open(&m->path, &m->content.msgs,
                &m->content.expunged, &m->content.mods, &m->content.log),
            fail_free);

    // populate messages by reading files
    PROP_GO(&e, populate_msgs(m), fail_free);

    PROP_GO(&e, imaildir_print_msgs(m), fail_free);

    return e;

fail_free:
    imaildir_free(m);
    return e;

fail_content_lock:
    uv_rwlock_destroy(&m->content.lock);
    return e;
}

// free must only be called if the maildir has no accessors
void imaildir_free(imaildir_t *m){
    if(!m) return;
    DROP_CMD(imaildir_print_msgs(m) );
    jsw_anode_t *node;
    // free all expunged
    while((node = jsw_apop(&m->content.expunged))){
        msg_expunge_t *expunge = CONTAINER_OF(node, msg_expunge_t, node);
        msg_expunge_free(&expunge);
    }

    // free all messages
    while((node = jsw_apop(&m->content.msgs))){
        msg_base_t *base = CONTAINER_OF(node, msg_base_t, node);
        msg_meta_free(&base->meta);
        msg_base_free(&base);
    }
    while((node = jsw_apop(&m->content.msgs_empty))){
        msg_base_t *base = CONTAINER_OF(node, msg_base_t, node);
        msg_meta_free(&base->meta);
        msg_base_free(&base);
    }

    // free any detached msg_meta's
    link_t *link;
    while((link = link_list_pop_first(&m->content.unreconciled))){
        // msg_update_t *update = CONTAINER_OF(link, msg_update_t, link);
        LOG_ERROR("FOUND UNRECONCILED UPDATES BUT DON'T KNOW HOW TO FREE THEM");
    }

    // none of the nodes in m->content.mods are still valid, so leave it alone

    // handle the case where imaildir_init failed in imaildir_log_open
    if(m->content.log){
        m->content.log->close(m->content.log);
    }

    uv_mutex_destroy(&m->access.mutex);
    uv_rwlock_destroy(&m->content.lock);
}

// this is for the himodseq that we serve to clients
static unsigned long imaildir_himodseq_dn(imaildir_t *m){
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atlast(&trav, &m->content.mods);
    if(node != NULL){
        msg_mod_t *mod = CONTAINER_OF(node, msg_mod_t, node);
        return mod->modseq;
    }

    // if the mailbox is empty, return 1
    return 1;
}

// this is for the himodseq when we sync from the server
static unsigned long imaildir_himodseq_up(imaildir_t *m){
    return m->content.log->get_himodseq_up(m->content.log);
}

// // content must be read-locked
// static derr_t acc_new(acc_t **out, imaildir_t *m, accessor_i *accessor){
//     derr_t e = E_OK;
//
//     acc_t *acc = malloc(sizeof(*acc));
//     if(acc == NULL) ORIG(&e, E_NOMEM, "no mem");
//     *acc = (acc_t){
//         .accessor = accessor,
//         .m = m,
//         .maildir = {
//             .update_flags = update_flags,
//             .new_msg = new_msg,
//             .expunge_msg = expunge_msg,
//             .fopen_by_uid = fopen_by_uid,
//             .reconcile_until = reconcile_until,
//         },
//         .reconciled = m->content.seq,
//     };
//
//     link_init(&acc->link);
//
//     *out = acc;
//     return e;
// }
//
// static void acc_free(acc_t **acc){
//     if(!*acc) return;
//     free(*acc);
//     *acc = NULL;
// }

// // for jsw_atree: get a msg_view_t from a node
// static const void *maildir_view_get(const jsw_anode_t *node){
//     const msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);
//     return (void*)view;
// }
// // for jsw_atree: compare two msg_view_t's by uid
// static int maildir_view_cmp(const void *a, const void *b){
//     const msg_view_t *viewa = a;
//     const msg_view_t *viewb = b;
//     return viewa->base->uid > viewb->base->uid;
// }

// // build a view of the maildir
// static derr_t imaildir_build_dirview(imaildir_t *m, jsw_atree_t *dirview_out){
//     derr_t e = E_OK;
//
//     // lock content while we copy it
//     uv_rwlock_rdlock(&m->content.lock);
//
//     // initialize the output view
//     jsw_ainit(dirview_out, maildir_view_cmp, maildir_view_get);
//
//     // make a view of every message
//     hashmap_iter_t i;
//     for(i = hashmap_first(&m->content.msgs); i.current; hashmap_next(&i)){
//         // build a message view of this message
//         msg_base_t *base = CONTAINER_OF(i.current, msg_base_t, h);
//         msg_view_t *view;
//         PROP_GO(&e, msg_view_new(&view, base), fail);
//         // add message view to the dir view
//         jsw_ainsert(dirview_out, &view->node);
//     }
//
//     uv_rwlock_rdunlock(&m->content.lock);
//
//     // pass the maildir_i
//     *maildir_out = &acc->maildir;
//
//     return e;
//
//     jsw_anode_t *node;
// fail:
//     // free all of the references
//     while((node = jsw_apop(dirview_out)) != NULL){
//         msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);
//         msg_view_free(&view);
//     }
// // fail_content_lock:
//     uv_rwlock_wrunlock(&m->content.lock);
//     return e;
// }

static ie_dstr_t *write_tag_up(derr_t *e, size_t tag){
    if(is_error(*e)) goto fail;

    DSTR_VAR(buf, 32);
    PROP_GO(e, FMT(&buf, "maildir_up%x", FU(tag)), fail);

    return ie_dstr_new(e, &buf, KEEP_RAW);

fail:
    return NULL;
}

// read the serial of a tag we issued
static derr_t read_tag_up(ie_dstr_t *tag, size_t *tag_out, bool *was_ours){
    derr_t e = E_OK;
    *tag_out = 0;

    DSTR_STATIC(maildir_up, "maildir_up");
    dstr_t ignore_substr = dstr_sub(&tag->dstr, 0, maildir_up.len);
    // make sure it starts with "maildir_up"
    if(dstr_cmp(&ignore_substr, &maildir_up) != 0){
        *was_ours = false;
        return e;
    }

    *was_ours = true;

    dstr_t number_substr = dstr_sub(&tag->dstr, maildir_up.len, tag->dstr.len);
    PROP(&e, dstr_tosize(&number_substr, tag_out, 10) );

    return e;
}

typedef struct {
    up_t *up;
    imap_cmd_cb_t cb;
} up_cb_t;
DEF_CONTAINER_OF(up_cb_t, cb, imap_cmd_cb_t);

// up_cb_free is an imap_cmd_cb_free_f
static void up_cb_free(imap_cmd_cb_t *cb){
    if(!cb) return;
    up_cb_t *up_cb = CONTAINER_OF(cb, up_cb_t, cb);
    free(up_cb);
}

static up_cb_t *up_cb_new(derr_t *e, up_t *up, size_t tag,
        imap_cmd_cb_call_f call, imap_cmd_t *cmd){
    if(is_error(*e)) goto fail;

    up_cb_t *up_cb = malloc(sizeof(*up_cb));
    if(!up_cb) goto fail;
    *up_cb = (up_cb_t){
        .up = up,
    };

    imap_cmd_cb_prep(&up_cb->cb, tag, call, up_cb_free);

    return up_cb;

fail:
    imap_cmd_free(cmd);
    return NULL;
}

// send a command and store its callback
static void send_cmd(up_t *up, imap_cmd_t *cmd, up_cb_t *up_cb){
    // store the callback
    link_list_append(&up->cbs, &up_cb->cb.link);

    // send the command through the conn_up
    up->conn->cmd(up->conn, cmd);
}

// close_done is an imap_cmd_cb_call_f
static derr_t close_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    up_cb_t *up_cb = CONTAINER_OF(cb, up_cb_t, cb);
    up_t *up = up_cb->up;

    if(st_resp->status != IE_ST_OK){
        ORIG(&e, E_PARAM, "close failed\n");
    }

    // signal that we are done with this connection
    up->conn->unselected(up->conn);

    /* TODO: we should be changing to a different primary connection now
       instead of waiting until somebody unregisters... */

    return e;
}

static derr_t send_close(up_t *up){
    derr_t e = E_OK;

    // issue a CLOSE command
    imap_cmd_arg_t arg = {0};
    size_t tag = ++up->tag;
    ie_dstr_t *tag_str = write_tag_up(&e, tag);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag_str, IMAP_CMD_CLOSE, arg);

    // build the callback
    up_cb_t *up_cb = up_cb_new(&e, up, tag, close_done, cmd);

    CHECK(&e);

    up->close_sent = true;
    send_cmd(up, cmd, up_cb);

    return e;
}

// after every command, evaluate our internal state to decide the next one
static derr_t next_cmd(up_t *up);

static derr_t imaildir_new_msg(imaildir_t *m, unsigned int uid,
        msg_flags_t flags, msg_base_t **out){
    derr_t e = E_OK;
    *out = NULL;

    uv_rwlock_wrlock(&m->content.lock);

    // get the last highets modseq
    unsigned long himodseq_dn = imaildir_himodseq_dn(m);

    // create a new meta
    msg_meta_t *meta;
    PROP(&e, msg_meta_new(&meta, flags, himodseq_dn + 1) );

    // don't know the internaldate yet
    imap_time_t intdate = {0};

    // create a new base
    msg_base_t *base;
    PROP_GO(&e, msg_base_new(&base, uid, intdate, meta), fail_meta);

    // add message to log
    maildir_log_i *log = m->content.log;
    PROP_GO(&e, log->update_msg(log, uid, base), fail_base);

    // insert meta into mods
    jsw_ainsert(&m->content.mods, &meta->mod.node);

    // insert base into msgs_empty
    jsw_ainsert(&m->content.msgs_empty, &base->node);

    uv_rwlock_wrunlock(&m->content.lock);

    *out = base;

    return e;

fail_base:
    msg_base_free(&base);
fail_meta:
    msg_meta_free(&meta);
    uv_rwlock_wrunlock(&m->content.lock);
    return e;
}

static derr_t imaildir_decrypt(imaildir_t *m, const dstr_t *cipher,
        const string_builder_t *path, size_t *len){
    derr_t e = E_OK;

    // create the file
    FILE *f;
    PROP(&e, fopen_path(path, "w", &f) );

    // TODO: fix decrypter_t API to support const input strings
    // copy the content, just to work around the stream-only API of decrypter_t
    dstr_t copy;
    PROP_GO(&e, dstr_new(&copy, cipher->len), cu_file);
    PROP_GO(&e, dstr_copy(cipher, &copy), cu_copy);

    dstr_t plain;
    PROP_GO(&e, dstr_new(&plain, cipher->len), cu_copy);

    // TODO: use key_tool_decrypt instead, it is more robust

    // create the decrypter
    decrypter_t dc;
    PROP_GO(&e, decrypter_new(&dc), cu_plain);
    PROP_GO(&e, decrypter_start(&dc, m->keypair, NULL, NULL), cu_dc);

    // decrypt the message
    PROP_GO(&e, decrypter_update(&dc, &copy, &plain), cu_dc);
    PROP_GO(&e, decrypter_finish(&dc, &plain), cu_dc);

    if(len) *len = plain.len;

    // write the file
    PROP_GO(&e, dstr_fwrite(f, &plain), cu_dc);

cu_dc:
    decrypter_free(&dc);

cu_plain:
    dstr_free(&plain);

cu_copy:
    dstr_free(&copy);

    int ret;
cu_file:
    ret = fclose(f);
    // check for closing error
    if(ret != 0 && !is_error(e)){
        TRACE(&e, "fclose(%x): %x\n", FSB(path, &DSTR_LIT("/")),
                FE(&errno));
        ORIG(&e, E_OS, "failed to write file");
    }

    return e;
}

static derr_t imaildir_handle_static_fetch_attr(imaildir_t *m,
        msg_base_t *base, const ie_fetch_resp_t *fetch){
    derr_t e = E_OK;

    if(!fetch->content) return e;

    // we shouldn't have anything after the message is filled
    if(base->filled){
        LOG_ERROR("dropping unexpected static fetch attributes\n");
        return e;
    }

    // we always fill all the static attributes in one request
    if(!fetch->intdate.year){
        ORIG(&e, E_RESPONSE, "missing INTERNALDATE response");
    }

    uv_rwlock_wrlock(&m->content.lock);

    base->ref.internaldate = fetch->intdate;

    /* save the internaldate in the database before saving the file content,
       in case we crash */
    maildir_log_i *log = m->content.log;
    PROP_GO(&e, log->update_msg(log, base->ref.uid, base), fail_lock);

    size_t tmp_id = m->content.tmp_count++;

    // decrypt without lock
    // TODO: this is not safe!  A dn_t could delete base.
    uv_rwlock_wrunlock(&m->content.lock);

    if(!fetch->content){
        ORIG(&e, E_RESPONSE, "missing RFC822 content response");
    }

    // figure the temporary file name
    DSTR_VAR(tmp_name, 32);
    NOFAIL(&e, E_FIXEDSIZE, FMT(&tmp_name, "%x", FU(tmp_id)) );

    // build the path
    string_builder_t tmp_dir = TMP(&m->path);
    string_builder_t tmp_path = sb_append(&tmp_dir, FD(&tmp_name));

    // do the decryption
    size_t len = 0;
    PROP(&e, imaildir_decrypt(m, &fetch->content->dstr, &tmp_path, &len) );
    base->ref.length = len;

    // get hostname
    DSTR_VAR(hostname, 256);
    gethostname(hostname.data, hostname.size);
    hostname.len = strnlen(hostname.data, HOSTNAME_COMPONENT_MAX_LEN);

    // get epochtime
    time_t tloc;
    time_t tret = time(&tloc);
    if(tret < 0){
        // if this fails... just use zero
        tloc = ((time_t) 0);
    }

    unsigned long epoch = (unsigned long)tloc;

    // figure the new path
    DSTR_VAR(cur_name, 255);
    PROP(&e, maildir_name_write(&cur_name, epoch, base->ref.uid, len,
                &hostname, NULL) );
    string_builder_t cur_dir = CUR(&m->path);
    string_builder_t cur_path = sb_append(&cur_dir, FD(&cur_name));

    // move the file into place
    PROP(&e, rename_path(&tmp_path, &cur_path) );

    // fill base
    PROP(&e, msg_base_fill(base, len, SUBDIR_CUR, &cur_name) );

    // move base to msgs
    jsw_anode_t *node = jsw_aerase(&m->content.msgs_empty, base);
    if(node != &base->node){
        ORIG(&e, E_INTERNAL, "removed wrong node from tree");
    }
    jsw_ainsert(&m->content.msgs, &base->node);

    /* TODO: register the file as downloaded in the database, so we can detect
             deletions from MUAs.  Probably this is not super important unless
             we also support tracking file updates. */

    return e;

fail_lock:
    uv_rwlock_wrunlock(&m->content.lock);
    return e;
}

static derr_t fetch_resp(up_t *up, const ie_fetch_resp_t *fetch){
    derr_t e = E_OK;

    // grab UID
    if(!fetch->uid){
        LOG_ERROR("detected fetch without UID, skipping\n");
        return e;
    }

    // do we already have this UID?
    uv_rwlock_rdlock(&up->m->content.lock);
    msg_base_t *base = NULL;
    // check filled messages
    jsw_anode_t *node = jsw_afind(&up->m->content.msgs, &fetch->uid, NULL);
    if(!node){
        // check empty messages
        node = jsw_afind(&up->m->content.msgs_empty, &fetch->uid, NULL);
    }
    if(node){
        base = CONTAINER_OF(node, msg_base_t, node);
    }
    uv_rwlock_rdunlock(&up->m->content.lock);

    if(!base){
        // new UID
        msg_flags_t flags = msg_flags_from_fetch_flags(fetch->flags);
        PROP(&e, imaildir_new_msg(up->m, fetch->uid, flags, &base) );

        if(!fetch->content){
            PROP(&e, seq_set_builder_add_val(&up->uids_to_download,
                        fetch->uid) );
        }
    }else{
        // existing UID
        // TODO: update flags
        LOG_ERROR("need to update flags");

        if(fetch->content){
            LOG_ERROR("dropping unexpected RFC822 content\n");
        }
    }

    PROP(&e, imaildir_handle_static_fetch_attr(up->m, base, fetch) );

    // did we see a MODSEQ value?
    if(fetch->modseq > 0){
        hmsc_saw_fetch(&up->hmsc, fetch->modseq);
    }

    return e;
}

// fetch_done is an imap_cmd_cb_call_f
static derr_t fetch_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    up_cb_t *up_cb = CONTAINER_OF(cb, up_cb_t, cb);
    up_t *up = up_cb->up;

    if(st_resp->status != IE_ST_OK){
        ORIG(&e, E_PARAM, "fetch failed\n");
    }

    PROP(&e, next_cmd(up) );

    return e;
}

static derr_t imaildir_get_unfilled(imaildir_t *m, seq_set_builder_t *ssb){
    derr_t e = E_OK;

    uv_rwlock_rdlock(&m->content.lock);

    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, &m->content.msgs_empty);
    for(; node != NULL; node = jsw_atnext(&trav)){
        msg_base_t *base = CONTAINER_OF(node, msg_base_t, node);
        PROP_GO(&e, seq_set_builder_add_val(ssb, base->ref.uid), cu);
    }

cu:
    uv_rwlock_rdunlock(&m->content.lock);
    return e;
}

static derr_t send_fetch(up_t *up){
    derr_t e = E_OK;

    // issue a UID FETCH command
    bool uid_mode = true;
    // fetch all the messages we need to download
    ie_seq_set_t *uidseq = seq_set_builder_extract(&e, &up->uids_to_download);
    // fetch UID, FLAGS, RFC822 content, INTERNALDATE, and MODSEQ
    ie_fetch_attrs_t *attr = ie_fetch_attrs_new(&e);
    ie_fetch_attrs_add_simple(&e, attr, IE_FETCH_ATTR_UID);
    ie_fetch_attrs_add_simple(&e, attr, IE_FETCH_ATTR_FLAGS);
    ie_fetch_attrs_add_simple(&e, attr, IE_FETCH_ATTR_RFC822);
    ie_fetch_attrs_add_simple(&e, attr, IE_FETCH_ATTR_INTDATE);
    ie_fetch_attrs_add_simple(&e, attr, IE_FETCH_ATTR_MODSEQ);

    // build fetch command
    ie_fetch_cmd_t *fetch = ie_fetch_cmd_new(&e, uid_mode, uidseq, attr, NULL);
    imap_cmd_arg_t arg = {.fetch=fetch};

    size_t tag = ++up->tag;
    ie_dstr_t *tag_str = write_tag_up(&e, tag);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag_str, IMAP_CMD_FETCH, arg);

    // build the callback
    up_cb_t *up_cb = up_cb_new(&e, up, tag, fetch_done, cmd);

    CHECK(&e);

    send_cmd(up, cmd, up_cb);

    return e;
}

// initial_search_done is an imap_cmd_cb_call_f
static derr_t initial_search_done(imap_cmd_cb_t *cb,
        const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    up_cb_t *up_cb = CONTAINER_OF(cb, up_cb_t, cb);
    up_t *up = up_cb->up;

    if(st_resp->status != IE_ST_OK){
        ORIG(&e, E_PARAM, "search failed\n");
    }

    if(!seq_set_builder_isempty(&up->uids_to_download)){
        /* skip next_cmd, since we can't store the HIGESTMODSEQ until after the
           first complete fetch */
        PROP(&e, send_fetch(up) );
    }else{
        PROP(&e, next_cmd(up) );
    }

    return e;
}

static derr_t search_resp(up_t *up, const ie_search_resp_t *search){
    derr_t e = E_OK;

    // send a UID fetch for each uid
    for(const ie_nums_t *uid = search->nums; uid != NULL; uid = uid->next){
        /* TODO: Check if we've already downloaded this UID.  This could happen
                 if a large initial download failed halfway through. */

        // add this UID to our list of existing UIDs
        PROP(&e, seq_set_builder_add_val(&up->uids_to_download, uid->num) );
    }

    return e;
}

static derr_t send_initial_search(up_t *up){
    derr_t e = E_OK;

    // issue a `UID SEARCH UID 1:*` command to find all existing messages
    bool uid_mode = true;
    ie_dstr_t *charset = NULL;
    // "1" is the first message, "0" represents "*" which is the last message
    ie_seq_set_t *range = ie_seq_set_new(&e, 1, 0);
    ie_search_key_t *search_key = ie_search_seq_set(&e, IE_SEARCH_UID, range);
    imap_cmd_arg_t arg = {
        .search=ie_search_cmd_new(&e, uid_mode, charset, search_key)
    };

    size_t tag = ++up->tag;
    ie_dstr_t *tag_str = write_tag_up(&e, tag);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag_str, IMAP_CMD_SEARCH, arg);

    // build the callback
    up_cb_t *up_cb = up_cb_new(&e, up, tag, initial_search_done, cmd);

    CHECK(&e);

    send_cmd(up, cmd, up_cb);

    return e;
}

// after every command, evaluate our internal state to decide the next one
static derr_t next_cmd(up_t *up){
    derr_t e = E_OK;

    // do we need to cache a newer, fresher modseq value?
    if(hmsc_step(&up->hmsc)){
        maildir_log_i *log = up->m->content.log;
        PROP(&e, log->set_himodseq_up(log, hmsc_now(&up->hmsc)) )
    }

    // never send anything more after a close
    if(up->close_sent) return e;

    /* Are we synchronized?  We are synchronized when:
         - hmsc_now() is nonzero (zero means SELECT (QRESYNC ...) failed)
         - there are no UIDs that we need to download */
    if(!hmsc_now(&up->hmsc)){
        /* zero himodseq means means SELECT (QRESYNC ...) failed, so request
           all the flags and UIDs explicitly */
        PROP(&e, send_initial_search(up) );
    }else if(!seq_set_builder_isempty(&up->uids_to_download)){
        // there are UID's we need to download
        PROP(&e, send_fetch(up) );
    }else{
        // we are synchronized!  Is it our first time?
        if(!up->synced){
            up->synced = true;
            // send the signal to all the conn_up's
            up_t *up_to_signal;
            LINK_FOR_EACH(up_to_signal, &up->m->access.ups, up_t, link){
                up_to_signal->conn->synced(up_to_signal->conn);
            }
        }

        // TODO: start IDLE here, when that's actually supported

    }

    return e;
}

// select_done is an imap_cmd_cb_call_f
static derr_t select_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    up_cb_t *up_cb = CONTAINER_OF(cb, up_cb_t, cb);
    up_t *up = up_cb->up;

    if(st_resp->status != IE_ST_OK){
        ORIG(&e, E_PARAM, "select failed\n");
    }

    /* Add imaildir_t's unfilled UIDs to uids_to_download.  This doesn't have
       to go here precisely, but it does have to happen *after* an up_t becomes
       the primary up_t, and it has to happen before the first next_cmd(), and
       it has to happen exactly once, so this is a pretty good spot. */
    PROP(&e, imaildir_get_unfilled(up->m, &up->uids_to_download) );

    /* if this is a first-time sync, we have to delay next_cmd(), which will
       try to save the HIMODSEQ */
    maildir_log_i *log = up->m->content.log;
    if(!log->get_himodseq_up(log)){
        PROP(&e, send_initial_search(up) );
    }else{
        PROP(&e, next_cmd(up) );
    }

    return e;
}

static derr_t make_select(up_t *up, imap_cmd_t **cmd_out, up_cb_t **cb_out){
    derr_t e = E_OK;

    *cmd_out = NULL;
    *cb_out = NULL;

    // use QRESYNC with select if we have a valid UIDVALIDITY and HIGHESTMODSEQ
    ie_select_params_t *params = NULL;
    unsigned int uidvld = up->m->content.log->get_uidvld(up->m->content.log);
    unsigned long our_himodseq = imaildir_himodseq_up(up->m);
    if(uidvld && our_himodseq){
        ie_select_param_arg_t params_arg = { .qresync = {
            .uidvld = uidvld,
            .last_modseq = our_himodseq,
        } };
        params = ie_select_params_new(&e, IE_SELECT_PARAM_QRESYNC, params_arg);
    }

    // issue a SELECT command
    ie_dstr_t *name = ie_dstr_new(&e, up->m->name, KEEP_RAW);
    ie_mailbox_t *mailbox = ie_mailbox_new_noninbox(&e, name);
    ie_select_cmd_t *select = ie_select_cmd_new(&e, mailbox, params);
    imap_cmd_arg_t arg = { .select=select, };

    size_t tag = ++up->tag;
    ie_dstr_t *tag_str = write_tag_up(&e, tag);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag_str, IMAP_CMD_SELECT, arg);

    // build the callback
    up_cb_t *up_cb = up_cb_new(&e, up, tag, select_done, cmd);

    CHECK(&e);

    *cmd_out = cmd;
    *cb_out = up_cb;

    return e;
}

derr_t imaildir_register_up(imaildir_t *m, maildir_conn_up_i *conn_up,
        maildir_i **maildir_out){
    derr_t e = E_OK;

    // allocate a new up_t
    up_t *up;
    PROP(&e, up_new(&up, conn_up, m) );

    /* to keep our state machine simple, we pre-allocate an initial command,
       so that race conditions and weirdities don't arise if we become the
       primary conn_up and then we fail to allocate a command */
    imap_cmd_t *cmd;
    up_cb_t *up_cb;
    PROP_GO(&e, make_select(up, &cmd, &up_cb), fail_up);

    // everything's ready

    *maildir_out = &up->maildir;

    // add the up_t to the maildir, and check if it the new primary conn_up
    uv_mutex_lock(&m->access.mutex);
    if(m->access.failed){
        ORIG_GO(&e, E_DEAD, "maildir in failed state", fail_dead);
    }
    bool is_primary = link_list_isempty(&m->access.ups);
    link_list_append(&m->access.ups, &up->link);
    m->access.naccessors++;
    uv_mutex_unlock(&m->access.mutex);

    if(!is_primary){
        // turns out we don't need cmd or up_cb
        imap_cmd_free(cmd);
        up_cb->cb.free(&up_cb->cb);
        return e;
    }

    send_cmd(up, cmd, up_cb);

    // treat the connection state as "selected", even though we just sent it
    up->selected = true;

    return e;

fail_dead:
    uv_mutex_unlock(&m->access.mutex);
fail_up:
    up_free(&up);
    return e;
}

void imaildir_unregister_up(maildir_i *maildir, maildir_conn_up_i *conn){
    // conn argument is just for type safety
    (void)conn;
    up_t *up = CONTAINER_OF(maildir, up_t, maildir);
    imaildir_t *m = up->m;

    uv_mutex_lock(&m->access.mutex);

    /* In closing state, the list of accessors is not edited here.  This
       ensures that the iteration through the accessors list during
       imaildir_fail() is always safe. */
    if(!m->access.failed){
        link_remove(&up->link);
    }

    up_free(&up);

    bool all_unregistered = (--m->access.naccessors == 0);

    /* done with our own thread safety, the race between all_unregistered and
       imaildir_register must be be resolved externally if we want it to be
       safe to call imaildir_free() inside an all_unregistered() callback */
    uv_mutex_unlock(&m->access.mutex);

    if(all_unregistered){
        m->dirmgr->all_unregistered(m->dirmgr);
    }
}

static void imaildir_fail(imaildir_t *m, derr_t error){
    uv_mutex_lock(&m->access.mutex);
    bool do_fail = !m->access.failed;
    m->access.failed = true;
    uv_mutex_unlock(&m->access.mutex);

    if(!do_fail) goto done;

    link_t *link;
    while((link = link_list_pop_first(&m->access.ups)) != NULL){
        up_t *up = CONTAINER_OF(link, up_t, link);
        // if there was an error, share it with all of the accessors.
        up->conn->release(up->conn, BROADCAST(error));
    }

done:
    // free the error
    DROP_VAR(&error);
}

// useful if an open maildir needs to be deleted
void imaildir_forceclose(imaildir_t *m){
    imaildir_fail(m, E_OK);
}

static derr_t delete_one_msg(const string_builder_t *base, const dstr_t *name,
        bool is_dir, void *data){
    derr_t e = E_OK;
    (void)data;

    // ignore directories
    if(is_dir) return e;

    string_builder_t path = sb_append(base, FD(name));

    PROP(&e, remove_path(&path) );

    return e;
}

// delete all the messages we have, like in case of UIDVALIDITY change
static derr_t delete_all_msgs(imaildir_t *m){
    derr_t e = E_OK;

    // check /cur and /new
    subdir_type_e subdirs[] = {SUBDIR_CUR, SUBDIR_NEW, SUBDIR_TMP};

    for(size_t i = 0; i < sizeof(subdirs)/sizeof(*subdirs); i++){
        subdir_type_e subdir = subdirs[i];
        string_builder_t subpath = SUB(&m->path, subdir);

        PROP(&e, for_each_file_in_dir2(&subpath, delete_one_msg, NULL) );
    }

    return e;
}

static derr_t check_uidvld(up_t *up, unsigned int uidvld){
    derr_t e = E_OK;

    maildir_log_i *log = up->m->content.log;
    unsigned int old_uidvld = log->get_uidvld(log);

    if(old_uidvld != uidvld){

        // TODO: puke if we have any active connections downwards

        // if old_uidvld is nonzero, this really is a change, not a first-time
        if(old_uidvld){
            LOG_ERROR("detected change in UIDVALIDITY, dropping cache\n");
        }else{
            LOG_ERROR("detected first-time download\n");
        }
        PROP(&e, log->drop(log) );
        PROP(&e, delete_all_msgs(up->m) );

        // set the new uidvld and reset the himodseq
        PROP(&e, log->set_uidvld(log, uidvld) );
        PROP(&e, log->set_himodseq_up(log, 0) );
    }

    return e;
}

// handle untagged OK responses separately from other status type responses
static derr_t untagged_ok(up_t *up, const ie_st_code_t *code,
        const dstr_t *text){
    derr_t e = E_OK;

    // Handle responses where the status code is what defines the behavior
    if(code != NULL){
        switch(code->type){
            case IE_ST_CODE_READ_ONLY:
                ORIG(&e, E_INTERNAL, "unable to handle READ only boxes");
                break;

            case IE_ST_CODE_READ_WRITE:
                // nothing special required
                break;

            case IE_ST_CODE_UIDNEXT:
                // nothing special required, we will use extensions instead
                break;

            case IE_ST_CODE_UIDVLD:
                PROP(&e, check_uidvld(up, code->arg.uidvld) );
                break;

            case IE_ST_CODE_PERMFLAGS:
                // TODO: check that these look sane
                break;

            case IE_ST_CODE_HIMODSEQ:
                // tell our himodseq calculator what we saw
                hmsc_saw_ok_code(&up->hmsc, code->arg.himodseq);
                break;

            case IE_ST_CODE_UNSEEN:
                // we can ignore this, since we use himodseq
                break;

            case IE_ST_CODE_NOMODSEQ:
                ORIG(&e, E_RESPONSE,
                        "server mailbox does not support modseq numbers");
                break;


            case IE_ST_CODE_ALERT:
            case IE_ST_CODE_PARSE:
            case IE_ST_CODE_TRYCREATE:
            case IE_ST_CODE_CAPA:
            case IE_ST_CODE_ATOM:
            // UIDPLUS extension
            case IE_ST_CODE_UIDNOSTICK:
            case IE_ST_CODE_APPENDUID:
            case IE_ST_CODE_COPYUID:
            // CONDSTORE extension
            case IE_ST_CODE_MODIFIED:
            // QRESYNC extension
            case IE_ST_CODE_CLOSED:
                (void)text;
                ORIG(&e, E_INTERNAL, "code not supported\n");
                break;
        }
    }

    return e;
}

static derr_t tagged_status_type(up_t *up, const ie_st_resp_t *st){
    derr_t e = E_OK;

    // read the tag
    size_t tag_found;
    bool was_ours;
    PROP(&e, read_tag_up(st->tag, &tag_found, &was_ours) );
    if(!was_ours){
        ORIG(&e, E_INTERNAL, "tag not ours");
    }

    // peek at the first command we need a response to
    link_t *link = up->cbs.next;
    if(link == NULL){
        TRACE(&e, "got tag %x with no commands in flight\n",
                FD(&st->tag->dstr));
        ORIG(&e, E_RESPONSE, "bad status type response");
    }

    // make sure the tag matches
    imap_cmd_cb_t *cb = CONTAINER_OF(link, imap_cmd_cb_t, link);
    if(cb->tag != tag_found){
        TRACE(&e, "got tag %x but expected %x\n",
                FU(tag_found), FU(cb->tag));
        ORIG(&e, E_RESPONSE, "bad status type response");
    }

    // do the callback
    link_remove(link);
    PROP_GO(&e, cb->call(cb, st), cu_cb);

cu_cb:
    cb->free(cb);

    return e;
}

static derr_t untagged_status_type(up_t *up, const ie_st_resp_t *st){
    derr_t e = E_OK;
    switch(st->status){
        case IE_ST_OK:
            // informational message
            PROP(&e, untagged_ok(up, st->code, &st->text->dstr) );
            break;
        case IE_ST_NO:
            // a warning about a command
            // TODO: handle this
            TRACE(&e, "unhandled * NO status message\n");
            ORIG(&e, E_INTERNAL, "unhandled message");
            break;
        case IE_ST_BAD:
            // an error not from a command, or not sure from which command
            // TODO: handle this
            TRACE(&e, "unhandled * BAD status message\n");
            ORIG(&e, E_INTERNAL, "unhandled message");
            break;
        case IE_ST_PREAUTH:
            // only allowed as a greeting
            // TODO: handle this
            TRACE(&e, "unhandled * PREAUTH status message\n");
            ORIG(&e, E_INTERNAL, "unhandled message");
            break;
        case IE_ST_BYE:
            // we are logging out or server is shutting down.
            // TODO: handle this
            TRACE(&e, "unhandled * BYE status message\n");
            ORIG(&e, E_INTERNAL, "unhandled message");
            break;
        default:
            TRACE(&e, "invalid status of unknown type %x\n", FU(st->status));
            ORIG(&e, E_INTERNAL, "bad imap parse");
    }

    return e;
}

// we either need to consume the resp or free it
static derr_t conn_up_resp(maildir_i *maildir, imap_resp_t *resp){
    derr_t e = E_OK;

    up_t *up = CONTAINER_OF(maildir, up_t, maildir);

    const imap_resp_arg_t *arg = &resp->arg;

    switch(resp->type){
        case IMAP_RESP_STATUS_TYPE:
            // tagged responses are handled by callbacks
            if(arg->status_type->tag){
                PROP_GO(&e, tagged_status_type(up, arg->status_type),
                        cu_resp);
            }else{
                PROP_GO(&e, untagged_status_type(up, arg->status_type),
                        cu_resp);
            }
            break;

        case IMAP_RESP_FETCH:
            PROP_GO(&e, fetch_resp(up, arg->fetch), cu_resp);
            break;

        case IMAP_RESP_SEARCH:
            PROP_GO(&e, search_resp(up, arg->search), cu_resp);
            break;

        case IMAP_RESP_EXISTS:
            // TODO: possibly handle this?
            break;
        case IMAP_RESP_RECENT:
            // TODO: possibly handle this?
            LOG_ERROR("IGNORING RECENT RESPONSE\n");
            break;
        case IMAP_RESP_FLAGS:
            // TODO: possibly handle this?
            break;

        case IMAP_RESP_STATUS:
        case IMAP_RESP_EXPUNGE:
        case IMAP_RESP_ENABLED:
        case IMAP_RESP_VANISHED:
            ORIG_GO(&e, E_INTERNAL, "unhandled responses", cu_resp);

        case IMAP_RESP_CAPA:
        case IMAP_RESP_LIST:
        case IMAP_RESP_LSUB:
            ORIG_GO(&e, E_INTERNAL, "Invalid responses", cu_resp);
    }

cu_resp:
    imap_resp_free(resp);

    return e;
}

// returned value is based on the entire maildir
bool conn_up_synced(maildir_i *maildir){

    up_t *up = CONTAINER_OF(maildir, up_t, maildir);
    imaildir_t *m = up->m;

    bool synced = false;

    uv_mutex_lock(&m->access.mutex);
    if(!link_list_isempty(&m->access.ups)){
        link_t *link = m->access.ups.next;
        up_t *primary_up = CONTAINER_OF(link, up_t, link);
        synced = primary_up->synced;
    }
    uv_mutex_unlock(&m->access.mutex);

    return synced;
}

// returned value is based on the entire maildir
// returned value is based on the entire maildir
bool conn_up_selected(maildir_i *maildir){

    up_t *up = CONTAINER_OF(maildir, up_t, maildir);

    // TODO: review the thread safety of this strategy.

    return up->selected;
}

static derr_t conn_up_unselect(maildir_i *maildir){
    derr_t e = E_OK;

    up_t *up = CONTAINER_OF(maildir, up_t, maildir);

    if(!up->selected){
        // signal that it's already done
        up->conn->unselected(up->conn);
        return e;
    }

    // otherwise, send the close
    PROP(&e, send_close(up) );

    return e;
}
