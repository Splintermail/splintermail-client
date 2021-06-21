/* fpr_watcher_t implements the logic for client-side alerts when detecting new
   fingerprints.

   A message should be shown to the client for every "new" fingerprint we see.

   A "new" fingerprint means one that we haven't seen before in a folder that
   we have previously synced (no alerts for ancient messages)

   A message should also be shown to the client if a new fingerprint is
   detected during XKEYSYNC (after the first XKEYSYNC is complete), though of
   course no fingerprint should result in multiple messages.

   That does imply that a fingerprint which is added/removed/re-added will not
   cause two messages.

   Lastly, a special message could be sent that says that a fingerprint was
   detected as 're-added', but that might be confusing because some clients
   would catch the transition but others wouldn't.  So for now we will not. */
typedef struct {
    // fprs is an append-only list of all fingerprints we have ever seen
    jsw_atree_t fprs;  // jsw_str_t->node
    // synced is a list of folders we have synced in the past
    jsw_atree_t synced;  // jsw_str_t->node
    // whether or not we have finished an XKEYSYNC
    bool xkeysynced;
    // the directory where we persist our state
    string_builder_t path;
} fpr_watcher_t;

// observed a fingerprint while decrypting
bool fpr_watcher_should_alert_on_decrypt(
    fpr_watcher_t *w, const dstr_t fpr_bin, const dstr_t mailbox
);

// observed a new fingerprint from XKEYSYNC
bool fpr_watcher_should_alert_on_new_key(
    fpr_watcher_t *w, const dstr_t fpr_bin
);

derr_t fpr_watcher_xkeysync_completed(fpr_watcher_t *w);
derr_t fpr_watcher_mailbox_synced(fpr_watcher_t *w, const dstr_t mailbox);

// presumably you've already sucessfully injected a message if you needed to
// (that implies we prefer duplicate user alerts to dropped user alerts)
derr_t fpr_watcher_add_fpr(fpr_watcher_t *w, const dstr_t fpr_bin);

void fpr_watcher_free(fpr_watcher_t *w);
derr_t fpr_watcher_init(fpr_watcher_t *w, string_builder_t path);
