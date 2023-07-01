// string methods
void qw_method_strip(qw_env_t env, qw_string_t *string);
void qw_method_lstrip(qw_env_t env, qw_string_t *string);
void qw_method_rstrip(qw_env_t env, qw_string_t *string);
void qw_method_upper(qw_env_t env, qw_string_t *string);
void qw_method_lower(qw_env_t env, qw_string_t *string);
void qw_method_wrap(qw_env_t env, qw_string_t *string);
void qw_method_pre(qw_env_t env, qw_string_t *string);
void qw_method_post(qw_env_t env, qw_string_t *string);
void qw_method_repl(qw_env_t env, qw_string_t *string);
void qw_method_lpad(qw_env_t env, qw_string_t *string);
void qw_method_rpad(qw_env_t env, qw_string_t *string);

// dict methods
void qw_method_get(qw_env_t env, qw_dict_t *dict);

// global builtins
extern qw_func_t qw_builtin_table;
extern qw_func_t qw_builtin_relpath;
extern qw_func_t qw_builtin_cat;
extern qw_func_t qw_builtin_exists;
extern qw_func_t qw_builtin_load;
