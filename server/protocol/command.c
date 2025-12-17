#include "command.h"
#include "../net/stream.h"
#include "../net/client.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>     // for strcasecmp
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>
#include <openssl/evp.h>
#include "../auth/auth.h"
#include "../auth/token.h"
#include "../database/db.h"
#include "../utils/logger.h"
#include <mysql/mysql.h>

#define BUFFER_SIZE 4096
#define MAX_CONCURRENT_UPLOADS 3

// Rate limiting: Track active uploads globally
static int active_uploads = 0;
#define STORAGE_ROOT "./storage"
#define TMP_SUFFIX ".part"
#define MAX_FILENAME_LEN 255
#define FILE_CHUNK_SIZE 2048
#define BASE64_CHUNK_SIZE (((FILE_CHUNK_SIZE + 2) / 3) * 4 + 4)

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

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

static int ensure_directory_exists(const char *path) {
    if (mkdir(path, 0755) == 0) {
        return 0;
    }
    if (errno == EEXIST) {
        return 0;
    }
    return -1;
}

static int prepare_storage_directory(int group_id, int dir_id, char *dir_out, size_t dir_size) {
    if (!dir_out || dir_size == 0) {
        return -1;
    }

    if (ensure_directory_exists(STORAGE_ROOT) != 0) {
        return -1;
    }

    char group_dir[PATH_MAX];
    int written = snprintf(group_dir, sizeof(group_dir), "%s/group_%d", STORAGE_ROOT, group_id);
    if (written <= 0 || written >= (int)sizeof(group_dir)) {
        return -1;
    }
    if (ensure_directory_exists(group_dir) != 0) {
        return -1;
    }

    written = snprintf(dir_out, dir_size, "%s/dir_%d", group_dir, dir_id);
    if (written <= 0 || written >= (int)dir_size) {
        return -1;
    }

    if (ensure_directory_exists(dir_out) != 0) {
        return -1;
    }
    return 0;
}

static void sanitize_filename(const char *input, char *output, size_t size) {
    if (!output || size == 0) return;
    output[0] = '\0';

    if (!input) return;

    size_t out_idx = 0;
    size_t len = strlen(input);
    for (size_t i = 0; i < len && out_idx + 1 < size; ++i) {
        unsigned char c = (unsigned char)input[i];
        if (c == '/' || c == '\\') {
            c = '_';
        } else if (c == '.' && i + 1 < len && input[i + 1] == '.') {
            c = '_';
        } else if (!isalnum(c) && c != '.' && c != '_' && c != '-' && c != ' ') {
            c = '_';
        }

        if (c == ' ') {
            c = '_';
        }

        if (isprint(c)) {
            output[out_idx++] = (char)c;
        }
    }

    if (out_idx == 0) {
        strncpy(output, "upload.bin", size - 1);
        output[size - 1] = '\0';
        return;
    }

    output[out_idx] = '\0';
}

static int user_in_group(int user_id, int group_id) {
    char query[256];
    snprintf(query, sizeof(query),
             "SELECT 1 FROM user_groups WHERE user_id=%d AND group_id=%d AND is_deleted=0 LIMIT 1",
             user_id, group_id);

    if (mysql_query(conn, query) != 0) {
        return -1;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) {
        return -1;
    }

    int exists = mysql_num_rows(res) > 0;
    mysql_free_result(res);
    return exists;
}

static int dir_belongs_to_group(int dir_id, int group_id) {
    char query[256];
    snprintf(query, sizeof(query),
             "SELECT 1 FROM directories WHERE dir_id=%d AND group_id=%d AND is_deleted=0 LIMIT 1",
             dir_id, group_id);

    if (mysql_query(conn, query) != 0) {
        return -1;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) {
        return -1;
    }

    int exists = mysql_num_rows(res) > 0;
    mysql_free_result(res);
    return exists;
}

static int is_user_admin_of_group(int user_id, int group_id) {
    char query[256];
    snprintf(query, sizeof(query),
             "SELECT 1 FROM user_groups WHERE user_id=%d AND group_id=%d AND role='admin' AND is_deleted=0 LIMIT 1",
             user_id, group_id);

    if (mysql_query(conn, query) != 0) {
        return -1;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) {
        return -1;
    }

    int is_admin = mysql_num_rows(res) > 0;
    mysql_free_result(res);
    return is_admin;
}

static int file_belongs_to_group(int file_id, int group_id) {
    char query[256];
    snprintf(query, sizeof(query),
             "SELECT 1 FROM files WHERE file_id=%d AND group_id=%d AND is_deleted=0 LIMIT 1",
             file_id, group_id);

    if (mysql_query(conn, query) != 0) {
        return -1;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) {
        return -1;
    }

    int exists = mysql_num_rows(res) > 0;
    mysql_free_result(res);
    return exists;
}

// Recursive delete directory and all its contents
static int delete_directory_recursive(int dir_id) {
    char query[512];

    // Delete all files in this directory
    snprintf(query, sizeof(query),
             "UPDATE files SET is_deleted=1, deleted_at=NOW() WHERE dir_id=%d AND is_deleted=0",
             dir_id);
    if (mysql_query(conn, query) != 0) {
        return -1;
    }

    // Get all subdirectories
    snprintf(query, sizeof(query),
             "SELECT dir_id FROM directories WHERE parent_dir_id=%d AND is_deleted=0",
             dir_id);
    if (mysql_query(conn, query) != 0) {
        return -1;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) {
        return -1;
    }

    // Recursively delete subdirectories
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        int subdir_id = atoi(row[0]);
        if (delete_directory_recursive(subdir_id) < 0) {
            mysql_free_result(res);
            return -1;
        }
    }
    mysql_free_result(res);

    // Finally delete this directory itself
    snprintf(query, sizeof(query),
             "UPDATE directories SET is_deleted=1, deleted_at=NOW() WHERE dir_id=%d",
             dir_id);
    if (mysql_query(conn, query) != 0) {
        return -1;
    }

    return 0;
}

// Recursive copy directory and all its contents
static int copy_directory_recursive(int src_dir_id, int target_parent_id, int user_id) {
    char query[1024];

    // Copy the directory itself first
    snprintf(query, sizeof(query),
             "INSERT INTO directories (dir_name, parent_dir_id, group_id, created_by) "
             "SELECT dir_name, %d, group_id, %d "
             "FROM directories WHERE dir_id=%d",
             target_parent_id, user_id, src_dir_id);

    if (mysql_query(conn, query) != 0) {
        return -1;
    }

    // Get the new directory ID
    int new_dir_id = mysql_insert_id(conn);

    // Copy all files in this directory
    snprintf(query, sizeof(query),
             "INSERT INTO files (file_name, file_path, file_size, file_type, dir_id, group_id, uploaded_by) "
             "SELECT file_name, file_path, file_size, file_type, %d, group_id, %d "
             "FROM files WHERE dir_id=%d AND is_deleted=0",
             new_dir_id, user_id, src_dir_id);

    if (mysql_query(conn, query) != 0) {
        return -1;
    }

    // Get all subdirectories
    snprintf(query, sizeof(query),
             "SELECT dir_id FROM directories WHERE parent_dir_id=%d AND is_deleted=0",
             src_dir_id);

    if (mysql_query(conn, query) != 0) {
        return -1;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) {
        return -1;
    }

    // Recursively copy subdirectories
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        int subdir_id = atoi(row[0]);
        if (copy_directory_recursive(subdir_id, new_dir_id, user_id) < 0) {
            mysql_free_result(res);
            return -1;
        }
    }
    mysql_free_result(res);

    return 0;
}

static int decode_base64_chunk(const char *input, unsigned char **output, size_t *out_len) {
    if (!input || !output || !out_len) {
        return -1;
    }

    size_t in_len = strlen(input);
    if (in_len == 0) {
        *output = NULL;
        *out_len = 0;
        return 0;
    }

    size_t max_len = (in_len / 4) * 3 + 3;
    unsigned char *buffer = (unsigned char *)malloc(max_len);
    if (!buffer) {
        return -1;
    }

    int decoded_len = EVP_DecodeBlock(buffer, (const unsigned char *)input, (int)in_len);
    if (decoded_len < 0) {
        free(buffer);
        return -1;
    }

    while (in_len > 0 && input[in_len - 1] == '=') {
        decoded_len--;
        in_len--;
    }

    *output = buffer;
    *out_len = (size_t)decoded_len;
    return 0;
}

