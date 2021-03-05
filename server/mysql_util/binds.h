MYSQL_BIND string_bind_in(const dstr_t *dstr);
MYSQL_BIND string_bind_out(dstr_t *dstr);

MYSQL_BIND blob_bind_in(const dstr_t *dstr);
MYSQL_BIND blob_bind_out(dstr_t *dstr);

MYSQL_BIND bool_bind_in(const bool *val);
MYSQL_BIND bool_bind_out(bool *val);

MYSQL_BIND bool_bind_in(const bool *val);
MYSQL_BIND bool_bind_out(bool *val);

MYSQL_BIND uint_bind_in(const unsigned int *val);
MYSQL_BIND uint_bind_out(unsigned int *val);

MYSQL_BIND uint64_bind_in(const uint64_t *val);
MYSQL_BIND uint64_bind_out(uint64_t *val);
