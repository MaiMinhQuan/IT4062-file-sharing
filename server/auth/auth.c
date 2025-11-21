#include <stdio.h>
#include <string.h>
#include <mysql/mysql.h>
#include "../database/db.h"
#include "auth.h"
#include "hash.h"
#include "token.h"

// Escape chuỗi chống SQL injection
static void escape(const char *in, char *out) {
    mysql_real_escape_string(conn, out, in, strlen(in));
}

int handle_register(const char *username, const char *password,
                    char *response, int resp_size)
{
    char u[200], p[200], query[500];
    escape(username, u);
    
    // Hash password trước khi lưu
    char password_hash[65];
    sha256_hash(password, password_hash, sizeof(password_hash));
    escape(password_hash, p);

    // Kiểm tra tồn tại
    snprintf(query, sizeof(query),
             "SELECT user_id FROM users WHERE username='%s'", u);

    if (mysql_query(conn, query) != 0) {
        snprintf(response, resp_size, "500");
        return -1;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (mysql_num_rows(res) > 0) {
        mysql_free_result(res);
        snprintf(response, resp_size, "409");
        return 0;
    }
    mysql_free_result(res);

    // Thêm user
    snprintf(query, sizeof(query),
             "INSERT INTO users (username, password) VALUES ('%s', '%s')",
             u, p);

    if (mysql_query(conn, query) != 0) {
        snprintf(response, resp_size, "500");
        return -1;
    }

    // Lấy user_id vừa tạo
    int user_id = (int)mysql_insert_id(conn);

    // Tạo token và lưu session
    char token[TOKEN_LENGTH + 1];
    generate_token(token, TOKEN_LENGTH);
    
    char error_msg[256];
    if (save_session_to_db(user_id, token, error_msg, sizeof(error_msg)) < 0) {
        snprintf(response, resp_size, "500");
        return -1;
    }

    // Trả về 200 với token
    snprintf(response, resp_size, "200 %s", token);
    return user_id;
}

int handle_login(const char *username, const char *password,
                 char *response, int resp_size)
{
    char u[200], p[200], query[500];
    escape(username, u);
    
    // Hash password để so sánh
    char password_hash[65];
    sha256_hash(password, password_hash, sizeof(password_hash));
    escape(password_hash, p);

    snprintf(query, sizeof(query),
             "SELECT user_id FROM users "
             "WHERE username='%s' AND password='%s'",
             u, p);

    if (mysql_query(conn, query) != 0) {
        snprintf(response, resp_size, "500");
        return -1;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    MYSQL_ROW row = mysql_fetch_row(res);

    if (!row) {
        mysql_free_result(res);
        snprintf(response, resp_size, "404");
        return 0;
    }

    int user_id = atoi(row[0]);
    mysql_free_result(res);

    // Tạo token và lưu session
    char token[TOKEN_LENGTH + 1];
    generate_token(token, TOKEN_LENGTH);
    
    char error_msg[256];
    if (save_session_to_db(user_id, token, error_msg, sizeof(error_msg)) < 0) {
        snprintf(response, resp_size, "500");
        return -1;
    }

    // Trả về 200 với token
    snprintf(response, resp_size, "200 %s", token);
    return user_id;
}