static int write_chunk_file(const char *path, const unsigned char *data, size_t len, int chunk_index) {
    if (!path) return -1;
    const char *mode = (chunk_index <= 1) ? "wb" : "ab";
    FILE *fp = fopen(path, mode);
    if (!fp) {
        return -1;
    }

    if (len > 0 && data) {
        size_t written = fwrite(data, 1, len, fp);
        if (written != len) {
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);
    return 0;
}

static long get_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    return (long)st.st_size;
}

static int insert_file_metadata(const char *file_name,
                                const char *file_path,
                                long file_size,
                                int group_id,
                                int dir_id,
                                int user_id) {
    if (!file_name || !file_path) {
        return -1;
    }

    char escaped_name[512];
    char escaped_path[1024];
    mysql_real_escape_string(conn, escaped_name, file_name, strlen(file_name));
    mysql_real_escape_string(conn, escaped_path, file_path, strlen(file_path));

    char query[2048];
    snprintf(query, sizeof(query),
             "INSERT INTO files (file_name, file_path, file_size, dir_id, group_id, uploaded_by) "
             "VALUES ('%s','%s',%ld,%d,%d,%d)",
             escaped_name, escaped_path, file_size, dir_id, group_id, user_id);

    if (mysql_query(conn, query) != 0) {
        return -1;
    }

    return 0;
}

static void send_upload_error(int idx, const char *reason) {
    if (reason) {
        log_error(idx, clients[idx].user_id, "UPLOAD_FILE: %s", reason);
    }
    const char *err = "500\r\n";
    enqueue_send(idx, err, strlen(err));
    log_send(idx, clients[idx].user_id, "500");
}

static void send_download_error(int idx, const char *reason) {
    if (reason) {
        log_error(idx, clients[idx].user_id, "DOWNLOAD_FILE: %s", reason);
    }
    const char *err = "500\r\n";
    enqueue_send(idx, err, strlen(err));
    log_send(idx, clients[idx].user_id, "500");
}

static int encode_base64_chunk(const unsigned char *input, size_t len,
                               char *output, size_t out_size) {
    if (!input && len > 0) return -1;
    if (!output || out_size == 0) return -1;

    if (len == 0) {
        if (out_size < 1) return -1;
        output[0] = '\0';
        return 0;
    }

    if (out_size < BASE64_CHUNK_SIZE) {
        // Ensure buffer large enough for max chunk
        return -1;
    }

    int encoded = EVP_EncodeBlock((unsigned char *)output, input, (int)len);
    if (encoded < 0) {
        return -1;
    }
    if (encoded >= (int)out_size) {
        return -1;
    }
    output[encoded] = '\0';
    return encoded;
}

static int fetch_file_metadata(int file_id,
                               char *name_out, size_t name_size,
                               char *path_out, size_t path_size,
                               long *size_out,
                               int *dir_id_out,
                               int *group_id_out) {
    if (!name_out || !path_out || !size_out || !dir_id_out || !group_id_out) {
        return -1;
    }

    char query[256];
    snprintf(query, sizeof(query),
             "SELECT file_name, file_path, file_size, dir_id, group_id "
             "FROM files "
             "WHERE file_id=%d AND is_deleted=0 LIMIT 1",
             file_id);

    if (mysql_query(conn, query) != 0) {
        return -1;
    }

    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) {
        return -1;
    }

    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        return 0;
    }

    const char *name = row[0] ? row[0] : "";
    const char *path = row[1] ? row[1] : "";
    const char *size_str = row[2] ? row[2] : "0";
    const char *dir_str = row[3] ? row[3] : "0";
    const char *group_str = row[4] ? row[4] : "0";

    strncpy(name_out, name, name_size - 1);
    name_out[name_size - 1] = '\0';
    strncpy(path_out, path, path_size - 1);
    path_out[path_size - 1] = '\0';

    *size_out = strtol(size_str, NULL, 10);
    *dir_id_out = atoi(dir_str);
    *group_id_out = atoi(group_str);

    mysql_free_result(res);
    return 1;
}

