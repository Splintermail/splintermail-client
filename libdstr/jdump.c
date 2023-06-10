#include "libdstr/libdstr.h"

#include <string.h>

DEF_CONTAINER_OF(_jdump_int_t, iface, jdump_i)
DEF_CONTAINER_OF(_jdump_uint_t, iface, jdump_i)
DEF_CONTAINER_OF(_jdump_dstr_t, iface, jdump_i)
DEF_CONTAINER_OF(_jdump_str_t, iface, jdump_i)
DEF_CONTAINER_OF(_jdump_strn_t, iface, jdump_i)
DEF_CONTAINER_OF(_jdump_arr_t, iface, jdump_i)
DEF_CONTAINER_OF(_jdump_obj_t, iface, jdump_i)

derr_type_t _jdump_null(jdump_i *iface, writer_i *out, int indent, int pos){
    (void)iface; (void)indent; (void)pos;
    return out->w->puts(out, "null", 4);
}

derr_type_t _jdump_true(jdump_i *iface, writer_i *out, int indent, int pos){
    (void)iface; (void)indent; (void)pos;
    return out->w->puts(out, "true", 4);
}

derr_type_t _jdump_false(jdump_i *iface, writer_i *out, int indent, int pos){
    (void)iface; (void)indent; (void)pos;
    return out->w->puts(out, "false", 5);
}

derr_type_t _jdump_int(jdump_i *iface, writer_i *out, int indent, int pos){
    (void)indent; (void)pos;
    intmax_t i = CONTAINER_OF(iface, _jdump_int_t, iface)->i;
    return FMT_UNLOCKED(out, "%x", FI(i));
}

derr_type_t _jdump_uint(jdump_i *iface, writer_i *out, int indent, int pos){
    (void)indent; (void)pos;
    uintmax_t u = CONTAINER_OF(iface, _jdump_uint_t, iface)->u;
    return FMT_UNLOCKED(out, "%x", FU(u));
}

static derr_type_t quoted_json_encode(const char *s, size_t n, writer_i *out){
    derr_type_t etype;
    writer_t w = *out->w;
    etype = w.putc(out, '"');
    if(etype) return etype;
    etype = json_encode_unlocked(s, n, out);
    if(etype) return etype;
    return w.putc(out, '"');
}

derr_type_t _jdump_dstr(jdump_i *iface, writer_i *out, int indent, int pos){
    (void)indent; (void)pos;
    dstr_t d = CONTAINER_OF(iface, _jdump_dstr_t, iface)->d;
    return quoted_json_encode(d.data, d.len, out);
}

derr_type_t _jdump_str(jdump_i *iface, writer_i *out, int indent, int pos){
    (void)indent; (void)pos;
    const char *s = CONTAINER_OF(iface, _jdump_str_t, iface)->s;
    return quoted_json_encode(s, strlen(s), out);
}

derr_type_t _jdump_strn(jdump_i *iface, writer_i *out, int indent, int pos){
    (void)indent; (void)pos;
    _jdump_strn_t *arg = CONTAINER_OF(iface, _jdump_strn_t, iface);
    return quoted_json_encode(arg->s, arg->n, out);
}

static derr_type_t nl_indent(writer_i *out, int pos){
    derr_type_t etype;
    writer_t w = *out->w;
    // newline and indent
    etype = w.putc(out, '\n');
    if(etype) return etype;
    for(int i = 0; i < pos; i++){
        etype = w.putc(out, ' ');
        if(etype) return etype;
    }
    return E_NONE;
}

derr_type_t _jdump_arr(jdump_i *iface, writer_i *out, int indent, int pos){
    derr_type_t etype;

    _jdump_arr_t *arr = CONTAINER_OF(iface, _jdump_arr_t, iface);
    jdump_i **items = arr->items;
    size_t n = arr->n;
    writer_t w = *out->w;

    etype = w.putc(out, '[');
    if(etype) return etype;

    bool started = false;
    int subpos = pos + indent;
    for(size_t i = 0; i < n; i++){
        jdump_i *item = items[i];
        if(item == NULL) continue;
        // joining comma
        if(!started){
            started = true;
        }else{
            etype = w.putc(out, ',');
            if(etype) return etype;
        }
        // whitespace
        if(indent){
            etype = nl_indent(out, subpos);
            if(etype) return etype;
        }
        // the item
        etype = item->jdump(item, out, indent, subpos);
        if(etype) return etype;
    }

    if(started && indent){
        etype = nl_indent(out, pos);
        if(etype) return etype;
    }

    return w.putc(out, ']');
}

derr_type_t _jdump_obj(jdump_i *iface, writer_i *out, int indent, int pos){
    derr_type_t etype;

    _jdump_obj_t *obj = CONTAINER_OF(iface, _jdump_obj_t, iface);
    jdump_kvp_t *kvps = obj->kvps;
    size_t n = obj->n;
    writer_t w = *out->w;

    etype = w.putc(out, '{');
    if(etype) return etype;

    bool started = false;
    int subpos = pos + indent;
    for(size_t i = 0; i < n; i++){
        dstr_t key = kvps[i].key;
        jdump_i *val = kvps[i].val;
        if(val == NULL) continue;

        // joining comma
        if(!started){
            started = true;
        }else{
            etype = w.putc(out, ',');
            if(etype) return etype;
        }
        // whitespace
        if(indent){
            etype = nl_indent(out, subpos);
            if(etype) return etype;
        }

        // the key
        etype = quoted_json_encode(key.data, key.len, out);
        if(etype) return etype;
        etype = w.putc(out, ':');
        if(etype) return etype;
        if(indent){
            // only put a space after the colon if an indent was requested
            etype = w.putc(out, ' ');
            if(etype) return etype;
        }

        // the val
        etype = val->jdump(val, out, indent, subpos);
        if(etype) return etype;
    }

    if(started && indent){
        etype = nl_indent(out, pos);
        if(etype) return etype;
    }

    return w.putc(out, '}');
}

derr_type_t jdump_quiet(jdump_i *j, writer_i *out, int indent){
    derr_type_t etype;
    writer_t w = *out->w;
    if(w.lock){
        etype = w.lock(out);
        if(etype) return etype;
    }
    etype = j->jdump(j, out, indent, 0);
    derr_type_t etype2 = E_NONE;
    if(w.unlock){
        etype2 = w.lock(out);
    }
    return etype ? etype : etype2;
}

derr_t jdump(jdump_i *j, writer_i *out, int indent){
    derr_t e = E_OK;
    derr_type_t etype = jdump_quiet(j, out, indent);
    if(etype) ORIG(&e, etype, "jdump failed");
    return e;
}
