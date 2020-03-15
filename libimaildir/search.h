// all functions are thread-unsafe

derr_t search_key_eval(const ie_search_key_t *key, const msg_view_t *view,
        unsigned int seq, unsigned int seq_max, unsigned int uid_max,
        bool *out);
