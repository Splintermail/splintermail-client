#ifndef IMAP_EXTENSION_H
#define IMAP_EXTENSION_H

#include "libdstr/libdstr.h"

typedef enum {
    EXT_UIDPLUS,
    EXT_ENABLE,
    EXT_CONDSTORE,
    EXT_QRESYNC,
} extension_e;

typedef enum {
    // some extensions are auto-enabled, so "off" might not mean "disabled"
    EXT_STATE_DISABLED,
    EXT_STATE_OFF,
    EXT_STATE_ON,
} extension_state_e;

typedef struct {
    extension_state_e uidplus;
    extension_state_e enable;
    extension_state_e condstore;
    extension_state_e qresync;
} extensions_t;

// throw an error if an action requires an extension to be enabled but it's not
derr_t extension_assert_on(const extensions_t *exts,
        extension_e type);

// same as above but in a builder-friendly (bison-friendly) signature
void extension_assert_on_builder(derr_t *e,
        const extensions_t *exts, extension_e type);


// throw an error if an action requires an extension to be available
derr_t extension_assert_available(const extensions_t *exts,
        extension_e type);
void extension_assert_available_builder(derr_t *e,
        extensions_t *exts, extension_e type);

// set an extension to "on", or throw an error if it is disabled
derr_t extension_trigger(extensions_t *exts, extension_e type);
void extension_trigger_builder(derr_t *e,
        extensions_t *exts, extension_e type);

// get the token that would represent an extension
const dstr_t *extension_token(extension_e ext);

#endif // IMAP_EXTENSION_H
