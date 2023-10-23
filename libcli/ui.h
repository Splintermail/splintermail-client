#ifdef _WIN32
// stuff for windows service to work
#define SVCNAME "splintermail"
extern SERVICE_STATUS g_svc_status;
extern SERVICE_STATUS_HANDLE g_svc_status_h;

VOID ReportSvcStatus(DWORD current_state, DWORD exit_code, DWORD wait_hint);
#endif // _WIN32

typedef struct {
    // libdstr
    derr_t (*dir_w_access_path)(
        const string_builder_t *sb, bool create, bool *ret
    );
    derr_t (*dir_rw_access_path)(
        const string_builder_t *sb, bool create, bool *ret
    );
    derr_t (*file_r_access_path)(const string_builder_t *sb, bool *ret);
    derr_t (*file_w_access_path)(const string_builder_t *sb, bool *ret);
    derr_t (*file_rw_access_path)(const string_builder_t *sb, bool *ret);
    derr_t (*exists_path)(const string_builder_t *sb, bool *ret);
    derr_t (*for_each_file_in_dir)(
        const string_builder_t *path, for_each_file_hook_t hook, void *userdata
    );
    // libcrypto
    derr_t (*ssl_library_init)(void);
    void (*ssl_library_close)(void);
    // api_client
    derr_t (*api_token_read_increment_write_path)(
        const string_builder_t *sb, api_token_t *token, bool *ok
    );
    void (*api_token_free0)(api_token_t *token);
    derr_t (*register_api_token_path_sync)(
        http_sync_t *sync,
        const dstr_t baseurl,
        const dstr_t user,
        const dstr_t pass,
        const string_builder_t *sb
    );
    derr_t (*api_pass_sync)(
        http_sync_t *sync,
        const dstr_t baseurl,
        const dstr_t path,
        const dstr_t arg,
        const dstr_t user,
        const dstr_t pass,
        json_t *json
    );
    derr_t (*api_token_sync)(
        http_sync_t *sync,
        const dstr_t baseurl,
        const dstr_t path,
        const dstr_t arg,
        api_token_t token,
        json_t *json
    );
    // console_input.h
    derr_t (*user_prompt)(dstr_t prompt, dstr_t *resp, bool hide);
    // status.h
    derr_t (*status_main)(const string_builder_t sockpath, bool follow);
    // configure.h
    derr_t (*configure_main)(
        const dstr_t baseurl,
        const dstr_t acmeurl,
        const string_builder_t sock,
        const string_builder_t smdir,
        const dstr_t user,
        bool force,
        int *retval
    );
    // libcitm.h
    derr_t (*uv_citm)(
        const addrspec_t *lspecs,
        size_t nlspecs,
        const addrspec_t remote,
        const char *key,
        const char *cert,
        dstr_t acme_dirurl,
        char *acme_verify_name,
        dstr_t sm_baseurl,
        string_builder_t sockpath,
        SSL_CTX *client_ctx,
        string_builder_t sm_dir,
        void (*indicate_ready)(void*, uv_citm_t*),
        void (*user_async_hook)(void*, uv_citm_t*),
        void *user_data
    );
} ui_i;

// pass all calls to the real thing
ui_i default_ui_harness(void);

derr_t trim_logfile(const char *path, long maxlen);

int do_main(const ui_i ui, int argc, char* argv[], bool windows_service);
