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

static void trim_crlf(char *str) {
    if (!str) return;
    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == '\r' || str[len - 1] == '\n')) {
        str[len - 1] = '\0';
        len--;
    }
}

static int parse_create_group_args(const char *raw_line,
                                   char *token_out, size_t token_size,
                                   char *name_out, size_t name_size,
                                   char *desc_out, size_t desc_size) {
    if (!raw_line) return 0;

    const char *args = raw_line + strlen("CREATE_GROUP");
    while (*args == ' ') args++;
    if (*args == '\0') return 0;

    const char *first_sep = strchr(args, '|');
    if (!first_sep) return 0;

    size_t token_len = first_sep - args;
    if (token_len == 0 || token_len >= token_size) return 0;
    memcpy(token_out, args, token_len);
    token_out[token_len] = '\0';

    const char *second_sep = strchr(first_sep + 1, '|');
    if (!second_sep) return 0;

    size_t name_len = second_sep - (first_sep + 1);
    if (name_len == 0 || name_len >= name_size) return 0;
    memcpy(name_out, first_sep + 1, name_len);
    name_out[name_len] = '\0';

    const char *desc_start = second_sep + 1;
    if (*desc_start == '\0') {
        desc_out[0] = '\0';
    } else {
        size_t desc_len = strlen(desc_start);
        while (desc_len > 0 &&
               (desc_start[desc_len - 1] == '\r' || desc_start[desc_len - 1] == '\n')) {
            desc_len--;
        }

        if (desc_len >= desc_size) {
            desc_len = desc_size - 1;
        }

        memcpy(desc_out, desc_start, desc_len);
        desc_out[desc_len] = '\0';
    }

    return 1;
}

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
    char raw_line[BUFFER_SIZE];
    memcpy(raw_line, buffer, copy_len);
    raw_line[copy_len] = '\0';
    trim_crlf(raw_line);

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
    // 6️⃣ CREATE_GROUP token|name|description
    // ============================
    if (strcasecmp(cmd, "CREATE_GROUP") == 0) {
        char token[TOKEN_LENGTH + 1];
        char group_name[256];
        char description[512];

        if (!parse_create_group_args(raw_line, token, sizeof(token),
                                     group_name, sizeof(group_name),
                                     description, sizeof(description))) {
            snprintf(response, sizeof(response), "400\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id < 0) {
            snprintf(response, sizeof(response), "500\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }
        if (user_id == 0) {
            snprintf(response, sizeof(response), "401\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        char escaped_name[512];
        char escaped_desc[1024];
        mysql_real_escape_string(conn, escaped_name, group_name, strlen(group_name));
        mysql_real_escape_string(conn, escaped_desc, description, strlen(description));

        char query[2048];
        snprintf(query, sizeof(query),
                 "CALL create_group('%s','%s',%d)",
                 escaped_name, escaped_desc, user_id);

        if (mysql_query(conn, query) != 0) {
            unsigned int errnum = mysql_errno(conn);
            if (errnum == 1062) {
                snprintf(response, sizeof(response), "409\r\n");
            } else {
                snprintf(response, sizeof(response), "500\r\n");
            }
            enqueue_send(idx, response, strlen(response));
            return;
        }

        int new_group_id = -1;
        do {
            MYSQL_RES *res = mysql_store_result(conn);
            if (res) {
                MYSQL_ROW row = mysql_fetch_row(res);
                if (row && row[0]) {
                    new_group_id = atoi(row[0]);
                }
                mysql_free_result(res);
            }
        } while (mysql_next_result(conn) == 0);

        if (new_group_id > 0) {
            snprintf(response, sizeof(response), "200 %d\r\n", new_group_id);
        } else {
            snprintf(response, sizeof(response), "500\r\n");
        }

        enqueue_send(idx, response, strlen(response));
        return;
    }

    // ============================
    // 7️⃣ LIST_GROUPS_JOINED token
    // ============================
    if (strcasecmp(cmd, "LIST_GROUPS_JOINED") == 0) {
        char *token = strtok(NULL, " \r\n");
        if (!token) {
            snprintf(response, sizeof(response), "400\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id < 0) {
            snprintf(response, sizeof(response), "500\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }
        if (user_id == 0) {
            snprintf(response, sizeof(response), "401\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        char query[256];
        snprintf(query, sizeof(query), "CALL get_user_groups(%d)", user_id);

        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        char groups_buffer[BUFFER_SIZE];
        groups_buffer[0] = '\0';
        size_t groups_len = 0;
        int group_count = 0;

        do {
            MYSQL_RES *res = mysql_store_result(conn);
            if (!res) {
                continue;
            }

            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res))) {
                group_count++;
                const char *group_id = row[0] ? row[0] : "";
                const char *group_name = row[1] ? row[1] : "";
                const char *description = row[2] ? row[2] : "";
                const char *role = row[3] ? row[3] : "";
                const char *created_at = row[4] ? row[4] : "";

                int written = snprintf(groups_buffer + groups_len,
                                       sizeof(groups_buffer) - groups_len,
                                       "%s|%s|%s|%s|%s\r\n",
                                       group_id, group_name, role, created_at, description);

                if (written < 0 ||
                    (size_t)written >= sizeof(groups_buffer) - groups_len) {
                    groups_len = sizeof(groups_buffer) - 1;
                    groups_buffer[groups_len] = '\0';
                    break;
                }

                groups_len += written;
            }

            mysql_free_result(res);
        } while (mysql_next_result(conn) == 0);

        snprintf(response, sizeof(response), "200 %d\r\n%s", group_count, groups_buffer);
        enqueue_send(idx, response, strlen(response));
        return;
    }

    // ============================
    // 8️⃣ REQUEST_JOIN_GROUP token group_id
    // ============================
    if (strcasecmp(cmd, "REQUEST_JOIN_GROUP") == 0) {
        char *token = next_token(&ptr);
        char *group_id_str = next_token(&ptr);

        if (!token || !group_id_str) {
            snprintf(response, sizeof(response), "400\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        // Verify token
        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id < 0) {
            snprintf(response, sizeof(response), "500\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }
        if (user_id == 0) {
            snprintf(response, sizeof(response), "401\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        int group_id = atoi(group_id_str);
        if (group_id <= 0) {
            snprintf(response, sizeof(response), "400\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        // Gọi stored procedure
        char query[512];
        snprintf(query, sizeof(query),
                 "CALL request_join_group(%d, %d, @result_code)",
                 user_id, group_id);

        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        // Xử lý kết quả từ stored procedure
        do {
            MYSQL_RES *res = mysql_store_result(conn);
            if (res) {
                mysql_free_result(res);
            }
        } while (mysql_next_result(conn) == 0);

        // Lấy result_code
        if (mysql_query(conn, "SELECT @result_code") != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        MYSQL_RES *res = mysql_store_result(conn);
        if (!res) {
            snprintf(response, sizeof(response), "500\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        MYSQL_ROW row = mysql_fetch_row(res);
        int result_code = 500;
        if (row && row[0]) {
            result_code = atoi(row[0]);
        }
        mysql_free_result(res);

        // Trả về response theo mã trạng thái
        snprintf(response, sizeof(response), "%d REQUEST_JOIN_GROUP %s\r\n",
                 result_code, group_id_str);
        enqueue_send(idx, response, strlen(response));
        return;
    }

    // ============================
    // 9️⃣ CHECK_ADMIN token group_id
    // ============================
    if (strcasecmp(cmd, "CHECK_ADMIN") == 0) {
        char *token = next_token(&ptr);
        char *group_id_str = next_token(&ptr);

        if (!token || !group_id_str) {
            snprintf(response, sizeof(response), "400\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        // Verify token
        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id < 0) {
            snprintf(response, sizeof(response), "500\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }
        if (user_id == 0) {
            snprintf(response, sizeof(response), "401\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        int group_id = atoi(group_id_str);
        if (group_id <= 0) {
            snprintf(response, sizeof(response), "400\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        // Gọi stored procedure
        char query[512];
        snprintf(query, sizeof(query),
                 "CALL check_admin(%d, %d, @result_code)",
                 user_id, group_id);

        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        // Xử lý kết quả từ stored procedure
        do {
            MYSQL_RES *res = mysql_store_result(conn);
            if (res) {
                mysql_free_result(res);
            }
        } while (mysql_next_result(conn) == 0);

        // Lấy result_code
        if (mysql_query(conn, "SELECT @result_code") != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        MYSQL_RES *res2 = mysql_store_result(conn);
        if (!res2) {
            snprintf(response, sizeof(response), "500\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        MYSQL_ROW row2 = mysql_fetch_row(res2);
        int result_code2 = 500;
        if (row2 && row2[0]) {
            result_code2 = atoi(row2[0]);
        }
        mysql_free_result(res2);

        // Trả về response theo mã trạng thái
        snprintf(response, sizeof(response), "%d CHECK_ADMIN %s\r\n",
                 result_code2, group_id_str);
        enqueue_send(idx, response, strlen(response));
        return;
    }

    // ============================
    // 🔟 HANDLE_JOIN_REQUEST token request_id option
    // ============================
    if (strcasecmp(cmd, "HANDLE_JOIN_REQUEST") == 0) {
        char *token = next_token(&ptr);
        char *request_id_str = next_token(&ptr);
        char *option = next_token(&ptr);

        if (!token || !request_id_str || !option) {
            snprintf(response, sizeof(response), "400\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        // Verify token
        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id < 0) {
            snprintf(response, sizeof(response), "500\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }
        if (user_id == 0) {
            snprintf(response, sizeof(response), "401\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        int request_id = atoi(request_id_str);
        if (request_id <= 0) {
            snprintf(response, sizeof(response), "400\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        // Kiểm tra option hợp lệ
        if (strcasecmp(option, "accepted") != 0 && strcasecmp(option, "rejected") != 0) {
            snprintf(response, sizeof(response), "400\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        // Escape option để tránh SQL injection
        char escaped_option[32];
        mysql_real_escape_string(conn, escaped_option, option, strlen(option));

        // Gọi stored procedure
        char query[512];
        snprintf(query, sizeof(query),
                 "CALL handle_join_request(%d, %d, '%s', @result_code)",
                 user_id, request_id, escaped_option);

        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        // Xử lý kết quả từ stored procedure
        do {
            MYSQL_RES *res = mysql_store_result(conn);
            if (res) {
                mysql_free_result(res);
            }
        } while (mysql_next_result(conn) == 0);

        // Lấy result_code
        if (mysql_query(conn, "SELECT @result_code") != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        MYSQL_RES *res3 = mysql_store_result(conn);
        if (!res3) {
            snprintf(response, sizeof(response), "500\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        MYSQL_ROW row3 = mysql_fetch_row(res3);
        int result_code3 = 500;
        if (row3 && row3[0]) {
            result_code3 = atoi(row3[0]);
        }
        mysql_free_result(res3);

        // Trả về response theo mã trạng thái
        snprintf(response, sizeof(response), "%d HANDLE_JOIN_REQUEST %s\r\n",
                 result_code3, request_id_str);
        enqueue_send(idx, response, strlen(response));
        return;
    }

    // ============================
    // 🔟 LIST_GROUPS_NOT_JOINED token
    // ============================
    if (strcasecmp(cmd, "LIST_GROUPS_NOT_JOINED") == 0) {
        char *token = strtok(NULL, " \r\n");
        if (!token) {
            snprintf(response, sizeof(response), "400\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id < 0) {
            snprintf(response, sizeof(response), "500\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }
        if (user_id == 0) {
            snprintf(response, sizeof(response), "401\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        char query[256];
        snprintf(query, sizeof(query), "CALL get_groups_not_joined(%d)", user_id);

        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        char groups_buffer[BUFFER_SIZE];
        groups_buffer[0] = '\0';
        size_t groups_len = 0;
        int group_count = 0;

        do {
            MYSQL_RES *res = mysql_store_result(conn);
            if (!res) {
                continue;
            }

            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res))) {
                group_count++;
                const char *group_id = row[0] ? row[0] : "";
                const char *group_name = row[1] ? row[1] : "";
                const char *description = row[2] ? row[2] : "";
                const char *admin_name = row[3] ? row[3] : "";
                const char *created_at = row[4] ? row[4] : "";

                int written = snprintf(groups_buffer + groups_len,
                                       sizeof(groups_buffer) - groups_len,
                                       "%s|%s|%s|%s|%s\r\n",
                                       group_id, group_name, description, admin_name, created_at);

                if (written < 0 ||
                    (size_t)written >= sizeof(groups_buffer) - groups_len) {
                    groups_len = sizeof(groups_buffer) - 1;
                    groups_buffer[groups_len] = '\0';
                    break;
                }

                groups_len += written;
            }

            mysql_free_result(res);
        } while (mysql_next_result(conn) == 0);

        snprintf(response, sizeof(response), "200 %d\r\n%s", group_count, groups_buffer);
        enqueue_send(idx, response, strlen(response));
        return;
    }

    // ⓫ GET_PENDING_REQUESTS token
    // ============================
    if (strcasecmp(cmd, "GET_PENDING_REQUESTS") == 0) {
        char *token = strtok(NULL, " \r\n");
        if (!token) {
            snprintf(response, sizeof(response), "400\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id < 0) {
            snprintf(response, sizeof(response), "500\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }
        if (user_id == 0) {
            snprintf(response, sizeof(response), "401\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        char query[256];
        snprintf(query, sizeof(query), "CALL get_pending_requests_for_admin(%d)", user_id);

        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            enqueue_send(idx, response, strlen(response));
            return;
        }

        char requests_buffer[BUFFER_SIZE];
        requests_buffer[0] = '\0';
        size_t requests_len = 0;
        int request_count = 0;

        do {
            MYSQL_RES *res = mysql_store_result(conn);
            if (!res) {
                continue;
            }

            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res))) {
                request_count++;
                const char *request_id = row[0] ? row[0] : "";
                const char *user_id_str = row[1] ? row[1] : "";
                const char *username = row[2] ? row[2] : "";
                const char *group_id = row[3] ? row[3] : "";
                const char *group_name = row[4] ? row[4] : "";
                const char *created_at = row[5] ? row[5] : "";

                int written = snprintf(requests_buffer + requests_len,
                                       sizeof(requests_buffer) - requests_len,
                                       "%s|%s|%s|%s|%s|%s\r\n",
                                       request_id, user_id_str, username, group_id, group_name, created_at);

                if (written < 0 ||
                    (size_t)written >= sizeof(requests_buffer) - requests_len) {
                    requests_len = sizeof(requests_buffer) - 1;
                    requests_buffer[requests_len] = '\0';
                    break;
                }

                requests_len += written;
            }

            mysql_free_result(res);
        } while (mysql_next_result(conn) == 0);

        snprintf(response, sizeof(response), "200 %d\r\n%s", request_count, requests_buffer);
        enqueue_send(idx, response, strlen(response));
        return;
    }

    // ⓬ Command không tồn tại
    // ============================
    snprintf(response, sizeof(response), "ERR UNKNOWN_COMMAND %s\r\n", cmd);
    enqueue_send(idx, response, strlen(response));
}
