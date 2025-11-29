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
#include <mysql/mysql.h>

#define BUFFER_SIZE 4096
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
             "SELECT 1 FROM user_groups WHERE user_id=%d AND group_id=%d LIMIT 1",
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
        printf("[UPLOAD_FILE][idx=%d] %s\n", idx, reason);
    }
    const char *err = "500\r\n";
    enqueue_send(idx, err, strlen(err));
}

static void send_download_error(int idx, const char *reason) {
    if (reason) {
        printf("[DOWNLOAD_FILE][idx=%d] %s\n", idx, reason);
    }
    const char *err = "500\r\n";
    enqueue_send(idx, err, strlen(err));
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
                strcpy(second_space + 1, "***");
            }
        }
    }
    
    // Log xử lý command (có thể bật lại khi cần debug)
    // printf("Processing command from idx %d: %s\n", idx, safe_log);

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
        char *username = strtok(NULL, " \r\n");
        char *password = strtok(NULL, " \r\n");

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
        char *username = strtok(NULL, " \r\n");
        char *password = strtok(NULL, " \r\n");

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
        char *token = strtok(NULL, " \r\n");

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
        char *token = strtok(NULL, " \r\n");

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
    // 8️⃣ UPLOAD_FILE token group_id dir_id file_name chunk_idx total_chunks payload
    // ============================
    if (strcasecmp(cmd, "UPLOAD_FILE") == 0) {
        char *token = strtok(NULL, " \r\n");
        char *group_id_str = strtok(NULL, " \r\n");
        char *dir_id_str = strtok(NULL, " \r\n");
        char *file_name_raw = strtok(NULL, " \r\n");
        char *chunk_idx_str = strtok(NULL, " \r\n");
        char *total_chunks_str = strtok(NULL, " \r\n");
        char *base64_payload = strtok(NULL, "\r\n");

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

        enqueue_send(idx, response, strlen(response));
        return;
    }

    // ============================
    // 9️⃣ DOWNLOAD_FILE token file_id chunk_idx
    // ============================
    if (strcasecmp(cmd, "DOWNLOAD_FILE") == 0) {
        char *token = strtok(NULL, " \r\n");
        char *file_id_str = strtok(NULL, " \r\n");
        char *chunk_idx_str = strtok(NULL, " \r\n");

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

        // Gửi response: "200 chunk_idx/total_chunks base64_data\r\n" hoặc "202 chunk_idx/total_chunks base64_data\r\n"
        if (chunk_index == total_chunks) {
            snprintf(response, sizeof(response), "200 %d/%ld %s\r\n", chunk_index, total_chunks, base64_output);
        } else {
            snprintf(response, sizeof(response), "202 %d/%ld %s\r\n", chunk_index, total_chunks, base64_output);
        }

        enqueue_send(idx, response, strlen(response));
        return;
    }

    // ============================
    // 🔟 Command không tồn tại
    // ============================
    snprintf(response, sizeof(response), "ERR UNKNOWN_COMMAND %s\r\n", cmd);
    enqueue_send(idx, response, strlen(response));
}
