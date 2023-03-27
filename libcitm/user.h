struct user_t;
typedef struct user_t user_t;
struct user_cb_i;
typedef struct user_cb_i user_cb_i;

struct user_cb_i {
    void (*dying)(user_cb_i*, user_t *caller, derr_t error);
    void (*dead)(user_cb_i*, user_t *caller);
};

struct user_t {
    user_cb_i *cb;
    // the login creds
    dstr_t name;
    dstr_t pass;
    hash_elem_t h;  // citme_t->users
    // the root directory for this user
    string_builder_t path;
    string_builder_t mail_path;
    string_builder_t key_path;
    // each user gets one dirmgr
    dirmgr_t dirmgr;

    // how we inject the citm decryption logic into the imaildir_t
    imaildir_hooks_i imaildir_hooks;

    keypair_t *my_keypair;

    keyshare_t keyshare;

    // logic for client-side 'new key' checks
    fpr_watcher_t fpr_watcher;

    link_t sf_pairs;  // sf_pair_t->link
    size_t npairs;
    bool closed;
    bool canceled;
};
DEF_CONTAINER_OF(user_t, imaildir_hooks, imaildir_hooks_i)
DEF_CONTAINER_OF(user_t, h, hash_elem_t)


// makes a copy of name, pass
// user->path will become root/name
derr_t user_new(
    user_t **out,
    user_cb_i *cb,
    const string_builder_t *root
);

void user_add_sf_pair(user_t *user, sf_pair_t *sf_pair);
void user_remove_sf_pair(user_t *user, sf_pair_t *sf_pair);
derr_t user_mailbox_synced(user_t *user, const dstr_t mailbox);
