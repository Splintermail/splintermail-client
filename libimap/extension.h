typedef enum {
    EXT_UIDPLUS,
    EXT_ENABLE,
    EXT_CONDSTORE,
    EXT_QRESYNC,
    EXT_UNSELECT,
    EXT_IDLE,
    EXT_XKEY,
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
    extension_state_e unselect;
    extension_state_e idle;
    extension_state_e xkey;
} extensions_t;

bool extension_is_on(const extensions_t *exts, extension_e type);

// throw an error if an action requires an extension to be enabled but it's not
derr_t extension_assert_on(const extensions_t *exts, extension_e type);

// same as above but in a builder-friendly signature
void extension_assert_on_builder(
    derr_t *e, const extensions_t *exts, extension_e type
);

bool extension_is_available(const extensions_t *exts, extension_e type);

// throw an error if an action requires an extension to be available
derr_t extension_assert_available(const extensions_t *exts, extension_e type);
void extension_assert_available_builder(
    derr_t *e, extensions_t *exts, extension_e type
);

// set an extension to "on" and return true, or return false if it is disabled
bool extension_trigger(extensions_t *exts, extension_e type);

// get the token that would represent an extension
dstr_t extension_token(extension_e ext);

const char *extension_msg(extension_e ext);
