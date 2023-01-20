#include "mysql_util.h"

MYSQL_BIND string_bind_in_ex(const dstr_t *dstr, char *is_null) {
    return (MYSQL_BIND){
        .buffer_type = MYSQL_TYPE_STRING,
        .buffer = dstr->data,
        .buffer_length = dstr->size,
        .length = &dstr->len,
        .is_null = is_null,
    };
}
MYSQL_BIND string_bind_in(const dstr_t *dstr) {
    return string_bind_in_ex(dstr, NULL);
}

MYSQL_BIND string_bind_out_ex(dstr_t *dstr, char *is_null) {
    return (MYSQL_BIND){
        .buffer_type = MYSQL_TYPE_STRING,
        .buffer = dstr->data,
        .buffer_length = dstr->size,
        .length = &dstr->len,
        .is_null = is_null,
    };
}
MYSQL_BIND string_bind_out(dstr_t *dstr) {
    return string_bind_out_ex(dstr, NULL);
}

MYSQL_BIND blob_bind_in_ex(const dstr_t *dstr, char *is_null) {
    return (MYSQL_BIND){
        .buffer_type = MYSQL_TYPE_BLOB,
        .buffer = dstr->data,
        .buffer_length = dstr->size,
        .length = &dstr->len,
        .is_null = is_null,
    };
}
MYSQL_BIND blob_bind_in(const dstr_t *dstr) {
    return blob_bind_in_ex(dstr, NULL);
}

MYSQL_BIND blob_bind_out_ex(dstr_t *dstr, char *is_null) {
    return (MYSQL_BIND){
        .buffer_type = MYSQL_TYPE_BLOB,
        .buffer = dstr->data,
        .buffer_length = dstr->size,
        .length = &dstr->len,
        .is_null = is_null,
    };
}
MYSQL_BIND blob_bind_out(dstr_t *dstr) {
    return blob_bind_out_ex(dstr, NULL);
}


MYSQL_BIND bool_bind_in_ex(const bool *val, char *is_null){
    // inputs are integer-like
    return (MYSQL_BIND){
        .buffer_type = MYSQL_TYPE_TINY,
        .buffer = val,
        .is_unsigned = true,
        .is_null = is_null,
    };
}
MYSQL_BIND bool_bind_in(const bool *val){
    return bool_bind_in_ex(val, NULL);
}

MYSQL_BIND bool_bind_out_ex(bool *val, char *is_null){
    // outputs are char[]-like
    return (MYSQL_BIND){
        .buffer_type = MYSQL_TYPE_BIT,
        .buffer = val,
        .buffer_length = sizeof(*val),
        .is_null = is_null,
    };
}
MYSQL_BIND bool_bind_out(bool *val){
    return bool_bind_out_ex(val, NULL);
}

MYSQL_BIND uint_bind_in_ex(const unsigned int *val, char *is_null){
    return (MYSQL_BIND){
        /* c type of int really is MYSQL_TYPE_LONG */
        /* see: https://dev.mysql.com/doc/c-api/8.0/en/c-api-prepared-statement-type-codes.html */
        .buffer_type = MYSQL_TYPE_LONG,
        .buffer = val,
        .is_unsigned = true,
        .is_null = is_null,
    };
}
MYSQL_BIND uint_bind_in(const unsigned int *val){
    return uint_bind_in_ex(val, NULL);
}

MYSQL_BIND uint_bind_out_ex(unsigned int *val, char *is_null){
    return (MYSQL_BIND){
        .buffer_type = MYSQL_TYPE_LONG,
        .buffer = val,
        .buffer_length = sizeof(*val),
        .is_unsigned = true,
        .is_null = is_null,
    };
}
MYSQL_BIND uint_bind_out(unsigned int *val){
    return uint_bind_out_ex(val, NULL);
}

MYSQL_BIND int_bind_in_ex(const int *val, char *is_null){
    return (MYSQL_BIND){
        .buffer_type = MYSQL_TYPE_LONG,
        .buffer = val,
        .is_unsigned = false,
        .is_null = is_null,
    };
}
MYSQL_BIND int_bind_in(const int *val){
    return int_bind_in_ex(val, NULL);
}

MYSQL_BIND int_bind_out_ex(int *val, char *is_null){
    return (MYSQL_BIND){
        .buffer_type = MYSQL_TYPE_LONG,
        .buffer = val,
        .buffer_length = sizeof(*val),
        .is_unsigned = false,
        .is_null = is_null,
    };
}
MYSQL_BIND int_bind_out(int *val){
    return int_bind_out_ex(val, NULL);
}


MYSQL_BIND uint64_bind_in_ex(const uint64_t *val, char *is_null){
    return (MYSQL_BIND){
        .buffer_type = MYSQL_TYPE_LONGLONG,
        .buffer = val,
        .is_unsigned = true,
        .is_null = is_null,
    };
}
MYSQL_BIND uint64_bind_in(const uint64_t *val){
    return uint64_bind_in_ex(val, NULL);
}

MYSQL_BIND uint64_bind_out_ex(uint64_t *val, char *is_null){
    return (MYSQL_BIND){
        .buffer_type = MYSQL_TYPE_LONGLONG,
        .buffer = val,
        .buffer_length = sizeof(*val),
        .is_unsigned = true,
        .is_null = is_null,
    };
}
MYSQL_BIND uint64_bind_out(uint64_t *val){
    return uint64_bind_out_ex(val, NULL);
}

MYSQL_BIND int64_bind_in_ex(const int64_t *val, char *is_null){
    return (MYSQL_BIND){
        .buffer_type = MYSQL_TYPE_LONGLONG,
        .buffer = val,
        .is_unsigned = false,
        .is_null = is_null,
    };
}
MYSQL_BIND int64_bind_in(const int64_t *val){
    return int64_bind_in_ex(val, NULL);
}

MYSQL_BIND int64_bind_out_ex(int64_t *val, char *is_null){
    return (MYSQL_BIND){
        .buffer_type = MYSQL_TYPE_LONGLONG,
        .buffer = val,
        .buffer_length = sizeof(*val),
        .is_unsigned = false,
        .is_null = is_null,
    };
}
MYSQL_BIND int64_bind_out(int64_t *val){
    return int64_bind_out_ex(val, NULL);
}
