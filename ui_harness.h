typedef struct {
    bool (*dir_r_access)(const char* path, bool create);
    bool (*dir_w_access)(const char* path, bool create);
    bool (*dir_rw_access)(const char* path, bool create);
    bool (*file_r_access)(const char* path);
    bool (*file_w_access)(const char* path);
    bool (*file_rw_access)(const char* path);
    bool (*exists)(const char* path);
    derr_t (*for_each_file_in_dir)(
        const char* path, for_each_file_hook_t hook, void* userdata
    );
} ui_harness_t;

extern ui_harness_t harness;
