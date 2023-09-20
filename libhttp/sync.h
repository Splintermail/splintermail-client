// a struct for making synchronous http requests
typedef struct {
    uv_loop_t loop;
    duv_scheduler_t scheduler;
    duv_http_t http;
    bool initialized;
} http_sync_t;

derr_t http_sync_init(http_sync_t *sync, SSL_CTX *ctx);
void http_sync_free(http_sync_t *sync);

/* response headers: headers you request are filled in linked-list order.
   E.g. if a header is going to appear twice you can select it twice to get
   both values. */
struct hdr_selector_t;
typedef struct hdr_selector_t hdr_selector_t;
struct hdr_selector_t {
    dstr_t key;
    dstr_t *value;
    hdr_selector_t *next;
    bool found;
};

derr_t http_sync_req(
    http_sync_t *sync,
    http_method_e method,
    url_t url,
    http_pairs_t *params,
    http_pairs_t *hdrs,
    const dstr_t body,
    // headers you want to receive
    hdr_selector_t *selectors,
    int *status,
    dstr_t *reason, // output is limited to 256 characters
    dstr_t *resp
);
