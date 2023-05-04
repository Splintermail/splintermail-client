struct keydir_i;
typedef struct keydir_i keydir_i;

/* keydir_i manages keys against a directory of key files.  It is used by both
   the preuser_t and the user_t.  As an interface, it allows the preuser_t and
   the user_t to be tested without filesystem interaction.

   However, the abstraction leaks badly into sf_pair_t. */
struct keydir_i {
    const keypair_t *(*mykey)(keydir_i*);
    link_t *(*peers)(keydir_i*);
    derr_t (*add_key)(keydir_i*, const dstr_t pem);
    void (*delete_key)(keydir_i*, const dstr_t bin_fpr);
    void (*free)(keydir_i*);

    // this part of the abstraction is basically giving up on an abstraction
    // some day the sc_t should be rewritten to be more unit-testable
    dirmgr_t *(*dirmgr)(keydir_i*);
    // a list with both mykey and peers
    link_t *(*all_keys)(keydir_i*);
    // an important event for our fpr_watcher_t
    derr_t (*mailbox_synced)(keydir_i *iface, const dstr_t mailbox);
};
DEF_STEAL_PTR(keydir_i)

/* not part of the interface because it is only called by the citm object
   between the preuser-to-user transition */
derr_t keydir_keysync_completed(keydir_i *iface);

/* keydir_t is the default keydir_i that uses an actual directory of keys.

   In addition to the responsibilities that the keydir_i implies, the keydir_t
   also holds the fpr_watcher and the dirmgr, decrypts messages, and injects
   local messages into the INBOX for new keys or decryption anomalies. */
// root and user must outlive the keydir_i
derr_t keydir_new(
    const string_builder_t *root,
    const dstr_t user,
    keydir_i **out
);
