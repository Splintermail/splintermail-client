#include "tools/qwwq/libqw.h"

#include <sys/stat.h>
#include <stdlib.h>
#include <errno.h>

#ifdef _WIN32
// windows
#else
// unix
#include <dlfcn.h>
#endif

DEF_CONTAINER_OF(qw_plugin_t, link, link_t)
DEF_CONTAINER_OF(qw_plugin_t, elem, hash_elem_t)

#ifdef _WIN32
// windows
static void *plugin_open(char *path){
    return LoadLibrary(path);
}
static void plugin_close(HMODULE handle){
    FreeLibrary(handle);
}
static void *plugin_lookup(HMODULE handle, char *name){
    // return (void*)GetProcAddress(handle, name);
    void *out = (void*)GetProcAddress(handle, name);
    if(!out){
        PFMT("lookup failed: %x(): %x\n", FS(name), FWINERR);
    }
    return out;
}
#else
// unix
static void *plugin_open(char *path){
    return dlopen(path, RTLD_LAZY);
}
static void plugin_close(void *handle){
    dlclose(handle);
}
static void *plugin_lookup(void *handle, char *name){
    return dlsym(handle, name);
}
#endif



static void qw_plugin_free(qw_plugin_t *p){
    qw_plugin_free_f fn;
    fn = (qw_plugin_free_f)plugin_lookup(p->handle, "qw_free");
    if(fn) fn();
    plugin_close(p->handle);
    hash_elem_remove(&p->elem);
    link_remove(&p->link);
    dstr_free(&p->name);
    free(p);
}

derr_t qw_plugins_init(
    qw_plugins_t *plugins, qw_origin_t *origin, LIST(dstr_t) paths
){
    derr_t e = E_OK;

    *plugins = (qw_plugins_t){ .origin = origin };
    link_init(&plugins->list);

    PROP_GO(&e, hashmap_init(&plugins->map), fail);

    #ifdef _WIN32
    // windows LoadLibrary requires absolute paths to disable search logic
    DSTR_VAR(cwd, 4096);
    char *cret = _getcwd(cwd.data, (int)cwd.size);
    if(!cret){
        ORIG_GO(&e, E_FIXEDSIZE, "getcwd() too long", fail);
    }
    cwd.len = strlen(cwd.data);
    #endif

    // copy paths
    PROP_GO(&e, LIST_NEW(dstr_t, &plugins->paths, paths.len), fail);
    for(size_t i = 0; i < paths.len; i++){
        dstr_t *path = &plugins->paths.data[plugins->paths.len++];
        *path = (dstr_t){0};
        #ifdef _WIN32
        // windows
        if(isabs(paths.data[i])){
            // absolute paths are fine
            PROP_GO(&e, dstr_copy(&paths.data[i], path), fail);
        }else{
            // prefix relative paths with cwd
            PROP_GO(&e, FMT(path, "%x/%x", FD(cwd), FD(paths.data[i])), fail);
        }
        #else
        // unix
        PROP_GO(&e, dstr_copy(&paths.data[i], path), fail);
        #endif
    }

    return e;

fail:
    for(size_t i = 0; i < plugins->paths.len; i++){
        dstr_free(&plugins->paths.data[i]);
    }
    LIST_FREE(dstr_t, &plugins->paths);
    hashmap_free(&plugins->map);
    return e;
}

void qw_plugins_free(qw_plugins_t *plugins){
    if(!plugins) return;
    // free plugins in reverse order
    link_t *link;
    while((link = link_list_pop_last(&plugins->list))){
        qw_plugin_t *p = CONTAINER_OF(link, qw_plugin_t, link);
        qw_plugin_free(p);
    }
    for(size_t i = 0; i < plugins->paths.len; i++){
        dstr_free(&plugins->paths.data[i]);
    }
    LIST_FREE(dstr_t, &plugins->paths);
    hashmap_free(&plugins->map);
    *plugins = (qw_plugins_t){0};
}

