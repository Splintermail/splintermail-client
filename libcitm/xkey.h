imap_cmd_t *xkeyadd_cmd(derr_t *e, ie_dstr_t *tag, const keypair_t *kp);

imap_cmd_t *xkeysync_cmd(derr_t *e, ie_dstr_t *tag, keydir_i *kd);

// check for XKEYSYNC or PLUS responses
derr_t check_xkeysync(imap_resp_t **respp, keydir_i *kd, bool *mykey_deleted);
