struct keydir_i;
typedef struct keydir_i keydir_i;

typedef struct {
    link_t *head;
    link_t *next;
} key_iter_t;

const keypair_t *key_next(key_iter_t *it);

/* keydir_i manages keys against a directory of keys.  It is used by both the
   preuser_t and the user_t.  As an interface, it allows the preuser_t and the
   user_t to be tested without filesystem interaction. */
struct keydir_i {
    keypair_t *(*mykey)(keydir_i*);
    key_iter_t (*peers)(keydir_i*);
    derr_t (*add_key)(keydir_i*, const dstr_t pem);
    void (*delete_key)(keydir_i*, const dstr_t bin_fpr);
    void (*free)(keydir_i*);
};
DEF_STEAL_PTR(keydir_i)

// keydir_t is the default keydir_i that uses an actual directory of keys
typedef struct {
    keydir_i iface;
} keydir_t;
DEF_CONTAINER_OF(keydir_t, iface, keydir_i)

derr_t keydir_new(keydir_t *kd, string_builder_t *path);
void keydir_free(keydir_t *kd);

/* we also have some utilities for xkey cmds and responses that are shared
   by the preuser_t and the user_t */

imap_cmd_t *xkeyadd_cmd(derr_t *e, ie_dstr_t *tag, const keypair_t *kp);

imap_cmd_t *xkeysync_cmd(derr_t *e, ie_dstr_t *tag, keydir_i *kd);
