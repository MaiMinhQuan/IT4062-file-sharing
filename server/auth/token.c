#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mysql/mysql.h>
#include "../database/db.h"
#include "token.h"

// Generate random alphanumeric token
void generate_token(char *token, int length) {
    const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    const int charset_size = sizeof(charset) - 1;
    
    srand(time(NULL) + rand());
    
    for (int i = 0; i < length; i++) {
        int index = rand() % charset_size;
        token[i] = charset[index];
    }
    token[length] = '\0';
}

// Save session to database
int save_session_to_db(int user_id, const char *token, char *error_msg, int error_size) {
    char query[512];
    char escaped_token[TOKEN_LENGTH * 2 + 1];
    
    mysql_real_escape_string(conn, escaped_token, token, strlen(token));
    
    // Calculate expiry time
    time_t now = time(NULL);
    time_t expires_at = now + TOKEN_EXPIRY_SECONDS;
    
    // Convert to SQL datetime format
    char expires_str[32];
    struct tm *tm_info = localtime(&expires_at);
    strftime(expires_str, sizeof(expires_str), "%Y-%m-%d %H:%M:%S", tm_info);
    
    // Insert session
    snprintf(query, sizeof(query),
             "INSERT INTO user_sessions (user_id, token, expires_at) "
             "VALUES (%d, '%s', '%s')",
             user_id, escaped_token, expires_str);
    
    if (mysql_query(conn, query) != 0) {
        snprintf(error_msg, error_size, "Database error: %s", mysql_error(conn));
        return -1;
    }
    
    return 1;
}

// Verify token and return user_id
int verify_token(const char *token, char *error_msg, int error_size) {
    char query[512];
    char escaped_token[TOKEN_LENGTH * 2 + 1];
    
    mysql_real_escape_string(conn, escaped_token, token, strlen(token));
    
    // Get current time
    time_t now = time(NULL);
    char now_str[32];
    struct tm *tm_info = localtime(&now);
    strftime(now_str, sizeof(now_str), "%Y-%m-%d %H:%M:%S", tm_info);
    
    // Check if token exists and not expired
    snprintf(query, sizeof(query),
             "SELECT user_id FROM user_sessions "
             "WHERE token='%s' AND expires_at > '%s'",
             escaped_token, now_str);
    
    if (mysql_query(conn, query) != 0) {
        snprintf(error_msg, error_size, "Database error: %s", mysql_error(conn));
        return -1;
    }
    
    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) {
        snprintf(error_msg, error_size, "Failed to get result");
        return -1;
    }
    
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        snprintf(error_msg, error_size, "Token invalid or expired");
        return 0;
    }
    
    int user_id = atoi(row[0]);
    mysql_free_result(res);
    
    return user_id;
}

// Cleanup expired sessions
void cleanup_expired_sessions() {
    char query[256];
    time_t now = time(NULL);
    char now_str[32];
    struct tm *tm_info = localtime(&now);
    strftime(now_str, sizeof(now_str), "%Y-%m-%d %H:%M:%S", tm_info);
    
    snprintf(query, sizeof(query),
             "DELETE FROM user_sessions WHERE expires_at < '%s'",
             now_str);
    
    mysql_query(conn, query);
}