// Hàm tách token an toàn
static char *next_token(char **ptr) {
    char *tok = strtok(*ptr, " \r\n");
    *ptr = NULL;  // để gọi tiếp strtok(NULL, ...)
    return tok;
}
// Helper function: Send response and log
static void send_response(int idx, const char *response) {
    enqueue_send(idx, response, strlen(response));
    
    // Create a copy for logging (without \r\n)
    // For long responses, log first 2000 chars with "..." indicator
    char log_buf[2048];
    size_t len = strlen(response);
    size_t max_log = sizeof(log_buf) - 10;  // Reserve space for "..." and null
    
    if (len > max_log) {
        memcpy(log_buf, response, max_log);
        strcpy(log_buf + max_log, "...");
        len = max_log + 3;
    } else {
        memcpy(log_buf, response, len);
    }
    log_buf[len] = '\0';
    
    // Remove trailing \r\n for cleaner logging
    char *end = log_buf + len - 1;
    while (end >= log_buf && (*end == '\r' || *end == '\n')) {
        *end = '\0';
        end--;
    }
    
    log_send(idx, clients[idx].user_id, "%s", log_buf);
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

    // Skip logging for internal VERIFY_TOKEN requests
    int should_log = (strncasecmp(safe_log, "VERIFY_TOKEN ", 13) != 0);

    if (should_log) {
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
        log_recv(idx, clients[idx].user_id, "%s", safe_log);
    }

    char response[BUFFER_SIZE];
    char raw_line[BUFFER_SIZE];
    memcpy(raw_line, buffer, copy_len);
    raw_line[copy_len] = '\0';
    trim_crlf(raw_line);

    // ============================
    // Parse command
    // ============================
    char *ptr = buffer;
    char *cmd = next_token(&ptr);

    if (!cmd) {
        snprintf(response, sizeof(response), "ERR EMPTY_COMMAND\r\n");
        send_response(idx, response);
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
            send_response(idx, response);
            return;
        }

        char resp[256];
        int user_id = handle_register(username, password, resp, sizeof(resp));
        
        if (user_id > 0) {
            clients[idx].user_id = user_id;
            log_info(idx, user_id, "User registered: username=%s", username);
        }

        snprintf(response, sizeof(response), "%s\r\n", resp);
        send_response(idx, response);
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
            send_response(idx, response);
            return;
        }

        char resp[256];
        int user_id = handle_login(username, password, resp, sizeof(resp));
        
        if (user_id > 0) {
            clients[idx].user_id = user_id;
            log_info(idx, user_id, "User authenticated: username=%s", username);
        }

        snprintf(response, sizeof(response), "%s\r\n", resp);
        send_response(idx, response);
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

        // Không log VERIFY_TOKEN response vì đây là internal check
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
            send_response(idx, response);
            return;
        }
        int old_user_id = clients[idx].user_id;

        // Xóa token khỏi database
        char escaped_token[256];
        mysql_real_escape_string(conn, escaped_token, token, strlen(token));

        char query[512];
        snprintf(query, sizeof(query),
                 "DELETE FROM user_sessions WHERE token='%s'",
                 escaped_token);

        if (mysql_query(conn, query) == 0 && mysql_affected_rows(conn) > 0) {
            snprintf(response, sizeof(response), "200\r\n");
            log_info(idx, old_user_id, "User logged out");
            clients[idx].user_id = 0;  // Clear user_id
        } else {
            snprintf(response, sizeof(response), "500\r\n");
            log_error(idx, old_user_id, "Logout failed");
        }

        send_response(idx, response);
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
            send_response(idx, response);
            return;
        }

        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id < 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }
        if (user_id == 0) {
            snprintf(response, sizeof(response), "401\r\n");
            send_response(idx, response);
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
            send_response(idx, response);
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

        send_response(idx, response);
        return;
    }

    // ============================
    // 7️⃣ LIST_GROUPS_JOINED token
    // ============================
    if (strcasecmp(cmd, "LIST_GROUPS_JOINED") == 0) {
        char *token = next_token(&ptr);
        if (!token) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id < 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }
        if (user_id == 0) {
            snprintf(response, sizeof(response), "401\r\n");
            send_response(idx, response);
            return;
        }

        char query[256];
        snprintf(query, sizeof(query), "CALL get_user_groups(%d)", user_id);

        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
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
                // Stored procedure trả về: group_id, group_name, role, created_at, description
                const char *group_id = row[0] ? row[0] : "";
                const char *group_name = row[1] ? row[1] : "";
                const char *role = row[2] ? row[2] : "";
                const char *created_at = row[3] ? row[3] : "";
                const char *description = row[4] ? row[4] : "";

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
        send_response(idx, response);
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
            send_response(idx, response);
            return;
        }

        // Verify token
        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id < 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }
        if (user_id == 0) {
            snprintf(response, sizeof(response), "401\r\n");
            send_response(idx, response);
            return;
        }

        int group_id = atoi(group_id_str);
        if (group_id <= 0) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Gọi stored procedure
        char query[512];
        snprintf(query, sizeof(query),
                 "CALL request_join_group(%d, %d, @result_code)",
                 user_id, group_id);

        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
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
            send_response(idx, response);
            return;
        }

        MYSQL_RES *res = mysql_store_result(conn);
        if (!res) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        MYSQL_ROW row = mysql_fetch_row(res);
        int result_code = 500;
        if (row && row[0]) {
            result_code = atoi(row[0]);
        }
        mysql_free_result(res);

        // Trả về response theo mã trạng thái
        snprintf(response, sizeof(response), "%d\r\n",
                 result_code);
        send_response(idx, response);
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
            send_response(idx, response);
            return;
        }

        // Verify token
        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id < 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }
        if (user_id == 0) {
            snprintf(response, sizeof(response), "401\r\n");
            send_response(idx, response);
            return;
        }

        int group_id = atoi(group_id_str);
        if (group_id <= 0) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Gọi stored procedure
        char query[512];
        snprintf(query, sizeof(query),
                 "CALL check_admin(%d, %d, @result_code)",
                 user_id, group_id);

        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
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
            send_response(idx, response);
            return;
        }

        MYSQL_RES *res2 = mysql_store_result(conn);
        if (!res2) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
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
                 send_response(idx, response);
        return;
    }
    // 🔟 HANDLE_JOIN_REQUEST token request_id option
    // ============================
    if (strcasecmp(cmd, "HANDLE_JOIN_REQUEST") == 0) {
        char *token = next_token(&ptr);
        char *request_id_str = next_token(&ptr);
        char *option = next_token(&ptr);

        if (!token || !request_id_str || !option) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Verify token
        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id < 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }
        if (user_id == 0) {
            snprintf(response, sizeof(response), "401\r\n");
            send_response(idx, response);
            return;
        }

        int request_id = atoi(request_id_str);
        if (request_id <= 0) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Kiểm tra option hợp lệ
        if (strcasecmp(option, "accepted") != 0 && strcasecmp(option, "rejected") != 0) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
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
            send_response(idx, response);
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
            send_response(idx, response);
            return;
        }

        MYSQL_RES *res3 = mysql_store_result(conn);
        if (!res3) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        MYSQL_ROW row3 = mysql_fetch_row(res3);
        int result_code3 = 500;
        if (row3 && row3[0]) {
            result_code3 = atoi(row3[0]);
        }
        mysql_free_result(res3);

        // Trả về response theo mã trạng thái
        snprintf(response, sizeof(response), "%d\r\n",
                 result_code3);
        send_response(idx, response);
        return;
    }

    // ============================
    // 🔟 LIST_GROUPS_NOT_JOINED token
    // ============================
    if (strcasecmp(cmd, "LIST_GROUPS_NOT_JOINED") == 0) {
        char *token = next_token(&ptr);
        if (!token) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id < 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }
        if (user_id == 0) {
            snprintf(response, sizeof(response), "401\r\n");
            send_response(idx, response);
            return;
        }

        char query[256];
        snprintf(query, sizeof(query), "CALL get_groups_not_joined(%d)", user_id);

        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
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
        send_response(idx, response);
        return;
    }
    // ⓫ GET_PENDING_REQUESTS token
    // ============================
    if (strcasecmp(cmd, "GET_PENDING_REQUESTS") == 0) {
        char *token = next_token(&ptr);
        if (!token) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id < 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }
        if (user_id == 0) {
            snprintf(response, sizeof(response), "401\r\n");
            send_response(idx, response);
            return;
        }

        char query[256];
        snprintf(query, sizeof(query), "CALL get_pending_requests_for_admin(%d)", user_id);

        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
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
        send_response(idx, response);
        return;
    }

    // ============================
    // ⓬ INVITE_USER_TO_GROUP token group_id user_id
    // ============================
    if (strcasecmp(cmd, "INVITE_USER_TO_GROUP") == 0) {
        char *token = next_token(&ptr);
        char *group_id_str = next_token(&ptr);
        char *invited_user_id_str = next_token(&ptr);

        if (!token || !group_id_str || !invited_user_id_str) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Verify token
        char error_msg[256];
        int admin_user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (admin_user_id < 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }
        if (admin_user_id == 0) {
            snprintf(response, sizeof(response), "401\r\n");
            send_response(idx, response);
            return;
        }

        int group_id = atoi(group_id_str);
        int invited_user_id = atoi(invited_user_id_str);

        if (group_id <= 0 || invited_user_id <= 0) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Kiểm tra admin có phải là admin của nhóm không
        char check_query[512];
        snprintf(check_query, sizeof(check_query),
                 "SELECT role FROM user_groups WHERE user_id=%d AND group_id=%d AND is_deleted=0",
                 admin_user_id, group_id);

        if (mysql_query(conn, check_query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        MYSQL_RES *check_res = mysql_store_result(conn);
        if (!check_res) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        MYSQL_ROW check_row = mysql_fetch_row(check_res);
        if (!check_row) {
            // User không thuộc nhóm
            mysql_free_result(check_res);
            snprintf(response, sizeof(response), "404\r\n");
            send_response(idx, response);
            return;
        }

        const char *role = check_row[0];
        if (strcmp(role, "admin") != 0) {
            // User không phải admin
            mysql_free_result(check_res);
            snprintf(response, sizeof(response), "403\r\n");
            send_response(idx, response);
            return;
        }
        mysql_free_result(check_res);

        // Kiểm tra user được mời có tồn tại không
        snprintf(check_query, sizeof(check_query),
                 "SELECT user_id FROM users WHERE user_id=%d",
                 invited_user_id);

        if (mysql_query(conn, check_query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        check_res = mysql_store_result(conn);
        if (!check_res || mysql_num_rows(check_res) == 0) {
            if (check_res) mysql_free_result(check_res);
            snprintf(response, sizeof(response), "404\r\n"); // User không tồn tại
            send_response(idx, response);
            return;
        }
        mysql_free_result(check_res);

        // Kiểm tra user đã là thành viên chưa
        snprintf(check_query, sizeof(check_query),
                 "SELECT user_id FROM user_groups WHERE user_id=%d AND group_id=%d AND is_deleted=0",
                 invited_user_id, group_id);

        if (mysql_query(conn, check_query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        check_res = mysql_store_result(conn);
        if (check_res && mysql_num_rows(check_res) > 0) {
            // Đã là thành viên
            mysql_free_result(check_res);
            snprintf(response, sizeof(response), "409\r\n");
            send_response(idx, response);
            return;
        }
        if (check_res) mysql_free_result(check_res);

        // Kiểm tra đã gửi lời mời trước đó chưa
        snprintf(check_query, sizeof(check_query),
                 "SELECT request_id FROM group_requests "
                 "WHERE user_id=%d AND group_id=%d AND request_type='invitation' AND status='pending'",
                 invited_user_id, group_id);

        if (mysql_query(conn, check_query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        check_res = mysql_store_result(conn);
        if (check_res && mysql_num_rows(check_res) > 0) {
            // Đã gửi lời mời trước đó
            mysql_free_result(check_res);
            snprintf(response, sizeof(response), "423\r\n");
            send_response(idx, response);
            return;
        }
        if (check_res) mysql_free_result(check_res);

        // Tạo lời mời
        char insert_query[512];
        snprintf(insert_query, sizeof(insert_query),
                 "INSERT INTO group_requests (user_id, group_id, request_type, status) "
                 "VALUES (%d, %d, 'invitation', 'pending')",
                 invited_user_id, group_id);

        if (mysql_query(conn, insert_query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        // Thành công
        snprintf(response, sizeof(response), "200\r\n");
        send_response(idx, response);
        return;
    }

    // ============================
    // ⓭ GET_USER_ID_BY_USERNAME username
    // ============================
    if (strcasecmp(cmd, "GET_USER_ID_BY_USERNAME") == 0) {
        char *username = next_token(&ptr);

        if (!username) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Tìm user_id từ username
        char escaped_username[256];
        mysql_real_escape_string(conn, escaped_username, username, strlen(username));

        char query[512];
        snprintf(query, sizeof(query),
                 "SELECT user_id FROM users WHERE username='%s'",
                 escaped_username);

        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        MYSQL_RES *res = mysql_store_result(conn);
        if (!res || mysql_num_rows(res) == 0) {
            if (res) mysql_free_result(res);
            snprintf(response, sizeof(response), "404\r\n"); // Username không tồn tại
            send_response(idx, response);
            return;
        }

        MYSQL_ROW row = mysql_fetch_row(res);
        int user_id = atoi(row[0]);
        mysql_free_result(res);

        // Trả về user_id
        snprintf(response, sizeof(response), "200 %d\r\n", user_id);
        send_response(idx, response);
        return;
    }

    // ============================
    // ⓮ GET_MY_INVITATIONS token
    // ============================
    if (strcasecmp(cmd, "GET_MY_INVITATIONS") == 0) {
        char *token = next_token(&ptr);

        if (!token) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Verify token
        char error_msg[256];
        int requester_user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (requester_user_id <= 0) {
            snprintf(response, sizeof(response), "401\r\n");
            send_response(idx, response);
            return;
        }

        // Lấy danh sách lời mời (status='pending', request_type='invitation')
        char query[1024];
        snprintf(query, sizeof(query),
                 "SELECT gr.request_id, gr.group_id, g.group_name, "
                 "gr.created_at "
                 "FROM group_requests gr "
                 "JOIN `groups` g ON gr.group_id = g.group_id "
                 "WHERE gr.user_id=%d AND gr.status='pending' AND gr.request_type='invitation' "
                 "ORDER BY gr.created_at DESC",
                 requester_user_id);

        if (mysql_query(conn, query) != 0) {
            fprintf(stderr, "MySQL Error: %s\n", mysql_error(conn));
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        MYSQL_RES *res = mysql_store_result(conn);
        if (!res) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        int num_invitations = mysql_num_rows(res);

        // Build invitation list in format: [invitation_n]: group_id group_name request_id request_status
        char invitations_str[BUFFER_SIZE];
        invitations_str[0] = '\0';
        size_t inv_len = 0;
        int inv_index = 1;

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
            // row[0]=request_id, row[1]=group_id, row[2]=group_name, row[3]=created_at
            const char *request_id = row[0] ? row[0] : "";
            const char *group_id = row[1] ? row[1] : "";
            const char *group_name = row[2] ? row[2] : "";

            int written = snprintf(invitations_str + inv_len,
                                   sizeof(invitations_str) - inv_len,
                                   "[invitation_%d]: %s %s %s pending ",
                                   inv_index, group_id, group_name, request_id);

            if (written < 0 || (size_t)written >= sizeof(invitations_str) - inv_len) {
                break;
            }

            inv_len += written;
            inv_index++;
        }

        mysql_free_result(res);

        // Format: "200 [invitation_1] [invitation_2] ... <CRLF>"
        snprintf(response, sizeof(response), "200 %s\r\n", invitations_str);
        send_response(idx, response);
        return;
    }

    // ============================
    // ⓯ RESPOND_TO_INVITATION token request_id action
    // action: accept hoặc reject
    // ============================
    if (strcasecmp(cmd, "RESPOND_TO_INVITATION") == 0) {
        char *token = next_token(&ptr);
        char *request_id_str = next_token(&ptr);
        char *action = next_token(&ptr);

        if (!token || !request_id_str || !action) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        int request_id = atoi(request_id_str);
        if (request_id <= 0) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Verify token
        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id <= 0) {
            snprintf(response, sizeof(response), "401\r\n");
            send_response(idx, response);
            return;
        }

        // Kiểm tra action hợp lệ
        if (strcasecmp(action, "accept") != 0 && strcasecmp(action, "reject") != 0) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Kiểm tra request_id có tồn tại và thuộc về user này không
        char query[512];
        snprintf(query, sizeof(query),
                 "SELECT group_id, status, request_type FROM group_requests "
                 "WHERE request_id=%d AND user_id=%d",
                 request_id, user_id);

        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        MYSQL_RES *res = mysql_store_result(conn);
        if (!res || mysql_num_rows(res) == 0) {
            if (res) mysql_free_result(res);
            snprintf(response, sizeof(response), "404\r\n"); // Request không tồn tại
            send_response(idx, response);
            return;
        }

        MYSQL_ROW row = mysql_fetch_row(res);
        int group_id = atoi(row[0]);
        const char *status = row[1];
        const char *request_type = row[2];

        // Kiểm tra request_type phải là 'invitation'
        if (strcmp(request_type, "invitation") != 0) {
            mysql_free_result(res);
            snprintf(response, sizeof(response), "403\r\n"); // Không phải invitation
            send_response(idx, response);
            return;
        }

        // Kiểm tra status phải là 'pending'
        if (strcmp(status, "pending") != 0) {
            mysql_free_result(res);
            snprintf(response, sizeof(response), "409\r\n"); // Đã xử lý rồi
            send_response(idx, response);
            return;
        }

        mysql_free_result(res);

        if (strcasecmp(action, "accept") == 0) {
            // Chấp nhận lời mời: thêm vào user_groups với role='member' (revive nếu trước đó bị xóa mềm)
            snprintf(query, sizeof(query),
                     "INSERT INTO user_groups (user_id, group_id, role, is_deleted) "
                     "VALUES (%d, %d, 'member', 0) "
                     "ON DUPLICATE KEY UPDATE role='member', is_deleted=0",
                     user_id, group_id);

            if (mysql_query(conn, query) != 0) {
                // Có thể đã là member rồi
                snprintf(response, sizeof(response), "500\r\n");
                send_response(idx, response);
                return;
            }

            // Cập nhật status của request thành 'accepted'
            snprintf(query, sizeof(query),
                     "UPDATE group_requests SET status='accepted' "
                     "WHERE request_id=%d",
                     request_id);

            if (mysql_query(conn, query) != 0) {
                fprintf(stderr, "MySQL Error updating status to accepted: %s\n", mysql_error(conn));
                snprintf(response, sizeof(response), "500\r\n");
                send_response(idx, response);
                return;
            }

            snprintf(response, sizeof(response), "200\r\n"); // Đã chấp nhận
        } else {
            // Từ chối lời mời: cập nhật status thành 'rejected'
            snprintf(query, sizeof(query),
                     "UPDATE group_requests SET status='rejected' "
                     "WHERE request_id=%d",
                     request_id);

            if (mysql_query(conn, query) != 0) {
                fprintf(stderr, "MySQL Error updating status to rejected: %s\n", mysql_error(conn));
                snprintf(response, sizeof(response), "500\r\n");
                send_response(idx, response);
                return;
            }

            snprintf(response, sizeof(response), "201\r\n"); // Đã từ chối
        }

        send_response(idx, response);
        return;
    }

    // ============================
    // ⓰ DELETE_ITEM token item_id type
    // ============================
    if (strcasecmp(cmd, "DELETE_ITEM") == 0) {
        char *token = next_token(&ptr);
        char *item_id_str = next_token(&ptr);
        char *type = next_token(&ptr);

        if (!token || !item_id_str || !type) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Verify token
        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id <= 0) {
            snprintf(response, sizeof(response), "401\r\n");
            send_response(idx, response);
            return;
        }

        int item_id = atoi(item_id_str);
        if (item_id <= 0) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Validate type
        if (strcasecmp(type, "F") != 0 && strcasecmp(type, "D") != 0) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Get group_id of the item
        int group_id = -1;
        char query[512];

        if (strcasecmp(type, "F") == 0) {
            // File
            snprintf(query, sizeof(query),
                     "SELECT group_id FROM files WHERE file_id=%d AND is_deleted=0",
                     item_id);
        } else {
            // Directory
            snprintf(query, sizeof(query),
                     "SELECT group_id FROM directories WHERE dir_id=%d AND is_deleted=0",
                     item_id);
        }

        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        MYSQL_RES *res = mysql_store_result(conn);
        if (!res || mysql_num_rows(res) == 0) {
            if (res) mysql_free_result(res);
            snprintf(response, sizeof(response), "404\r\n");
            send_response(idx, response);
            return;
        }

        MYSQL_ROW row = mysql_fetch_row(res);
        group_id = atoi(row[0]);
        mysql_free_result(res);

        // Check if user is admin
        int is_admin = is_user_admin_of_group(user_id, group_id);
        if (is_admin < 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }
        if (is_admin == 0) {
            snprintf(response, sizeof(response), "403\r\n");
            send_response(idx, response);
            return;
        }

        // Soft delete the item
        if (strcasecmp(type, "F") == 0) {
            // Delete single file
            snprintf(query, sizeof(query),
                     "UPDATE files SET is_deleted=1, deleted_at=NOW() WHERE file_id=%d",
                     item_id);
            if (mysql_query(conn, query) != 0) {
                snprintf(response, sizeof(response), "500\r\n");
                send_response(idx, response);
                return;
            }
        } else {
            // Delete directory recursively (all files and subdirectories)
            if (delete_directory_recursive(item_id) < 0) {
                snprintf(response, sizeof(response), "500\r\n");
                send_response(idx, response);
                return;
            }
        }

        snprintf(response, sizeof(response), "200\r\n");
        send_response(idx, response);
        return;
    }

    // ============================
    // ⓱ RENAME_ITEM token item_id new_name type
    // ============================
    if (strcasecmp(cmd, "RENAME_ITEM") == 0) {
        char *token = next_token(&ptr);
        char *item_id_str = next_token(&ptr);
        char *new_name = next_token(&ptr);
        char *type = next_token(&ptr);

        if (!token || !item_id_str || !new_name || !type) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Verify token
        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id <= 0) {
            snprintf(response, sizeof(response), "401\r\n");
            send_response(idx, response);
            return;
        }

        int item_id = atoi(item_id_str);
        if (item_id <= 0) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Validate type
        if (strcasecmp(type, "F") != 0 && strcasecmp(type, "D") != 0) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Get group_id of the item
        int group_id = -1;
        char query[512];

        if (strcasecmp(type, "F") == 0) {
            snprintf(query, sizeof(query),
                     "SELECT group_id FROM files WHERE file_id=%d AND is_deleted=0",
                     item_id);
        } else {
            snprintf(query, sizeof(query),
                     "SELECT group_id FROM directories WHERE dir_id=%d AND is_deleted=0",
                     item_id);
        }

        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        MYSQL_RES *res = mysql_store_result(conn);
        if (!res || mysql_num_rows(res) == 0) {
            if (res) mysql_free_result(res);
            snprintf(response, sizeof(response), "404\r\n");
            send_response(idx, response);
            return;
        }

        MYSQL_ROW row = mysql_fetch_row(res);
        group_id = atoi(row[0]);
        mysql_free_result(res);

        // Check if user is admin
        int is_admin = is_user_admin_of_group(user_id, group_id);
        if (is_admin < 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }
        if (is_admin == 0) {
            snprintf(response, sizeof(response), "403\r\n");
            send_response(idx, response);
            return;
        }

        // Escape new_name
        char escaped_name[512];
        mysql_real_escape_string(conn, escaped_name, new_name, strlen(new_name));

        // Update the name
        if (strcasecmp(type, "F") == 0) {
            snprintf(query, sizeof(query),
                     "UPDATE files SET file_name='%s', updated_at=NOW() WHERE file_id=%d",
                     escaped_name, item_id);
        } else {
            snprintf(query, sizeof(query),
                     "UPDATE directories SET dir_name='%s', updated_at=NOW() WHERE dir_id=%d",
                     escaped_name, item_id);
        }

        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        snprintf(response, sizeof(response), "200\r\n");
        send_response(idx, response);
        return;
    }

    // ============================
    // ⓲ MOVE_ITEM token item_id target_dir_id type
    // ============================
    if (strcasecmp(cmd, "MOVE_ITEM") == 0) {
        char *token = next_token(&ptr);
        char *item_id_str = next_token(&ptr);
        char *target_dir_id_str = next_token(&ptr);
        char *type = next_token(&ptr);

        if (!token || !item_id_str || !target_dir_id_str || !type) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Verify token
        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id <= 0) {
            snprintf(response, sizeof(response), "401\r\n");
            send_response(idx, response);
            return;
        }

        int item_id = atoi(item_id_str);
        int target_dir_id = atoi(target_dir_id_str);

        if (item_id <= 0 || target_dir_id <= 0) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Validate type
        if (strcasecmp(type, "F") != 0 && strcasecmp(type, "D") != 0) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Get group_id of the item
        int item_group_id = -1;
        char query[512];

        if (strcasecmp(type, "F") == 0) {
            snprintf(query, sizeof(query),
                     "SELECT group_id FROM files WHERE file_id=%d AND is_deleted=0",
                     item_id);
        } else {
            snprintf(query, sizeof(query),
                     "SELECT group_id FROM directories WHERE dir_id=%d AND is_deleted=0",
                     item_id);
        }

        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        MYSQL_RES *res = mysql_store_result(conn);
        if (!res || mysql_num_rows(res) == 0) {
            if (res) mysql_free_result(res);
            snprintf(response, sizeof(response), "404\r\n");
            send_response(idx, response);
            return;
        }

        MYSQL_ROW row = mysql_fetch_row(res);
        item_group_id = atoi(row[0]);
        mysql_free_result(res);

        // Get group_id of target directory
        snprintf(query, sizeof(query),
                 "SELECT group_id FROM directories WHERE dir_id=%d AND is_deleted=0",
                 target_dir_id);

        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        res = mysql_store_result(conn);
        if (!res || mysql_num_rows(res) == 0) {
            if (res) mysql_free_result(res);
            snprintf(response, sizeof(response), "404\r\n");
            send_response(idx, response);
            return;
        }

        row = mysql_fetch_row(res);
        int target_group_id = atoi(row[0]);
        mysql_free_result(res);

        // Check if both belong to same group
        if (item_group_id != target_group_id) {
            snprintf(response, sizeof(response), "403\r\n");
            send_response(idx, response);
            return;
        }

        // Check if user is admin
        int is_admin = is_user_admin_of_group(user_id, item_group_id);
        if (is_admin < 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }
        if (is_admin == 0) {
            snprintf(response, sizeof(response), "403\r\n");
            send_response(idx, response);
            return;
        }

        // Move the item
        if (strcasecmp(type, "F") == 0) {
            snprintf(query, sizeof(query),
                     "UPDATE files SET dir_id=%d, updated_at=NOW() WHERE file_id=%d",
                     target_dir_id, item_id);
        } else {
            snprintf(query, sizeof(query),
                     "UPDATE directories SET parent_dir_id=%d, updated_at=NOW() WHERE dir_id=%d",
                     target_dir_id, item_id);
        }

        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        snprintf(response, sizeof(response), "200\r\n");
        send_response(idx, response);
        return;
    }

    // ============================
    // ⓳ COPY_ITEM token item_id target_dir_id type
    // ============================
    if (strcasecmp(cmd, "COPY_ITEM") == 0) {
        char *token = next_token(&ptr);
        char *item_id_str = next_token(&ptr);
        char *target_dir_id_str = next_token(&ptr);
        char *type = next_token(&ptr);

        if (!token || !item_id_str || !target_dir_id_str || !type) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Verify token
        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id <= 0) {
            snprintf(response, sizeof(response), "401\r\n");
            send_response(idx, response);
            return;
        }

        int item_id = atoi(item_id_str);
        int target_dir_id = atoi(target_dir_id_str);

        if (item_id <= 0 || target_dir_id <= 0) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Validate type
        if (strcasecmp(type, "F") != 0 && strcasecmp(type, "D") != 0) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Get group_id of the item
        int item_group_id = -1;
        char query[1024];

        if (strcasecmp(type, "F") == 0) {
            snprintf(query, sizeof(query),
                     "SELECT group_id FROM files WHERE file_id=%d AND is_deleted=0",
                     item_id);
        } else {
            snprintf(query, sizeof(query),
                     "SELECT group_id FROM directories WHERE dir_id=%d AND is_deleted=0",
                     item_id);
        }

        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        MYSQL_RES *res = mysql_store_result(conn);
        if (!res || mysql_num_rows(res) == 0) {
            if (res) mysql_free_result(res);
            snprintf(response, sizeof(response), "404\r\n");
            send_response(idx, response);
            return;
        }

        MYSQL_ROW row = mysql_fetch_row(res);
        item_group_id = atoi(row[0]);
        mysql_free_result(res);

        // Get group_id of target directory
        snprintf(query, sizeof(query),
                 "SELECT group_id FROM directories WHERE dir_id=%d AND is_deleted=0",
                 target_dir_id);

        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        res = mysql_store_result(conn);
        if (!res || mysql_num_rows(res) == 0) {
            if (res) mysql_free_result(res);
            snprintf(response, sizeof(response), "404\r\n");
            send_response(idx, response);
            return;
        }

        row = mysql_fetch_row(res);
        int target_group_id = atoi(row[0]);
        mysql_free_result(res);

        // Check if both belong to same group
        if (item_group_id != target_group_id) {
            snprintf(response, sizeof(response), "403\r\n");
            send_response(idx, response);
            return;
        }

        // Check if user is admin
        int is_admin = is_user_admin_of_group(user_id, item_group_id);
        if (is_admin < 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }
        if (is_admin == 0) {
            snprintf(response, sizeof(response), "403\r\n");
            send_response(idx, response);
            return;
        }

        // Copy the item
        if (strcasecmp(type, "F") == 0) {
            // Copy single file - duplicate record in database
            snprintf(query, sizeof(query),
                     "INSERT INTO files (file_name, file_path, file_size, file_type, dir_id, group_id, uploaded_by) "
                     "SELECT file_name, file_path, file_size, file_type, %d, group_id, %d "
                     "FROM files WHERE file_id=%d",
                     target_dir_id, user_id, item_id);
            if (mysql_query(conn, query) != 0) {
                snprintf(response, sizeof(response), "500\r\n");
                send_response(idx, response);
                return;
            }
        } else {
            // Copy directory recursively (all files and subdirectories)
            if (copy_directory_recursive(item_id, target_dir_id, user_id) < 0) {
                snprintf(response, sizeof(response), "500\r\n");
                send_response(idx, response);
                return;
            }
        }

        snprintf(response, sizeof(response), "200\r\n");
        send_response(idx, response);
        return;
    }

    // ============================
    // LIST_GROUP_MEMBERS token group_id
    // ============================
    if (strcasecmp(cmd, "LIST_GROUP_MEMBERS") == 0) {
        char *token = next_token(&ptr);
        char *group_id_str = next_token(&ptr);

        if (!token || !group_id_str) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        int group_id = atoi(group_id_str);
        if (group_id <= 0) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Verify token
        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id <= 0) {
            snprintf(response, sizeof(response), "401\r\n");
            send_response(idx, response);
            return;
        }

        // Check if group exists
        char query[512];
        snprintf(query, sizeof(query),
                 "SELECT group_id FROM `groups` WHERE group_id=%d", group_id);

        if (mysql_query(conn, query) != 0) {
            fprintf(stderr, "MySQL Error (check group): %s\n", mysql_error(conn));
            snprintf(response, sizeof(response), "500 LIST_GROUP_MEMBERS %d\r\n", group_id);
            send_response(idx, response);
            return;
        }

        MYSQL_RES *res = mysql_store_result(conn);
        if (!res || mysql_num_rows(res) == 0) {
            if (res) mysql_free_result(res);
            snprintf(response, sizeof(response), "404 LIST_GROUP_MEMBERS %d\r\n", group_id);
            send_response(idx, response);
            return;
        }
        mysql_free_result(res);

        // Check if user is member of the group
        int membership = user_in_group(user_id, group_id);
        if (membership != 1) {
            snprintf(response, sizeof(response), "403 LIST_GROUP_MEMBERS %d\r\n", group_id);
            send_response(idx, response);
            return;
        }

        // Get all members of the group
        snprintf(query, sizeof(query),
                 "SELECT u.user_id, u.username, ug.role "
                 "FROM user_groups ug "
                 "JOIN users u ON ug.user_id = u.user_id "
                 "WHERE ug.group_id=%d AND ug.is_deleted=0 "
                 "ORDER BY ug.role DESC, u.username ASC",
                 group_id);

        if (mysql_query(conn, query) != 0) {
            fprintf(stderr, "MySQL Error (get members): %s\n", mysql_error(conn));
            snprintf(response, sizeof(response), "500 LIST_GROUP_MEMBERS %d\r\n", group_id);
            send_response(idx, response);
            return;
        }

        res = mysql_store_result(conn);
        if (!res) {
            fprintf(stderr, "MySQL Error (store result): %s\n", mysql_error(conn));
            snprintf(response, sizeof(response), "500 LIST_GROUP_MEMBERS %d\r\n", group_id);
            send_response(idx, response);
            return;
        }

        // Build response: 200 LIST_GROUP_MEMBERS username||role<SPACE>... group_id<CRLF>
        char members_data[BUFFER_SIZE * 4] = {0};
        int first = 1;

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
            if (!first) {
                strcat(members_data, " ");
            }
            first = 0;

            char member_entry[256];
            snprintf(member_entry, sizeof(member_entry), "%s||%s",
                     row[1] ? row[1] : "?",  // username
                     row[2] ? row[2] : "?"); // role
            strcat(members_data, member_entry);
        }
        mysql_free_result(res);

        // Nếu không có member nào (không nên xảy ra vì user đã check membership)
        if (first) {
            strcpy(members_data, "");
        }

        snprintf(response, sizeof(response), "200 %s %d\r\n",
                 members_data, group_id);
        send_response(idx, response);
        return;
    }

    // ============================
    // REMOVE_MEMBER token group_id user_id
    // Only admin can remove; removal is soft delete (user_groups.is_deleted = 1)
    // Response codes:
    // 200: success
    // 403: no permission / invalid token / cannot remove admin
    // 404: group not found / user not found / target not in group
    // 500: server error
    // ============================
    if (strcasecmp(cmd, "REMOVE_MEMBER") == 0) {
        char *token = next_token(&ptr);
        char *group_id_str = next_token(&ptr);
        char *target_user_id_str = next_token(&ptr);

        if (!token || !group_id_str || !target_user_id_str) {
            snprintf(response, sizeof(response), "404\r\n");
            send_response(idx, response);
            return;
        }

        int group_id = atoi(group_id_str);
        int target_user_id = atoi(target_user_id_str);
        if (group_id <= 0 || target_user_id <= 0) {
            snprintf(response, sizeof(response), "404\r\n");
            send_response(idx, response);
            return;
        }

        // Verify token
        char error_msg[256];
        int admin_user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (admin_user_id <= 0) {
            snprintf(response, sizeof(response), "403\r\n");
            send_response(idx, response);
            return;
        }

        // Check if group exists
        char query[512];
        snprintf(query, sizeof(query),
                 "SELECT 1 FROM `groups` WHERE group_id=%d LIMIT 1",
                 group_id);
        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        MYSQL_RES *res = mysql_store_result(conn);
        if (!res) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }
        if (mysql_num_rows(res) == 0) {
            mysql_free_result(res);
            snprintf(response, sizeof(response), "404\r\n");
            send_response(idx, response);
            return;
        }
        mysql_free_result(res);

        // Check if target user exists
        snprintf(query, sizeof(query),
                 "SELECT 1 FROM users WHERE user_id=%d LIMIT 1",
                 target_user_id);
        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        res = mysql_store_result(conn);
        if (!res) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }
        if (mysql_num_rows(res) == 0) {
            mysql_free_result(res);
            snprintf(response, sizeof(response), "404\r\n");
            send_response(idx, response);
            return;
        }
        mysql_free_result(res);

        // Admin must be active admin of the group
        int is_admin = is_user_admin_of_group(admin_user_id, group_id);
        if (is_admin < 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }
        if (is_admin == 0) {
            snprintf(response, sizeof(response), "403\r\n");
            send_response(idx, response);
            return;
        }

        // Target must be an active member (not deleted)
        snprintf(query, sizeof(query),
                 "SELECT role FROM user_groups WHERE user_id=%d AND group_id=%d AND is_deleted=0",
                 target_user_id, group_id);
        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        res = mysql_store_result(conn);
        if (!res) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        MYSQL_ROW row = mysql_fetch_row(res);
        if (!row) {
            mysql_free_result(res);
            snprintf(response, sizeof(response), "404\r\n");
            send_response(idx, response);
            return;
        }

        const char *role = row[0];
        if (role && strcmp(role, "admin") == 0) {
            mysql_free_result(res);
            // Only allow removing members, not admins
            snprintf(response, sizeof(response), "403\r\n");
            send_response(idx, response);
            return;
        }
        mysql_free_result(res);

        // Soft delete membership
        snprintf(query, sizeof(query),
                 "UPDATE user_groups SET is_deleted=1 WHERE user_id=%d AND group_id=%d AND is_deleted=0",
                 target_user_id, group_id);
        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        if ((int)mysql_affected_rows(conn) == 0) {
            snprintf(response, sizeof(response), "404\r\n");
            send_response(idx, response);
            return;
        }

        snprintf(response, sizeof(response), "200\r\n");
        send_response(idx, response);
        return;
    }

    // ============================
    // LIST_FOLDER_CONTENT token group_id dir_id
    // ============================
    if (strcasecmp(cmd, "LIST_FOLDER_CONTENT") == 0) {
        char *token = next_token(&ptr);
        char *group_id_str = next_token(&ptr);
        char *dir_id_str = next_token(&ptr);

        if (!token || !group_id_str) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        int group_id = atoi(group_id_str);
        int dir_id = dir_id_str ? atoi(dir_id_str) : 0;

        if (group_id <= 0) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Verify token
        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id <= 0) {
            snprintf(response, sizeof(response), "401\r\n");
            send_response(idx, response);
            return;
        }

        // Check if user is member of the group
        int membership = user_in_group(user_id, group_id);
        if (membership != 1) {
            snprintf(response, sizeof(response), "403\r\n");
            send_response(idx, response);
            return;
        }

        // If dir_id is 0, get root directory of the group
        if (dir_id == 0) {
            char query[512];
            snprintf(query, sizeof(query),
                     "SELECT root_dir_id FROM `groups` WHERE group_id=%d", group_id);

            if (mysql_query(conn, query) != 0) {
                fprintf(stderr, "MySQL Error: %s\n", mysql_error(conn));
                snprintf(response, sizeof(response), "500\r\n");
                send_response(idx, response);
                return;
            }

            MYSQL_RES *res = mysql_store_result(conn);
            if (!res || mysql_num_rows(res) == 0) {
                if (res) mysql_free_result(res);
                snprintf(response, sizeof(response), "404\r\n");
                send_response(idx, response);
                return;
            }

            MYSQL_ROW row = mysql_fetch_row(res);
            if (row[0]) {
                dir_id = atoi(row[0]);
            }
            mysql_free_result(res);
        }

        // Get parent_dir_id of current directory
        int parent_dir_id = 0;
        char query[1024];
        snprintf(query, sizeof(query),
                 "SELECT parent_dir_id FROM directories WHERE dir_id=%d", dir_id);

        if (mysql_query(conn, query) == 0) {
            MYSQL_RES *res = mysql_store_result(conn);
            if (res && mysql_num_rows(res) > 0) {
                MYSQL_ROW row = mysql_fetch_row(res);
                if (row[0]) {
                    parent_dir_id = atoi(row[0]);
                }
            }
            if (res) mysql_free_result(res);
        }

        // Build response with directories and files
        char content_data[BUFFER_SIZE * 8] = {0};

        // Get subdirectories
        snprintf(query, sizeof(query),
                 "SELECT d.dir_id, d.dir_name, 'D' as type, 0 as file_size "
                 "FROM directories d "
                 "WHERE d.parent_dir_id=%d AND d.group_id=%d AND d.is_deleted=0 "
                 "ORDER BY d.dir_name ASC",
                 dir_id, group_id);

        if (mysql_query(conn, query) != 0) {
            fprintf(stderr, "MySQL Error (get dirs): %s\n", mysql_error(conn));
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        MYSQL_RES *res = mysql_store_result(conn);
        if (!res) {
            fprintf(stderr, "MySQL Error (store dirs): %s\n", mysql_error(conn));
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        int first = 1;
        MYSQL_ROW row;

        // Format: D|dir_id|dir_name<SPACE>...
        while ((row = mysql_fetch_row(res))) {
            if (!first) strcat(content_data, " ");
            first = 0;

            char entry[512];
            snprintf(entry, sizeof(entry), "D|%s|%s",
                     row[0] ? row[0] : "?",  // dir_id
                     row[1] ? row[1] : "?"); // dir_name
            strcat(content_data, entry);
        }
        mysql_free_result(res);

        // Get files in current directory
        snprintf(query, sizeof(query),
                 "SELECT f.file_id, f.file_name, f.file_size "
                 "FROM files f "
                 "WHERE f.dir_id=%d AND f.group_id=%d AND f.is_deleted=0 "
                 "ORDER BY f.file_name ASC",
                 dir_id, group_id);

        if (mysql_query(conn, query) != 0) {
            fprintf(stderr, "MySQL Error (get files): %s\n", mysql_error(conn));
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        res = mysql_store_result(conn);
        if (!res) {
            fprintf(stderr, "MySQL Error (store files): %s\n", mysql_error(conn));
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        // Format: F|file_id|file_name|file_size<SPACE>...
        while ((row = mysql_fetch_row(res))) {
            if (!first) strcat(content_data, " ");
            first = 0;

            char entry[512];
            snprintf(entry, sizeof(entry), "F|%s|%s|%s",
                     row[0] ? row[0] : "?",  // file_id
                     row[1] ? row[1] : "?",  // file_name
                     row[2] ? row[2] : "0"); // file_size
            strcat(content_data, entry);
        }
        mysql_free_result(res);

        // Response: "200 current_dir_id parent_dir_id D|dir_id|dir_name<SPACE>... F|file_id|file_name|file_size<SPACE>...<CRLF>"
        snprintf(response, sizeof(response), "200 %d %d%s%s\r\n",
                 dir_id,
                 parent_dir_id,
                 strlen(content_data) > 0 ? " " : "",
                 content_data);

        send_response(idx, response);
        return;
    }

    // ============================
    // CREATE_FOLDER token group_id parent_dir_id folder_name
    // ============================
    if (strcasecmp(cmd, "CREATE_FOLDER") == 0) {
        char *token = next_token(&ptr);
        char *group_id_str = next_token(&ptr);
        char *parent_dir_id_str = next_token(&ptr);
        char *folder_name = next_token(&ptr);

        if (!token || !group_id_str || !parent_dir_id_str || !folder_name) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        int group_id = atoi(group_id_str);
        int parent_dir_id = atoi(parent_dir_id_str);

        if (group_id <= 0 || parent_dir_id < 0) {
            snprintf(response, sizeof(response), "400\r\n");
            send_response(idx, response);
            return;
        }

        // Verify token
        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id <= 0) {
            snprintf(response, sizeof(response), "401\r\n");
            send_response(idx, response);
            return;
        }

        // Check if user is member of the group
        int membership = user_in_group(user_id, group_id);
        if (membership != 1) {
            snprintf(response, sizeof(response), "403\r\n");
            send_response(idx, response);
            return;
        }

        // If parent_dir_id is 0, get root directory
        if (parent_dir_id == 0) {
            char query[512];
            snprintf(query, sizeof(query),
                     "SELECT root_dir_id FROM `groups` WHERE group_id=%d", group_id);

            if (mysql_query(conn, query) != 0) {
                snprintf(response, sizeof(response), "500\r\n");
                send_response(idx, response);
                return;
            }

            MYSQL_RES *res = mysql_store_result(conn);
            if (!res || mysql_num_rows(res) == 0) {
                if (res) mysql_free_result(res);
                snprintf(response, sizeof(response), "404\r\n");
                send_response(idx, response);
                return;
            }

            MYSQL_ROW row = mysql_fetch_row(res);
            if (row[0]) {
                parent_dir_id = atoi(row[0]);
            }
            mysql_free_result(res);
        }

        // Validate parent directory belongs to group
        int dir_valid = dir_belongs_to_group(parent_dir_id, group_id);
        if (dir_valid != 1) {
            snprintf(response, sizeof(response), "404\r\n");
            send_response(idx, response);
            return;
        }

        // Check if folder name already exists in parent directory
        char query[1024];
        snprintf(query, sizeof(query),
                 "SELECT 1 FROM directories WHERE parent_dir_id=%d AND dir_name='%s' AND is_deleted=0 LIMIT 1",
                 parent_dir_id, folder_name);

        if (mysql_query(conn, query) != 0) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        MYSQL_RES *res = mysql_store_result(conn);
        if (!res) {
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        if (mysql_num_rows(res) > 0) {
            mysql_free_result(res);
            snprintf(response, sizeof(response), "409\r\n"); // Conflict
            send_response(idx, response);
            return;
        }
        mysql_free_result(res);

        // Create new folder
        snprintf(query, sizeof(query),
                 "INSERT INTO directories (dir_name, parent_dir_id, group_id, created_by) "
                 "VALUES ('%s', %d, %d, %d)",
                 folder_name, parent_dir_id, group_id, user_id);

        if (mysql_query(conn, query) != 0) {
            fprintf(stderr, "MySQL Error (create folder): %s\n", mysql_error(conn));
            snprintf(response, sizeof(response), "500\r\n");
            send_response(idx, response);
            return;
        }

        int new_dir_id = (int)mysql_insert_id(conn);

        // Log activity
        snprintf(query, sizeof(query),
                 "INSERT INTO activity_log (user_id, description, group_id) "
                 "VALUES (%d, 'create_directory', %d)",
                 user_id, group_id);
        mysql_query(conn, query);

        snprintf(response, sizeof(response), "200\r\n");
        send_response(idx, response);
        printf("[CREATE_FOLDER] Created folder '%s' with ID %d in parent_dir_id=%d\n",
               folder_name, new_dir_id, parent_dir_id);
        return;
    }

    // ============================
    // 8️⃣ UPLOAD_FILE token group_id dir_id file_name chunk_idx total_chunks payload
    // ============================
    if (strcasecmp(cmd, "UPLOAD_FILE") == 0) {
        char *token = next_token(&ptr);
        char *group_id_str = next_token(&ptr);
        char *dir_id_str = next_token(&ptr);
        char *file_name_raw = next_token(&ptr);
        char *chunk_idx_str = next_token(&ptr);
        char *total_chunks_str = next_token(&ptr);
        char *base64_payload = next_token(&ptr);

        if (!token || !group_id_str || !dir_id_str || !file_name_raw ||
            !chunk_idx_str || !total_chunks_str || !base64_payload) {
            send_upload_error(idx, "Thiếu tham số upload");
            return;
        }

        int group_id = atoi(group_id_str);
        int dir_id = atoi(dir_id_str);
        int chunk_index = atoi(chunk_idx_str);
        int total_chunks = atoi(total_chunks_str);

        if (group_id <= 0 || dir_id <= 0 || chunk_index <= 0 ||
            total_chunks <= 0 || chunk_index > total_chunks) {
            send_upload_error(idx, "Tham số số học không hợp lệ");
            return;
        }

        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id <= 0) {
            send_upload_error(idx, "Token không hợp lệ");
            return;
        }

        int membership = user_in_group(user_id, group_id);
        if (membership != 1) {
            send_upload_error(idx, "User không thuộc group");
            return;
        }

        int dir_valid = dir_belongs_to_group(dir_id, group_id);
        if (dir_valid != 1) {
            send_upload_error(idx, "Thư mục không tồn tại trong group");
            return;
        }

        char safe_filename[MAX_FILENAME_LEN];
        sanitize_filename(file_name_raw, safe_filename, sizeof(safe_filename));

        char dir_path[PATH_MAX];
        if (prepare_storage_directory(group_id, dir_id, dir_path, sizeof(dir_path)) != 0) {
            send_upload_error(idx, "Không tạo được thư mục lưu trữ");
            return;
        }

        char final_path[PATH_MAX];
        char temp_path[PATH_MAX];
        int written = snprintf(final_path, sizeof(final_path), "%s/%s", dir_path, safe_filename);
        if (written <= 0 || written >= (int)sizeof(final_path)) {
            send_upload_error(idx, "Đường dẫn file quá dài");
            return;
        }
        written = snprintf(temp_path, sizeof(temp_path), "%s%s", final_path, TMP_SUFFIX);
        if (written <= 0 || written >= (int)sizeof(temp_path)) {
            send_upload_error(idx, "Đường dẫn file tạm quá dài");
            return;
        }

        unsigned char *decoded = NULL;
        size_t decoded_len = 0;
        if (decode_base64_chunk(base64_payload, &decoded, &decoded_len) != 0) {
            send_upload_error(idx, "Giải mã base64 thất bại");
            return;
        }

        if (write_chunk_file(temp_path, decoded, decoded_len, chunk_index) != 0) {
            free(decoded);
            send_upload_error(idx, "Ghi chunk xuống file tạm thất bại");
            return;
        }
        free(decoded);

        if (chunk_index == total_chunks) {
            if (rename(temp_path, final_path) != 0) {
                send_upload_error(idx, "Đổi tên file tạm thất bại");
                return;
            }

            long file_size = get_file_size(final_path);
            if (file_size < 0) {
                send_upload_error(idx, "Không đọc được kích thước file sau upload");
                return;
            }

            if (insert_file_metadata(safe_filename, final_path, file_size,
                                     group_id, dir_id, user_id) != 0) {
                send_upload_error(idx, "Ghi metadata file vào DB thất bại");
                return;
            }

            snprintf(response, sizeof(response), "200 %d/%d\r\n", chunk_index, total_chunks);
        } else {
            snprintf(response, sizeof(response), "202 %d/%d\r\n", chunk_index, total_chunks);
        }

        send_response(idx, response);
        return;
    }

    // ============================
    // 9️⃣ DOWNLOAD_FILE token file_id chunk_idx
    // ============================
    if (strcasecmp(cmd, "DOWNLOAD_FILE") == 0) {
        char *token = next_token(&ptr);
        char *file_id_str = next_token(&ptr);
        char *chunk_idx_str = next_token(&ptr);

        if (!token || !file_id_str || !chunk_idx_str) {
            send_download_error(idx, "Thiếu tham số download");
            return;
        }

        int file_id = atoi(file_id_str);
        int chunk_index = atoi(chunk_idx_str);

        if (file_id <= 0 || chunk_index <= 0) {
            send_download_error(idx, "Tham số số học không hợp lệ");
            return;
        }

        char error_msg[256];
        int user_id = verify_token(token, error_msg, sizeof(error_msg));
        if (user_id <= 0) {
            send_download_error(idx, "Token không hợp lệ");
            return;
        }

        char file_name[MAX_FILENAME_LEN];
        char file_path[PATH_MAX];
        long file_size = 0;
        int dir_id = 0;
        int group_id = 0;

        int fetch_res = fetch_file_metadata(file_id, file_name, sizeof(file_name),
                                            file_path, sizeof(file_path),
                                            &file_size, &dir_id, &group_id);
        if (fetch_res <= 0) {
            send_download_error(idx, "File không tồn tại");
            return;
        }

        int membership = user_in_group(user_id, group_id);
        if (membership != 1) {
            send_download_error(idx, "User không thuộc group");
            return;
        }

        long total_chunks = (file_size > 0)
                                ? (file_size + FILE_CHUNK_SIZE - 1) / FILE_CHUNK_SIZE
                                : 1;

        if (chunk_index > total_chunks) {
            send_download_error(idx, "Chỉ số chunk vượt quá tổng số chunk");
            return;
        }

        FILE *fp = fopen(file_path, "rb");
        if (!fp) {
            send_download_error(idx, "Mở file để đọc thất bại");
            return;
        }

        if (file_size > 0) {
            if (fseek(fp, (chunk_index - 1) * FILE_CHUNK_SIZE, SEEK_SET) != 0) {
                fclose(fp);
                send_download_error(idx, "Dịch chuyển con trỏ file thất bại");
                return;
            }
        }

        // Đọc chunk từ file
        unsigned char chunk_buffer[FILE_CHUNK_SIZE];
        size_t bytes_to_read = FILE_CHUNK_SIZE;
        if (chunk_index == total_chunks && file_size > 0) {
            // Chunk cuối cùng - đọc phần còn lại
            long remaining = file_size - (chunk_index - 1) * FILE_CHUNK_SIZE;
            if (remaining > 0 && remaining < FILE_CHUNK_SIZE) {
                bytes_to_read = (size_t)remaining;
            }
        }

        size_t bytes_read = 0;
        if (file_size > 0) {
            bytes_read = fread(chunk_buffer, 1, bytes_to_read, fp);
            if (bytes_read == 0 && ferror(fp)) {
                fclose(fp);
                send_download_error(idx, "Đọc chunk từ file thất bại");
                return;
            }
        }
        fclose(fp);

        // Encode chunk thành base64
        char base64_output[BASE64_CHUNK_SIZE];
        int encoded_len = encode_base64_chunk(chunk_buffer, bytes_read, base64_output, sizeof(base64_output));
        if (encoded_len < 0) {
            send_download_error(idx, "Mã hoá chunk thất bại");
            return;
        }

        // Gửi response: "200 chunk_idx/total_chunks file_name base64_data\r\n" hoặc "202 chunk_idx/total_chunks file_name base64_data\r\n"
        if (chunk_index == total_chunks) {
            snprintf(response, sizeof(response), "200 %d/%ld %s %s\r\n", chunk_index, total_chunks, file_name, base64_output);
        } else {
            snprintf(response, sizeof(response), "202 %d/%ld %s %s\r\n", chunk_index, total_chunks, file_name, base64_output);
        }

        send_response(idx, response);
        return;
    }

    // ============================
    // ⓴ Command không tồn tại
    // ============================
    snprintf(response, sizeof(response), "ERR UNKNOWN_COMMAND %s\r\n", cmd);
    send_response(idx, response);
}