static bool qw_plugin_find(
    LIST(dstr_t) paths, qw_env_t env, dstr_t name, dstr_t *buf
){
    for(size_t i = 0; i < paths.len; i++){
        buf->len = 0;
        derr_type_t etype;
        #ifdef _WIN32
        // windows
        char *fmtstr = "%x/%x.dll";
        #else
        #ifdef __APPLE__
        // mac
        char *fmtstr = "%x/%x.dylib";
        #else
        // linux
        char *fmtstr = "%x/%x.so";
        #endif
        #endif
        etype = FMT_QUIET(buf, fmtstr, FD(paths.data[i]), FD(name));
        if(etype){
            qw_error(env.engine, "plugin name too long: \"%x\"", FD(name));
        }
        struct stat s;
        int ret = stat(buf->data, &s);
        if(ret){
            // file doesn't exist?
            if(errno == ENOENT || errno == ENOTDIR) continue;
            // some other error
            qw_error(env.engine, "stat(%x): %x", FD(*buf), FE(errno));
        }
        // found the library
        PFMT("loaded %x\n", FD(*buf));
        return true;
    }
    return false;
}

qw_val_t *qw_plugin_load(qw_plugins_t *plugins, qw_env_t env, dstr_t name){
    // check cache first
    hash_elem_t *elem = hashmap_gets(&plugins->map, &name);
    if(elem){
        // return cached value
        return CONTAINER_OF(elem, qw_plugin_t, elem)->val;
    }

    // search paths
    DSTR_VAR(buf, 4096);
    bool ok = qw_plugin_find(plugins->paths, env, name, &buf);
    if(!ok){
        qw_error(env.engine, "failed to find plugin \"%x\"", FD(name));
    }

    // allocate memory
    qw_plugin_t *p = malloc(sizeof(*p));
    if(!p) qw_error(env.engine, "out of memory");
    *p = (qw_plugin_t){0};
    derr_type_t etype = dstr_new_quiet(&p->name, name.len);
    if(etype){
        free(p);
        qw_error(env.engine, "out of memory");
    }
    DROP_CMD( dstr_copy(&name, &p->name) );

    // actually load the library
    p->handle = plugin_open(buf.data);
    if(!p->handle){
        dstr_free(&p->name);
        free(p);
        #ifdef _WIN32
        fmt_i *fmterr = FWINERR;
        #else
        fmt_i *fmterr = FS(dlerror());
        #endif
        qw_error(env.engine,
            "failed to load plugin \"%x\": %x", FD(name), fmterr
        );
    }

    // find init function
    qw_plugin_init_f fn;
    fn = (qw_plugin_init_f)plugin_lookup(p->handle, "qw_init");
    if(!fn){
        plugin_close(p->handle);
        dstr_free(&p->name);
        free(p);
        qw_error(env.engine,
            "loaded plugin \"%x\" but found no qw_init()", FD(name)
        );
    }

    // call init function
    qw_env_t plugin_env = { env.engine, plugins->origin };
    p->val = fn(plugin_env);
    if(!p->val){
        plugin_close(p->handle);
        dstr_free(&p->name);
        free(p);
        qw_error(env.engine,
            "loaded plugin \"%x\" but qw_init() returned NULL", FD(name)
        );
    }

    // place plugin in map and list
    hashmap_sets(&plugins->map, &p->name, &p->elem);
    link_list_append(&plugins->list, &p->link);

    return p->val;
}

qw_val_t *qw_mkdict(qw_env_t env, qw_mkdict_t *items, size_t nitems){
    // allocate keymap
    jsw_atree_t *keymap = qw_malloc(env, sizeof(jsw_atree_t), PTRSIZE);
    jsw_ainit(keymap, jsw_cmp_dstr, jsw_get_qw_key);
    // allocate dict
    size_t size = sizeof(qw_dict_t) + PTRSIZE * (nitems - MIN(nitems, 1));
    qw_dict_t *dict = qw_malloc(env, size, PTRSIZE);
    *dict = (qw_dict_t){
        .type = QW_VAL_DICT,
        .keymap = keymap,
        .origin = env.origin,
    };
    // pack keymap and dict->vals
    for(size_t i = 0; i < nitems; i++){
        qw_keymap_add_key(env, keymap, dstr_from_cstr(items[i].key));
        dict->vals[i] = items[i].val;
    }
    return &dict->type;
}
