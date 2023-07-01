typedef struct {
    dstr_t name;
    #ifdef _WIN32
    // windows library handle
    HMODULE handle;
    #else
    // unix library handle
    void *handle;
    #endif
    // cache the return value of qw_init()
    qw_val_t *val;
    link_t link;  // qw_plugins_t->list
    hash_elem_t elem;  // qw_plugins_t->map
} qw_plugin_t;

typedef struct {
    // keep the origin against which we allocate module objects
    qw_origin_t *origin;
    // keep a list of search paths
    LIST(dstr_t) paths;
    /* keep an ordered dict, so we can do key lookups and also reliably free
       plugins in reverse order */
    link_t list;  // qw_plugin_t->link
    hashmap_t map;  // qw_plugin_t->elem
} qw_plugins_t;

// origin must remain valid until qw_plugins_free()
derr_t qw_plugins_init(
    qw_plugins_t *plugins, qw_origin_t *origin, LIST(dstr_t) paths
);
void qw_plugins_free(qw_plugins_t *plugins);

qw_val_t *qw_plugin_load(qw_plugins_t *plugins, qw_env_t env, dstr_t name);

//// PLUGIN API ////

#ifdef _WIN32
// windows dll symbols need explicit exporting
#define QW_EXPORT __declspec(dllexport)
#else
#define QW_EXPORT
#endif

/* the plugin init has access to the stack, but probably shouldn't use it;
   instead it should return a qw_val_t (likely a dict, unless the plugin only
   exposes a single symbol) */
typedef qw_val_t *(*qw_plugin_init_f)(qw_env_t);

// the free function is optional
typedef void (*qw_plugin_free_f)(void);

typedef struct {
    char *key;
    qw_val_t *val;
} qw_mkdict_t;

// helper function for creating a dict with compile-time keys
qw_val_t *qw_mkdict(qw_env_t env, qw_mkdict_t *items, size_t nitems);
#define QW_MKDICT(env, ...) \
    qw_mkdict( \
        env, \
        (qw_mkdict_t[]){__VA_ARGS__}, \
        sizeof((qw_mkdict_t[]){__VA_ARGS__}) / sizeof(qw_mkdict_t) \
    )

/* example plugin exposes both init and free and loads an external library
   which needs to be torn down before exiting:

    QW_EXPORT qw_val_t *qw_init(qw_env_t env);
    QW_EXPORT void qw_free(void);

    qw_func_t func_a = {...};
    qw_func_t func_b = {...};

    qw_val_t *qw_init(qw_env_t env){
        OPENSSL_init_ssl();

        // return a dict as the plugin return value
        return QW_MKDICT(env,
            {"a", &func_a.type},
            {"b", &func_b.type},
        );
    }

    void qw_free(void){
        OPENSSL_cleanup();
    }
*/
