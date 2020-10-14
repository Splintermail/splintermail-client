#include "libimap.h"

// throw an error if an action requires an extension to be enabled
derr_t extension_assert_on(const extensions_t *exts,
        extension_e type){
    derr_t e = E_OK;

    extension_state_e state;
    const char *msg;

    switch(type){
        case EXT_UIDPLUS:
            state = exts->uidplus;
            msg = "UIDPLUS extension for IMAP is not available";
            break;
        case EXT_ENABLE:
            state = exts->enable;
            msg = "ENABLE extension for IMAP is not available";
            break;
        case EXT_CONDSTORE:
            state = exts->condstore;
            msg = "CONDSTORE extension for IMAP is not available";
            break;
        case EXT_QRESYNC:
            state = exts->qresync;
            msg = "QRESYNC extension for IMAP is not available";
            break;
        case EXT_UNSELECT:
            state = exts->unselect;
            msg = "UNSELECT extension for IMAP is not available";
            break;
        case EXT_IDLE:
            state = exts->idle;
            msg = "IDLE extension for IMAP is not available";
            break;
        default:
            ORIG(&e, E_INTERNAL, "invalid extension type");
    }

    if(state != EXT_STATE_ON){
        ORIG(&e, E_PARAM, msg);
    }

    return e;
}

// same as above but in a builder-friendly (bison-friendly) signature
void extension_assert_on_builder(derr_t *e,
        const extensions_t *exts, extension_e type){
    IF_PROP(e, extension_assert_on(exts, type)){}
}


// throw an error if an action requires an extension to be available
derr_t extension_assert_available(const extensions_t *exts,
        extension_e type){
    derr_t e = E_OK;

    extension_state_e state;
    const char *msg;

    switch(type){
        case EXT_UIDPLUS:
            state = exts->uidplus;
            msg = "UIDPLUS extension for IMAP is not available";
            break;
        case EXT_ENABLE:
            state = exts->enable;
            msg = "ENABLE extension for IMAP is not available";
            break;
        case EXT_CONDSTORE:
            state = exts->condstore;
            msg = "CONDSTORE extension for IMAP is not available";
            break;
        case EXT_QRESYNC:
            state = exts->qresync;
            msg = "QRESYNC extension for IMAP is not available";
            break;
        case EXT_UNSELECT:
            state = exts->unselect;
            msg = "UNSELECT extension for IMAP is not available";
            break;
        case EXT_IDLE:
            state = exts->idle;
            msg = "IDLE extension for IMAP is not available";
            break;
        default:
            ORIG(&e, E_INTERNAL, "invalid extension type");
    }

    if(state == EXT_STATE_DISABLED){
        ORIG(&e, E_PARAM, msg);
    }

    return e;
}

void extension_assert_available_builder(derr_t *e,
        extensions_t *exts, extension_e type){
    IF_PROP(e, extension_assert_available(exts, type)){}
}


// set an extension to "on", or throw an error if it is disabled
derr_t extension_trigger(extensions_t *exts, extension_e type){
    derr_t e = E_OK;

    extension_state_e *state;
    const char *msg;

    switch(type){
        case EXT_UIDPLUS:
            state = &exts->uidplus;
            msg = "UIDPLUS extension for IMAP is not available";
            break;
        case EXT_ENABLE:
            state = &exts->enable;
            msg = "ENABLE extension for IMAP is not available";
            break;
        case EXT_CONDSTORE:
            state = &exts->condstore;
            msg = "CONDSTORE extension for IMAP is not available";
            break;
        case EXT_QRESYNC:
            state = &exts->qresync;
            msg = "QRESYNC extension for IMAP is not available";
            break;
        case EXT_UNSELECT:
            state = &exts->unselect;
            msg = "UNSELECT extension for IMAP is not available";
            break;
        case EXT_IDLE:
            state = &exts->idle;
            msg = "IDLE extension for IMAP is not available";
            break;
        default:
            ORIG(&e, E_INTERNAL, "invalid extension type");
    }

    if(*state == EXT_STATE_DISABLED){
        ORIG(&e, E_PARAM, msg);
    }

    *state = EXT_STATE_ON;

    return e;
}

// same as above but in a builder-friendly (bison-friendly) signature
void extension_trigger_builder(derr_t *e,
        extensions_t *exts, extension_e type){
    IF_PROP(e, extension_trigger(exts, type)){}
}


DSTR_STATIC(EXT_UIDPLUS_dstr, "UIDPLUS");
DSTR_STATIC(EXT_ENABLE_dstr, "ENABLE");
DSTR_STATIC(EXT_CONDSTORE_dstr, "CONDSTORE");
DSTR_STATIC(EXT_QRESYNC_dstr, "QRESYNC");
DSTR_STATIC(EXT_UNSELECT_dstr, "UNSELECT");
DSTR_STATIC(EXT_IDLE_dstr, "IDLE");
DSTR_STATIC(EXT_unknown_dstr, "unknown");

const dstr_t *extension_token(extension_e ext){
    switch(ext){
        case EXT_UIDPLUS: return &EXT_UIDPLUS_dstr;
        case EXT_ENABLE: return &EXT_ENABLE_dstr;
        case EXT_CONDSTORE: return &EXT_CONDSTORE_dstr;
        case EXT_QRESYNC: return &EXT_QRESYNC_dstr;
        case EXT_UNSELECT: return &EXT_UNSELECT_dstr;
        case EXT_IDLE: return &EXT_IDLE_dstr;
    }
    return &EXT_unknown_dstr;
}
