#include "imap_extension.h"
#include "logger.h"

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
