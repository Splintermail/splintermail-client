derr_type_t utf8_encode_quiet(
    uint32_t codepoint, derr_type_t (*foreach)(char, void*), void *data
);

derr_type_t utf16_encode_quiet(
    uint32_t codepoint, derr_type_t (*foreach)(uint16_t, void*), void *data
);

/* codepointp and tailp are required, and when in ends with an incomplete utf8
   sequence, they must be preserved for the next invocation */
derr_type_t utf8_decode_stream(
    const char *in,
    size_t len,
    derr_type_t (*foreach)(uint32_t, void*),
    void *data,
    uint32_t *codepointp,
    size_t *tailp
);

derr_type_t utf8_decode_quiet(
    const char *in,
    size_t len,
    derr_type_t (*foreach)(uint32_t, void*),
    void *data
);


typedef struct {
    uint8_t have_bom : 1;
    uint8_t bigendian : 1;
    uint8_t state : 6;
    uint8_t u1;
    uint8_t u2;
    uint8_t u3;
} utf16_state_t;

// create a storage value that acts like we read a big-endian BOM
/* useful if you need to parse from the middle of a utf16 string, or for
   parsing a string of encoding utf16-be, which has no BOM */
utf16_state_t utf16_start_be(void);

// same thing, but little-endian
utf16_state_t utf16_start_le(void);

/* statep is required, and when in ends with an incomplete utf16 surrogate
   pair, it must be preserved for the next invocation */
derr_type_t utf16_decode_stream(
    const char *in,
    size_t len,
    derr_type_t (*foreach)(uint32_t, void*),
    void *data,
    utf16_state_t *statep
);

// expects a BOM at the beginning
derr_type_t utf16_decode_quiet(
    const char *in,
    size_t len,
    derr_type_t (*foreach)(uint32_t, void*),
    void *data
);

// no BOM; forced big-endian
derr_type_t utf16_be_decode_quiet(
    const char *in,
    size_t len,
    derr_type_t (*foreach)(uint32_t, void*),
    void *data
);

// no BOM; forced little-endian
derr_type_t utf16_le_decode_quiet(
    const char *in,
    size_t len,
    derr_type_t (*foreach)(uint32_t, void*),
    void *data
);
