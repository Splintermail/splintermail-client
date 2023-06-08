#include "libdstr/libdstr.h"

#include <string.h>

derr_type_t _sb_write_dstr(const string_builder_t *sb, writer_i *out){
    dstr_t d = sb->arg.dstr;
    return out->w->puts(out, d.data, d.len);
}

derr_type_t _sb_write_str(const string_builder_t *sb, writer_i *out){
    char *str = (char*)sb->arg.args.a.ptr;
    return out->w->puts(out, str, strlen(str));
}

derr_type_t _sb_write_strn(const string_builder_t *sb, writer_i *out){
    char *str = (char*)sb->arg.args.a.ptr;
    size_t len = sb->arg.args.b.z;
    return out->w->puts(out, str, len);
}

derr_type_t _sb_write_int(const string_builder_t *sb, writer_i *out){
    intmax_t i = sb->arg.args.a.i;
    return _fmt_int(FI(i), out);
}

derr_type_t _sb_write_uint(const string_builder_t *sb, writer_i *out){
    uintmax_t u = sb->arg.args.a.u;
    return _fmt_uint(FU(u), out);
}

string_builder_t sb_append(const string_builder_t *prev, string_builder_t sb){
    sb.prev = prev;
    return sb;
}

string_builder_t sb_prepend(const string_builder_t *next, string_builder_t sb){
    sb.next = next;
    return sb;
}

derr_t sb_expand(
    const string_builder_t* sb,
    dstr_t* stack_dstr,
    dstr_t* heap_dstr,
    dstr_t** out
){
    derr_t e = E_OK;

    // try and expand into stack_dstr
    derr_type_t etype = FMT_QUIET(stack_dstr, "%x", FSB(*sb));
    if(etype != E_NONE) goto use_heap;
    // it worked, return stack_dstr as *out
    *out = stack_dstr;
    return E_OK;

use_heap:
    // we will need to allocate the heap_dstr to be bigger than stack_dstr
    PROP(&e, dstr_new(heap_dstr, stack_dstr->size * 2) );
    PROP_GO(&e, FMT(heap_dstr, "%x", FSB(*sb)), fail_heap);
    *out = heap_dstr;
    return e;

fail_heap:
    dstr_free(heap_dstr);
    return e;
}

static derr_type_t wput_sb(writer_i *out, string_builder_t sb, dstr_t joiner){
    derr_type_t etype;

    if(sb.prev != NULL){
        // prev element
        etype = wput_sb(out, *sb.prev, joiner);
        if(etype) return etype;
        // joiner
        etype = out->w->puts(out, joiner.data, joiner.len);
        if(etype) return etype;
    }

    // this element
    etype = sb.write(&sb, out);
    if(etype) return etype;

    if(sb.next != NULL){
        // joiner element
        etype = out->w->puts(out, joiner.data, joiner.len);
        if(etype) return etype;
        // next
        etype = wput_sb(out, *sb.next, joiner);
        if(etype) return etype;
    }

    return E_NONE;
}

DEF_CONTAINER_OF(_fmt_sb_t, iface, fmt_i)

derr_type_t _fmt_sb(const fmt_i *iface, writer_i *out){
    _fmt_sb_t *arg = CONTAINER_OF(iface, _fmt_sb_t, iface);

    return wput_sb(out, arg->sb, arg->joiner);
}
