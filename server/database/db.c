#include <stdio.h>
#include <stdlib.h>
#include "db.h"

MYSQL *conn = NULL;

void init_mysql() {
    conn = mysql_init(NULL);

    if (conn == NULL) {
        fprintf(stderr, "mysql_init() failed\n");
        exit(1);
    }

    // Thay your_password bằng password của bạn
    if (mysql_real_connect(conn, "localhost", "root", "your_new_password",
                          "file_sharing_system", 0, NULL, 0) == NULL) {
        fprintf(stderr, "mysql_real_connect() failed: %s\n", mysql_error(conn));
        mysql_close(conn);
        exit(1);
    }

    printf("MySQL connected successfully!\n");
}

void close_mysql() {
    if (conn != NULL) {
        mysql_close(conn);
        printf("MySQL connection closed.\n");
    }
}
