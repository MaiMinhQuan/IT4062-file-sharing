#ifndef DB_H
#define DB_H

#include <mysql/mysql.h>

extern MYSQL *conn;

void init_mysql();
void close_mysql();

#endif
