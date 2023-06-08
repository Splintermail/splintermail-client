// string_bind_in_* requires a dstr_t* to return pointers to valid memory
MYSQL_BIND string_bind_in_ex(const dstr_t *dstr, char *is_null);
MYSQL_BIND string_bind_in(const dstr_t *dstr);
MYSQL_BIND string_bind_out_ex(dstr_t *dstr, char *is_null);
MYSQL_BIND string_bind_out(dstr_t *dstr);

// blob_bind_in_* requires a dstr_t* to return pointers to valid memory
MYSQL_BIND blob_bind_in_ex(const dstr_t *dstr, char *is_null);
MYSQL_BIND blob_bind_in(const dstr_t *dstr);
MYSQL_BIND blob_bind_out_ex(dstr_t *dstr, char *is_null);
MYSQL_BIND blob_bind_out(dstr_t *dstr);

MYSQL_BIND bool_bind_in_ex(const bool *val, char *is_null);
MYSQL_BIND bool_bind_in(const bool *val);
MYSQL_BIND bool_bind_out_ex(bool *val, char *is_null);
MYSQL_BIND bool_bind_out(bool *val);

MYSQL_BIND bool_bind_in_ex(const bool *val, char *is_null);
MYSQL_BIND bool_bind_in(const bool *val);
MYSQL_BIND bool_bind_out_ex(bool *val, char *is_null);
MYSQL_BIND bool_bind_out(bool *val);

MYSQL_BIND uint_bind_in_ex(const unsigned int *val, char *is_null);
MYSQL_BIND uint_bind_in(const unsigned int *val);
MYSQL_BIND uint_bind_out_ex(unsigned int *val, char *is_null);
MYSQL_BIND uint_bind_out(unsigned int *val);

MYSQL_BIND int_bind_in_ex(const int *val, char *is_null);
MYSQL_BIND int_bind_in(const int *val);
MYSQL_BIND int_bind_out_ex(int *val, char *is_null);
MYSQL_BIND int_bind_out(int *val);

MYSQL_BIND uint64_bind_in_ex(const uint64_t *val, char *is_null);
MYSQL_BIND uint64_bind_in(const uint64_t *val);
MYSQL_BIND uint64_bind_out_ex(uint64_t *val, char *is_null);
MYSQL_BIND uint64_bind_out(uint64_t *val);

MYSQL_BIND int64_bind_in_ex(const int64_t *val, char *is_null);
MYSQL_BIND int64_bind_in(const int64_t *val);
MYSQL_BIND int64_bind_out_ex(int64_t *val, char *is_null);
MYSQL_BIND int64_bind_out(int64_t *val);
