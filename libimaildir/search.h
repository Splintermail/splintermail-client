// all functions are thread-unsafe

derr_t search_key_eval(
    const ie_search_key_t *key,
    const msg_view_t *view,
    unsigned int seq,
    unsigned int seq_max,
    unsigned int uid_dn_max,
    // get a read-only copy of either headers or whole body, must be idempotent
    derr_t (*get_hdrs)(void*, const imf_hdrs_t**),
    void *get_hdrs_data,
    derr_t (*get_imf)(void*, const imf_t**),
    void *get_imf_data,
    bool *out
);

// exposed for testing purposes
bool date_a_is_on_b(imap_time_t a, imap_time_t b);
bool date_a_is_before_b(imap_time_t a, imap_time_t b);
bool date_a_is_since_b(imap_time_t a, imap_time_t b);
