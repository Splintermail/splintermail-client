#include "mysql_util.h"

MYSQL_BIND string_bind_in(const dstr_t *dstr) {
    return (MYSQL_BIND){
        .buffer_type = MYSQL_TYPE_STRING,
        .buffer = dstr->data,
        .buffer_length = dstr->size,
        .length = &dstr->len,
    };
}

MYSQL_BIND string_bind_out(dstr_t *dstr) {
    return (MYSQL_BIND){
        .buffer_type = MYSQL_TYPE_STRING,
        .buffer = dstr->data,
        .buffer_length = dstr->size,
        .length = &dstr->len,
    };
}

MYSQL_BIND blob_bind_in(const dstr_t *dstr) {
    return (MYSQL_BIND){
        .buffer_type = MYSQL_TYPE_BLOB,
        .buffer = dstr->data,
        .buffer_length = dstr->size,
        .length = &dstr->len,
    };
}

MYSQL_BIND blob_bind_out(dstr_t *dstr) {
    return (MYSQL_BIND){
        .buffer_type = MYSQL_TYPE_BLOB,
        .buffer = dstr->data,
        .buffer_length = dstr->size,
        .length = &dstr->len,
    };
}


MYSQL_BIND bool_bind_in(const bool *val){
    return (MYSQL_BIND){
        .buffer_type = MYSQL_TYPE_TINY,
        .buffer = val,
        .is_unsigned = true,
    };
}

MYSQL_BIND bool_bind_out(bool *val){
    return (MYSQL_BIND){
        .buffer_type = MYSQL_TYPE_TINY,
        .buffer = val,
        .is_unsigned = true,
    };
}

MYSQL_BIND uint_bind_in(const unsigned int *val){
    return (MYSQL_BIND){
        /* c type of int really is MYSQL_TYPE_LONG */
        /* see: https://dev.mysql.com/doc/c-api/8.0/en/c-api-prepared-statement-type-codes.html */
        .buffer_type = MYSQL_TYPE_LONG,
        .buffer = val,
        .is_unsigned = true,
    };
}

MYSQL_BIND uint_bind_out(unsigned int *val){
    return (MYSQL_BIND){
        /* c type of int really is MYSQL_TYPE_LONG */
        /* see: https://dev.mysql.com/doc/c-api/8.0/en/c-api-prepared-statement-type-codes.html */
        .buffer_type = MYSQL_TYPE_LONG,
        .buffer = val,
        .is_unsigned = true,
    };
}