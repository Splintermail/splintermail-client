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
    // the root of the user's maildir
    string_builder_t path;
    // each user gets one dirmgr
    dirmgr_t dirmgr;

    // a manager_i for the key-fetching fetcher_t
    manager_i keyfetcher_mgr;
    fetcher_t *keyfetcher;

    refs_t refs;

    link_t sf_pairs;  // sf_pair_t->link
    size_t npairs;
    bool closed;
    bool canceled;
};
DEF_CONTAINER_OF(user_t, keyfetcher_mgr, manager_i);
DEF_CONTAINER_OF(user_t, h, hash_elem_t);
DEF_CONTAINER_OF(user_t, refs, refs_t);


// makes a copy of name
// user->path will become root/name
derr_t user_new(
    user_t **user,
    user_cb_i *cb,
    const dstr_t *name,
    const dstr_t *pass,
    const string_builder_t *root
);

void user_add_sf_pair(user_t *user, sf_pair_t *sf_pair);
void user_remove_sf_pair(user_t *user, sf_pair_t *sf_pair);
