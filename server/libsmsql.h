#ifndef SMSQL_H
#define SMSQL_H

#include "mysql_util.h"

// all the queries used by splintermail

#define SMSQL_UUID_SIZE 32


derr_t get_uuid_for_email(MYSQL *sql, dstr_t *email, dstr_t *uuid, bool *ok);

#endif // SM_SQL_H
