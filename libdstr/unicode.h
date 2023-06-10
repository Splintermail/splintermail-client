derr_type_t utf8_encode_quiet(
    uint32_t codepoint, derr_type_t (*foreach)(char, void*), void *data
);

derr_type_t utf16_encode_quiet(
    uint32_t codepoint, derr_type_t (*foreach)(uint16_t, void*), void *data
);

derr_type_t utf8_decode_quiet(
    const char *in,
    size_t len,
    derr_type_t (*foreach)(uint32_t, void*),
    void *data
);
