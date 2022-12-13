typedef struct {
    derr_t (*dir_r_access_path)(const string_builder_t *sb, bool create, bool *ret);
    derr_t (*dir_w_access_path)(const string_builder_t *sb, bool create, bool *ret);
    derr_t (*dir_rw_access_path)(const string_builder_t *sb, bool create, bool *ret);
    derr_t (*file_r_access_path)(const string_builder_t *sb, bool *ret);
    derr_t (*file_w_access_path)(const string_builder_t *sb, bool *ret);
    derr_t (*file_rw_access_path)(const string_builder_t *sb, bool *ret);
    derr_t (*exists_path)(const string_builder_t *sb, bool *ret);
    derr_t (*for_each_file_in_dir)(
        const string_builder_t *path, for_each_file_hook_t hook, void *userdata
    );
} ui_harness_t;

extern ui_harness_t harness;
