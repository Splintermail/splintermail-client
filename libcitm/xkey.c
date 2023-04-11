#include "libcitm/libcitm.h"

const keypair_t *key_next(key_iter_t *it){
    if(it->next == it->head) return NULL;
    link_t *link = it->next;
    it->next = it->next->next;
    return CONTAINER_OF(link, keypair_t, link);
}

imap_cmd_t *xkeyadd_cmd(derr_t *e, ie_dstr_t *tag, const keypair_t *kp){
    if(is_error(*e)) goto fail;

    // get the pem-encoded key to send
    DSTR_VAR(pem, 2048);
    PROP_GO(e, keypair_get_public_pem(kp, &pem), fail);

    // issue an XKEYADD command
    ie_dstr_t *ie_pem = ie_dstr_new(e, &pem, KEEP_RAW);
    imap_cmd_arg_t arg = { .xkeyadd = ie_pem };
    imap_cmd_t *cmd = imap_cmd_new(e, tag, IMAP_CMD_XKEYADD, arg);

    return cmd;

fail:
    ie_dstr_free(tag);
    return NULL;
}

// builder API for ie_dstr_t's from binary fingerprints
static ie_dstr_t *build_fpr(derr_t *e, const dstr_t *bin_fpr){
    if(is_error(*e)) return NULL;
    DSTR_VAR(hex_fpr, 128);
    IF_PROP(e, bin2hex(bin_fpr, &hex_fpr) ){ return NULL; }
    return ie_dstr_new(e, &hex_fpr, KEEP_RAW);
}

imap_cmd_t *xkeysync_cmd(derr_t *e, ie_dstr_t *tag, keydir_i *kd){
    if(is_error(*e)){
        ie_dstr_free(tag);
        return NULL;
    }

    // make a list of fingerprints we expect the server to have
    ie_dstr_t *fprs = build_fpr(e, kd->mykey(kd)->fingerprint);
    key_iter_t it = kd->peers(kd);
    const keypair_t *kp;
    while((kp = key_next(&it))){
        ie_dstr_add(e, fprs, build_fpr(e, kp->fingerprint));
    }

    // make an XKEYSYNC command
    imap_cmd_arg_t arg = { .xkeysync = fprs };
    return imap_cmd_new(e, tag, IMAP_CMD_XKEYSYNC, arg);
}
