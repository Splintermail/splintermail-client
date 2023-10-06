#include "libcitm/libcitm.h"

#define STATUS_DSTR(name, val, str) case name: return DSTR_LIT(str);
dstr_t status_maj_dstr(status_maj_e maj){
    switch(maj){
        STATUS_MAJ_MAP(STATUS_DSTR)
        default: return DSTR_LIT("UNKNOWN");
    }
}
dstr_t status_min_dstr(status_min_e min){
    switch(min){
        STATUS_MIN_MAP(STATUS_DSTR)
        default: return DSTR_LIT("UNKNOWN");
    }
}
#undef STATUS_DSTR

DEF_CONTAINER_OF(_jdump_tri_t, iface, jdump_i)

derr_type_t _jdump_tri(jdump_i *iface, writer_i *out, int indent, int pos){
    (void)indent; (void)pos;
    tri_e tri = CONTAINER_OF(iface, _jdump_tri_t, iface)->tri;
    switch(tri){
        case TRI_NO: return out->w->puts(out, "\"no\"", 4);
        case TRI_YES: return out->w->puts(out, "\"yes\"", 5);
        case TRI_NA: return out->w->puts(out, "\"na\"", 4);
    }
    LOG_FATAL("unknown tristate value: %x", FI(tri));
}

DEF_CONTAINER_OF(_jspec_tri_t, jspec, jspec_t)

derr_t _jspec_tri(jspec_t *jspec, jctx_t *ctx){
    derr_t e = E_OK;

    if(!jctx_require_type(ctx, JSON_STRING)) return e;
    tri_e *out = CONTAINER_OF(jspec, _jspec_tri_t, jspec)->out;
    dstr_t text = jctx_text(ctx);
    if(dstr_eq(text, DSTR_LIT("no"))){
        *out = TRI_NO;
    }else if(dstr_eq(text, DSTR_LIT("yes"))){
        *out = TRI_YES;
    }else if(dstr_eq(text, DSTR_LIT("na"))){
        *out = TRI_NA;
    }else{
        jctx_error(ctx,
            "expected one of \"no\", \"yes\", or \"na\" but found %x\n",
            FD(text)
        );
        *out = 0;
    }

    return e;
}

derr_t citm_status_init(
    citm_status_t *status,
    int version_maj,
    int version_min,
    int version_patch,
    dstr_t fulldomain,
    dstr_t status_maj,
    dstr_t status_min,
    tri_e configured,
    tri_e tls_ready
){
    derr_t e = E_OK;

    *status = (citm_status_t){
        .version_maj = version_maj,
        .version_min = version_min,
        .version_patch = version_patch,
        .configured = configured,
        .tls_ready = tls_ready,
    };

    PROP_GO(&e, dstr_copy2(fulldomain, &status->fulldomain), fail);
    PROP_GO(&e, dstr_copy2(status_maj, &status->status_maj), fail);
    PROP_GO(&e, dstr_copy2(status_min, &status->status_min), fail);

    return e;

fail:
    citm_status_free(status);
    return e;
}

void citm_status_free(citm_status_t *status){
    dstr_free(&status->fulldomain);
    dstr_free(&status->status_maj);
    dstr_free(&status->status_min);
    *status = (citm_status_t){0};
}
