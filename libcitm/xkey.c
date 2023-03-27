#include "libcitm/libcitm.h"

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
    const keypair_t *kp;
    LINK_FOR_EACH(kp, kd->peers(kd), keypair_t, link){
        ie_dstr_add(e, fprs, build_fpr(e, kp->fingerprint));
    }

    // make an XKEYSYNC command
    imap_cmd_arg_t arg = { .xkeysync = fprs };
    return imap_cmd_new(e, tag, IMAP_CMD_XKEYSYNC, arg);
}

// check for XKEYSYNC or PLUS responses
derr_t check_xkeysync(imap_resp_t **respp, keydir_i *kd, bool *mykey_deleted){
    derr_t e = E_OK;

    *mykey_deleted = false;

    imap_resp_t *resp = *respp;

    if(resp->type == IMAP_RESP_XKEYSYNC){
        ie_xkeysync_resp_t *xkeysync = resp->arg.xkeysync;
        if(!xkeysync){
            // XKEYSYNC OK is ignored
        }else if(xkeysync->created){
            // got a new pubkey, add it to the keydir_i
            PROP(&e, kd->add_key(kd, xkeysync->created->dstr) );
        }else if(xkeysync->deleted){
            // a key we knew of was deleted
            DSTR_VAR(binfpr, 64);
            PROP(&e, hex2bin(&xkeysync->deleted->dstr, &binfpr) );
            if(dstr_eq(binfpr, *kd->mykey(kd)->fingerprint)){
                // mykey was deleted, time to puke
                *mykey_deleted = true;
            }else{
                // any other key, just delete it from the keydir_i
                kd->delete_key(kd, binfpr);
            }
        }
        // mark this response as consumed
        imap_resp_free(STEAL(imap_resp_t, respp));
        return e;
    }

    /* the `+ OK` resp marks the end of the initial sync, but we don't care;
       the preuser_t pipelines a DONE so it _only_ sees the initial sync, and
       the user_t operates after an initial sync is already complete */
    if(resp->type == IMAP_RESP_PLUS){
        // mark this response as consumed
        imap_resp_free(STEAL(imap_resp_t, respp));
        return e;
    }

    // not handled here
    return e;
}
