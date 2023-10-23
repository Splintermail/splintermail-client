struct configure_i;
typedef struct configure_i configure_i;

typedef struct {
    advancer_t advancer;

    configure_i *ci;

    // parameters
    string_builder_t smdir;
    bool force;
    bool wait;
    int *retval;

    citm_status_t status;
    dstr_t user;
    dstr_t pass;
    dstr_t resp;
    dstr_t tos_url;
    // when we create an installation, we want to zeroize the json parsed text
    json_t json;
    dstr_t json_text;
    char _json_text[4096];
    json_node_t json_nodes[64];
    installation_t inst;

    bool init : 1;

    bool sc_check : 1;

    bool configure_done : 1;
    bool tos_started : 1;
    bool tos_done : 1;
    bool agreement : 1;
    bool inst_started : 1;
    bool inst_done : 1;

    bool wait_msg_sent : 1;
    bool status_dirty : 1;
} configure_t;

derr_t configure_init(
    configure_t *c,
    configure_i *ci,
    scheduler_i *scheduler,
    const string_builder_t smdir,
    const dstr_t user,
    bool force,
    int *retval
);

struct configure_i {
    // resource handling
    derr_t (*io_init)(configure_i*, bool sc);
    bool (*io_close)(configure_i*);

    // async calls
    void (*status_client_check)(configure_i*);
    void (*get_acme_dir)(configure_i*, dstr_t *resp);
    void (*add_inst)(configure_i*, dstr_t user, dstr_t pass, json_t *resp);

    // blocking calls
    void (*print)(configure_i*, const char *fstr, const fmt_i *arg);
    derr_t (*prompt_one_of)(
        configure_i*, dstr_t msg, const char *opts, size_t *ret
    );
    derr_t (*user_prompt)(configure_i*, dstr_t msg, dstr_t *out, bool hide);
    derr_t (*mkdirs)(configure_i*, string_builder_t path);
    derr_t (*dir_rw_access)(configure_i*, string_builder_t path, bool *ret);
    derr_t (*write_inst)(configure_i*, installation_t, string_builder_t);
};

void configure_get_acme_dir_done(configure_t *c, derr_t e);
void configure_add_inst_done(configure_t *c, derr_t e);
void configure_status_client_update(configure_t *c, citm_status_t status);
void configure_status_client_done(configure_t *c, derr_t e);
