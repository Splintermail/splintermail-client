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
