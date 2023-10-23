typedef struct {
    configure_i iface;
    configure_t c;

    // parameters
    dstr_t baseurl;
    url_t acmeurl;
    string_builder_t status_sock;

    duv_root_t root;

    duv_http_t http;
    api_client_t apic;
    status_client_t sc;

    stream_reader_t reader;
    duv_http_req_t req;
} uv_configure_t;

// user is empty when --user is not present
derr_t uv_configure_main(
    const dstr_t baseurl,
    const dstr_t acmeurl,
    const string_builder_t status_sock,
    const string_builder_t smdir,
    const dstr_t user,
    bool force,
    int *retval
);
