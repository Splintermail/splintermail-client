#include "libimap.h"

#define ORIG_EXT(type) \
    ORIG(&e, \
        E_PARAM, \
        "%x extension for IMAP is not available", \
        FD(extension_token(type)) \
    )

static const extension_state_e *const_extension_find(
    const extensions_t *exts, extension_e type
){
    switch(type){
        case EXT_UIDPLUS:   return &exts->uidplus;
        case EXT_ENABLE:    return &exts->enable;
        case EXT_CONDSTORE: return &exts->condstore;
        case EXT_QRESYNC:   return &exts->qresync;
        case EXT_UNSELECT:  return &exts->unselect;
        case EXT_IDLE:      return &exts->idle;
        case EXT_XKEY:      return &exts->xkey;
    }
    LOG_FATAL("invalid extension type");
    return NULL;
}

static extension_state_e *extension_find(
    extensions_t *exts, extension_e type
){
    switch(type){
        case EXT_UIDPLUS:   return &exts->uidplus;
        case EXT_ENABLE:    return &exts->enable;
        case EXT_CONDSTORE: return &exts->condstore;
        case EXT_QRESYNC:   return &exts->qresync;
        case EXT_UNSELECT:  return &exts->unselect;
        case EXT_IDLE:      return &exts->idle;
        case EXT_XKEY:      return &exts->xkey;
    }
    LOG_FATAL("invalid extension type");
    return NULL;
}

bool extension_is_on(const extensions_t *exts, extension_e type){
    const extension_state_e *state = const_extension_find(exts, type);
    return *state == EXT_STATE_ON;
}

// throw an error if an action requires an extension to be enabled
derr_t extension_assert_on(const extensions_t *exts, extension_e type){
    derr_t e = E_OK;
    if(!extension_is_on(exts, type)) ORIG_EXT(type);
    return e;
}

// same as above but in a builder-friendly signature
void extension_assert_on_builder(derr_t *e,
        const extensions_t *exts, extension_e type){
    IF_PROP(e, extension_assert_on(exts, type)){}
}

bool extension_is_available(const extensions_t *exts, extension_e type){
    const extension_state_e *state = const_extension_find(exts, type);
    return *state != EXT_STATE_DISABLED;
}

// throw an error if an action requires an extension to be available
derr_t extension_assert_available(const extensions_t *exts, extension_e type){
    derr_t e = E_OK;
    if(!extension_is_available(exts, type)) ORIG_EXT(type);
    return e;
}

void extension_assert_available_builder(
    derr_t *e, extensions_t *exts, extension_e type
){
    IF_PROP(e, extension_assert_available(exts, type)){}
}


// set an extension to "on" and return true, or return false if it is disabled
bool extension_trigger(extensions_t *exts, extension_e type){
    extension_state_e *state = extension_find(exts, type);
    if(*state == EXT_STATE_DISABLED) return false;
    *state = EXT_STATE_ON;
    return true;
}


DSTR_STATIC(EXT_UIDPLUS_dstr, "UIDPLUS");
DSTR_STATIC(EXT_ENABLE_dstr, "ENABLE");
DSTR_STATIC(EXT_CONDSTORE_dstr, "CONDSTORE");
DSTR_STATIC(EXT_QRESYNC_dstr, "QRESYNC");
DSTR_STATIC(EXT_UNSELECT_dstr, "UNSELECT");
DSTR_STATIC(EXT_IDLE_dstr, "IDLE");
DSTR_STATIC(EXT_XKEY_dstr, "XKEY");
DSTR_STATIC(EXT_unknown_dstr, "unknown");

const dstr_t *extension_token(extension_e ext){
    switch(ext){
        case EXT_UIDPLUS: return &EXT_UIDPLUS_dstr;
        case EXT_ENABLE: return &EXT_ENABLE_dstr;
        case EXT_CONDSTORE: return &EXT_CONDSTORE_dstr;
        case EXT_QRESYNC: return &EXT_QRESYNC_dstr;
        case EXT_UNSELECT: return &EXT_UNSELECT_dstr;
        case EXT_IDLE: return &EXT_IDLE_dstr;
        case EXT_XKEY: return &EXT_XKEY_dstr;
    }
    return &EXT_unknown_dstr;
}

const char *extension_msg(extension_e ext){
    switch(ext){
        case EXT_UIDPLUS: return "UIDPLUS extension not available";
        case EXT_ENABLE: return "ENABLE extension not available";
        case EXT_CONDSTORE: return "CONDSTORE extension not available";
        case EXT_QRESYNC: return "QRESYNC extension not available";
        case EXT_UNSELECT: return "UNSELECT extension not available";
        case EXT_IDLE: return "IDLE extension not available";
        case EXT_XKEY: return "XKEY extension not available";
    }
    LOG_FATAL("invalid extension type");
    return NULL;
}
