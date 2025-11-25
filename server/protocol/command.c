#include "command.h"
#include "../net/stream.h"
#include "../net/client.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>     // for strcasecmp
#include "../auth/auth.h"
#include "../auth/token.h"
#include "../database/db.h"
#include <mysql/mysql.h>

#define BUFFER_SIZE 4096
#define MAX_CONCURRENT_UPLOADS 3

// Rate limiting: Track active uploads globally
static int active_uploads = 0;

// Hàm tách token an toàn
static char *next_token(char **ptr) {
    char *tok = strtok(*ptr, " \r\n");
    *ptr = NULL;  // để gọi tiếp strtok(NULL, ...)
    return tok;
}

void process_command(int idx, const char *line, int line_len) {
    char buffer[BUFFER_SIZE];
    int copy_len = (line_len < BUFFER_SIZE) ? line_len : (BUFFER_SIZE - 1);

    memcpy(buffer, line, copy_len);
    buffer[copy_len] = '\0';

    // Tạo log an toàn - ẩn password
    char safe_log[BUFFER_SIZE];
    strncpy(safe_log, buffer, sizeof(safe_log) - 1);
    safe_log[sizeof(safe_log) - 1] = '\0';
    
    // Nếu là LOGIN hoặc REGISTER, ẩn password
    if (strncasecmp(safe_log, "LOGIN ", 6) == 0 || strncasecmp(safe_log, "REGISTER ", 9) == 0) {
        char *first_space = strchr(safe_log, ' ');
        if (first_space) {
            char *second_space = strchr(first_space + 1, ' ');
            if (second_space) {
                // Thay password bằng ***
                snprintf(second_space + 1, safe_log + sizeof(safe_log) - (second_space + 1), "***");
            }
        }
    }
    
    printf("Processing command from idx %d: %s\n", idx, safe_log);

    char response[BUFFER_SIZE];

    // ============================
    // 1️⃣ Parse command
    // ============================
    char *ptr = buffer;
    char *cmd = next_token(&ptr);

    if (!cmd) {
        snprintf(response, sizeof(response), "ERR EMPTY_COMMAND\r\n");
        enqueue_send(idx, response, strlen(response));
        return;
    }

    // ============================
    // 2️⃣ REGISTER username password
    // ============================
    if (strcasecmp(cmd, "REGISTER") == 0) {
        char *username = next_token(&ptr);
        char *password = next_token(&ptr);

        if (!username || !password) {
            snprintf(response, sizeof(response), "400\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        char resp[256];
        handle_register(username, password, resp, sizeof(resp));

        snprintf(response, sizeof(response), "%s\r\n", resp);
        enqueue_send(idx, response, strlen(response));
        return;
    }

    // ============================
    // 3️⃣ LOGIN username password
    // ============================
    if (strcasecmp(cmd, "LOGIN") == 0) {
        char *username = next_token(&ptr);
        char *password = next_token(&ptr);

        if (!username || !password) {
            snprintf(response, sizeof(response), "400\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        char resp[256];
        handle_login(username, password, resp, sizeof(resp));

        snprintf(response, sizeof(response), "%s\r\n", resp);
        enqueue_send(idx, response, strlen(response));
        return;
    }

    // ============================
    // 4️⃣ VERIFY_TOKEN token
    // ============================
    if (strcasecmp(cmd, "VERIFY_TOKEN") == 0) {
        char *token = next_token(&ptr);

        if (!token) {
            snprintf(response, sizeof(response), "400\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        // Verify token
        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        
        if (user_id > 0) {
            snprintf(response, sizeof(response), "200\r\n");  // Token hợp lệ
        } else {
            snprintf(response, sizeof(response), "401\r\n");  // Token không hợp lệ hoặc hết hạn
        }
        
        enqueue_send(idx, response, strlen(response));
        return;
    }

    // ============================
    // 5️⃣ LOGOUT token
    // ============================
    if (strcasecmp(cmd, "LOGOUT") == 0) {
        char *token = next_token(&ptr);

        if (!token) {
            snprintf(response, sizeof(response), "400\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        // Xóa token khỏi database
        char escaped_token[256];
        mysql_real_escape_string(conn, escaped_token, token, strlen(token));
        
        char query[512];
        snprintf(query, sizeof(query),
                 "DELETE FROM user_sessions WHERE token='%s'",
                 escaped_token);
        
        if (mysql_query(conn, query) == 0 && mysql_affected_rows(conn) > 0) {
            snprintf(response, sizeof(response), "200\r\n");
        } else {
            snprintf(response, sizeof(response), "500\r\n");
        }
        
        enqueue_send(idx, response, strlen(response));
        return;
    }

    // ============================
    // 6️⃣ Command không tồn tại
    // ============================
    snprintf(response, sizeof(response), "ERR UNKNOWN_COMMAND %s\r\n", cmd);
    enqueue_send(idx, response, strlen(response));
}
