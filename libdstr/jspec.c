#include "libdstr/libdstr.h"

#include <stdlib.h>
#include <string.h>

DEF_CONTAINER_OF(jspec_dstr_t, jspec, jspec_t)
DEF_CONTAINER_OF(jspec_bool_t, jspec, jspec_t)
DEF_CONTAINER_OF(jspec_object_t, jspec, jspec_t)
DEF_CONTAINER_OF(jspec_map_t, jspec, jspec_t)
DEF_CONTAINER_OF(jspec_optional_t, jspec, jspec_t)
DEF_CONTAINER_OF(jspec_tuple_t, jspec, jspec_t)
DEF_CONTAINER_OF(jspec_list_t, jspec, jspec_t)
DEF_CONTAINER_OF(jspec_jptr_t, jspec, jspec_t)

#define DECLARE_NUMERICS(suffix, type) \
    DEF_CONTAINER_OF(jspec_to ## suffix ## _t, jspec, jspec_t)
INTEGERS_MAP(DECLARE_NUMERICS)
#undef DECLARE_NUMERICS

DEF_CONTAINER_OF(jspec_xdstr_t, jspec, jspec_t)
DEF_CONTAINER_OF(jspec_xstr_t, jspec, jspec_t)
DEF_CONTAINER_OF(jspec_xstrn_t, jspec, jspec_t)

jctx_t jctx_fork(jctx_t *base, json_node_t *node, bool *ok, dstr_t *errbuf){
    jctx_t ctx = {
        .node = node,
        .parent = base ? base->parent : NULL,
        .ok = ok,
        .errbuf = errbuf,
    };
    return ctx;
}

// most use cases would just create subcontexts, which don't need freeing
jctx_t jctx_sub_index(jctx_t *base, size_t index, json_node_t *node){
    jctx_t ctx = {
        .node = node,
        .parent = base,
        .ok = base->ok,
        .errbuf = base->errbuf,
        .index = index,
        .istext = false,
    };
    return ctx;
}

jctx_t jctx_sub_key(jctx_t *base, dstr_t text, json_node_t *node){
    jctx_t ctx = {
        .node = node,
        .parent = base,
        .ok = base->ok,
        .errbuf = base->errbuf,
        .text = text,
        .istext = true,
    };
    return ctx;
}

// recursive layer under _fmt_jpath
static derr_type_t jpath_render_quiet(const jctx_t *ctx, writer_i *out){
    if(!ctx->parent){
        // root item
        return out->w->puts(out, "<root>", 6);
    }
    // print parent first
    derr_type_t etype = jpath_render_quiet(ctx->parent, out);
    if(etype) return etype;
    if(ctx->istext){
        return FMT_UNLOCKED(out, ".%x", FD_DBG(ctx->text));
    }else{
        return FMT_UNLOCKED(out, "[%x]", FU(ctx->index));
    }
}

DEF_CONTAINER_OF(_fmt_jpath_t, iface, fmt_i)

derr_type_t _fmt_jpath(const fmt_i *iface, writer_i *out){
    const jctx_t *ctx = CONTAINER_OF(iface, _fmt_jpath_t, iface)->ctx;
    // call recursive layer
    return jpath_render_quiet(ctx, out);
}

derr_type_t _jctx_error(
    jctx_t *ctx, const char *fstr, const fmt_i **args, size_t nargs
){
    *ctx->ok = false;
    if(!ctx->errbuf) return E_NONE;
    return _fmt_quiet(WD(ctx->errbuf), fstr, args, nargs);
}

bool _jctx_require_type(jctx_t *ctx, json_type_e *types, size_t ntypes){
    json_type_e node_type = ctx->node->type;
    for(size_t i = 0; i < ntypes; i++){
        if(node_type == types[i]) return true;
    }
    if(ntypes == 1){
        jctx_error(ctx,
            "expected %x-type but found %x-type\n",
            FD(json_type_to_dstr(types[0])),
            FD(json_type_to_dstr(node_type))
        );
    }else{
        // finite number of types, so this is ok
        DSTR_VAR(buf, 256);
        for(size_t i = 0; i < ntypes; i++){
            if(i) dstr_append_quiet(&buf, &DSTR_LIT(", "));
            FMT_QUIET(&buf, "%x", FD(json_type_to_dstr(types[i])));
        }
        jctx_error(ctx,
            "expected one of [%x] types but found %x-type\n",
            FD(buf),
            FD(json_type_to_dstr(node_type))
        );
    }
    return false;
}

derr_t jspec_read_ex(jspec_t *jspec, json_ptr_t ptr, bool *ok, dstr_t *errbuf){
    derr_t e = E_OK;

    if(ptr.error){
        ORIG(&e, E_PARAM, "invalid json reference");
    }

    *ok = true;
    jctx_t ctx = jctx_fork(NULL, ptr.node, ok, errbuf);

    PROP(&e, jspec->read(jspec, &ctx) );

    return e;
}

derr_t jspec_read(jspec_t *jspec, json_ptr_t ptr){
    derr_t e = E_OK;

    dstr_t errbuf = {0};
    bool ok;

    PROP_GO(&e, jspec_read_ex(jspec, ptr, &ok, &errbuf), fail);
    if(!ok){
        // just steal errbuf
        e.msg = errbuf;
        ORIG(&e, E_PARAM, "json did not match jspec");
    }

fail:
    dstr_free(&errbuf);
    return e;
}

// jspec_t implementations //

derr_t jspec_dstr_read(jspec_t *jspec, jctx_t *ctx){
    derr_t e = E_OK;

    if(!jctx_require_type(ctx, JSON_STRING)) return e;
    dstr_t text = jctx_text(ctx);
    jspec_dstr_t *j = CONTAINER_OF(jspec, jspec_dstr_t, jspec);
    if(j->copy){
        PROP(&e, dstr_copy(&text, j->out) );
    }else{
        *j->out = text;
    }

    return e;
}

derr_t jspec_bool_read(jspec_t *jspec, jctx_t *ctx){
    derr_t e = E_OK;

    if(!jctx_require_type(ctx, JSON_TRUE, JSON_FALSE)) return e;
    jspec_bool_t *j = CONTAINER_OF(jspec, jspec_bool_t, jspec);
    *j->out = (ctx->node->type == JSON_TRUE);

    return e;
}

static int cmpkeys(const void *va, const void *vb){
    // since .key comes first in _jkey_t, we'll pretend we dstr_t*'s
    const dstr_t *a = va;
    const dstr_t *b = vb;
    return dstr_cmp2(*a, *b);
}

derr_t jspec_object_read(jspec_t *jspec, jctx_t *ctx){
    derr_t e = E_OK;

    if(!jctx_require_type(ctx, JSON_OBJECT)) return e;

    jspec_object_t *j = CONTAINER_OF(jspec, jspec_object_t, jspec);

#ifdef BUILD_DEBUG
    // check that keys are pre-sorted, or bsearch will fail
    if(j->nkeys){
        dstr_t prev = j->keys[0].key;
        for(size_t i = 1; i < j->nkeys; i++){
            dstr_t key = j->keys[i].key;
            if(dstr_cmp2(prev, key) > -1){
                LOG_FATAL(
                    "JOBJ keys are not presorted; \"%x\" is before \"%x\"\n",
                    FD(prev), FD(key)
                );
            }
            prev = key;
        }
    }
#endif

    for(json_node_t *key = ctx->node->child; key; key = key->next){
        dstr_t keytext = key->text;
        json_node_t *value = key->child;

        // look for matching jspec
        _jkey_t *match = bsearch(
            &keytext, j->keys, j->nkeys, sizeof(*j->keys), cmpkeys
        );
        if(!match){
            if(!j->allow_extras){
                jctx_error(ctx, "unexpected key: \"%x\"\n", FD(keytext));
            }
            continue;
        }

        if(match->found){
            jctx_error(ctx,
                "duplicate entries for key: \"%x\"\n", FD(keytext)
            );
            continue;
        }

        match->found = true;
        if(match->present) *match->present = true;

        // descend into subkey
        jctx_t subctx = jctx_sub_key(ctx, keytext, value);
        PROP(&e, jctx_read(&subctx, match->value) );
    }

    // check that all required keys were found
    for(size_t i = 0; i < j->nkeys; i++){
        _jkey_t *jkey = &j->keys[i];
        if(!jkey->found){
            if(jkey->present){
                *jkey->present = false;
            }else{
                jctx_error(ctx,
                    "missing required key: \"%x\"\n", FD(jkey->key)
                );
            }
        }
    }

    return e;
}

derr_t jspec_map_read(jspec_t *jspec, jctx_t *ctx){
    derr_t e = E_OK;

    if(!jctx_require_type(ctx, JSON_OBJECT)) return e;

    jspec_map_t *j = CONTAINER_OF(jspec, jspec_map_t, jspec);

    size_t index = 0;
    for(json_node_t *key = ctx->node->child; key; key = key->next){
        dstr_t keytext = key->text;
        jctx_t subctx = jctx_sub_key(ctx, keytext, key->child);
        PROP(&e, j->read_kvp(&subctx, keytext, index, j->data) );
        index++;
    }

    return e;
}

derr_t jspec_optional_read(jspec_t *jspec, jctx_t *ctx){
    derr_t e = E_OK;

    jspec_optional_t *j = CONTAINER_OF(jspec, jspec_optional_t, jspec);

    if(ctx->node->type == JSON_NULL){
        *j->nonnull = false;
        return e;
    }

    *j->nonnull = true;
    PROP(&e, jctx_read(ctx, j->subspec) );

    return e;
}

derr_t jspec_tuple_read(jspec_t *jspec, jctx_t *ctx){
    derr_t e = E_OK;

    if(!jctx_require_type(ctx, JSON_ARRAY)) return e;

    jspec_tuple_t *j = CONTAINER_OF(jspec, jspec_tuple_t, jspec);

    json_node_t *item = ctx->node->child;
    for(size_t i = 0; i < j->nitems; i++){
        if(!item){
            jctx_error(ctx, "not enough items in tuple\n");
            return e;
        }
        // descend into this item
        jctx_t subctx = jctx_sub_index(ctx, i, item);
        PROP(&e, jctx_read(&subctx, j->items[i]) );
        item = item->next;
    }
    if(item){
        jctx_error(ctx, "too many items in tuple\n");
    }

    return e;
}

derr_t jspec_list_read(jspec_t *jspec, jctx_t *ctx){
    derr_t e = E_OK;

    if(!jctx_require_type(ctx, JSON_ARRAY)) return e;

    jspec_list_t *j = CONTAINER_OF(jspec, jspec_list_t, jspec);

    size_t index = 0;
    for(json_node_t *item = ctx->node->child; item; item = item->next){
        jctx_t subctx = jctx_sub_index(ctx, index, item);
        PROP(&e, j->read_item(&subctx, index, j->data) );
        index++;
    }

    return e;
}

derr_t jspec_jptr_read(jspec_t *jspec, jctx_t *ctx){
    derr_t e = E_OK;

    jspec_jptr_t *j = CONTAINER_OF(jspec, jspec_jptr_t, jspec);

    *j->ptr = (json_ptr_t){ .node = ctx->node };

    return e;
}

#define DEFINE_INTEGERS(suffix, type) \
    derr_t jspec_to ## suffix ## _read(jspec_t *jspec, jctx_t *ctx){ \
        derr_t e = E_OK; \
        if(!jctx_require_type(ctx, JSON_NUMBER)) return e; \
        jspec_to ## suffix ## _t *j = \
            CONTAINER_OF(jspec, jspec_to ## suffix ## _t, jspec); \
        dstr_t text = jctx_text(ctx); \
        derr_type_t etype = dstr_to ## suffix ## _quiet(text, j->out, 10); \
        if(etype != E_NONE){ \
            jctx_error(ctx, \
                "unable to convert \"%x\" into " #type "\n", FD(text) \
            ); \
        } \
        return e; \
    }

INTEGERS_MAP(DEFINE_INTEGERS)

// jspect-expect series

derr_t jspec_xdstr_read(jspec_t *jspec, jctx_t *ctx){
    derr_t e = E_OK;

    if(!jctx_require_type(ctx, JSON_STRING)) return e;
    dstr_t text = jctx_text(ctx);
    dstr_t d = CONTAINER_OF(jspec, jspec_xdstr_t, jspec)->d;
    if(!dstr_eq(text, d)){
        jctx_error(ctx,
            "wrong value: expected \"%x\" but got \"%x\"\n",
            FD_DBG(d),
            FD_DBG(text)
        );
    }
    return e;
}

derr_t jspec_xstr_read(jspec_t *jspec, jctx_t *ctx){
    derr_t e = E_OK;

    if(!jctx_require_type(ctx, JSON_STRING)) return e;
    dstr_t text = jctx_text(ctx);
    const char *s = CONTAINER_OF(jspec, jspec_xstr_t, jspec)->s;
    size_t n = strlen(s);
    if(text.len != n || strncmp(text.data, s, text.len) != 0){
        jctx_error(ctx,
            "wrong value: expected \"%x\" but got \"%x\"\n",
            FSN_DBG(s, n),
            FD_DBG(text)
        );
    }
    return e;
}

derr_t jspec_xstrn_read(jspec_t *jspec, jctx_t *ctx){
    derr_t e = E_OK;

    if(!jctx_require_type(ctx, JSON_STRING)) return e;
    dstr_t text = jctx_text(ctx);
    jspec_xstrn_t *arg = CONTAINER_OF(jspec, jspec_xstrn_t, jspec);
    const char *s = arg->s;
    size_t n = arg->n;
    if(text.len != n || strncmp(text.data, s, text.len) != 0){
        jctx_error(ctx,
            "wrong value: expected \"%x\" but got \"%x\"\n",
            FSN_DBG(s, n),
            FD_DBG(text)
        );
    }
    return e;
}

