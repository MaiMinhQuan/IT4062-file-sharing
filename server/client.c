#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <termios.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#include <ctype.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 1234
#define BUFFER_SIZE 4096
#define TOKEN_LENGTH 32

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define FILE_CHUNK_SIZE 2048
#define BASE64_ENCODED_SIZE (((FILE_CHUNK_SIZE + 2) / 3) * 4 + 4)
#define UPLOAD_COMMAND_BUFFER (BASE64_ENCODED_SIZE + 512)
#define DOWNLOAD_RESPONSE_BUFFER (BASE64_ENCODED_SIZE + 512)
#define MAX_FILENAME_LEN 255

// Global token storage
char current_token[TOKEN_LENGTH + 1] = {0};

// Global socket - persistent connection
int global_sock = -1;

// Forward declarations
int connect_to_server();
void handle_create_group();
void handle_list_groups();
void handle_upload_file();
void handle_download_file();

// Kiểm tra token còn hợp lệ hay không
int is_token_valid() {
    // Nếu không có token thì chưa login
    if (strlen(current_token) == 0) {
        return 0;
    }
    
    int sock = connect_to_server();
    if (sock < 0) {
        return 0;
    }
    
    // Gửi lệnh VERIFY_TOKEN để kiểm tra
    char command[BUFFER_SIZE];
    snprintf(command, sizeof(command), "VERIFY_TOKEN %s\r\n", current_token);
    send(sock, command, strlen(command), 0);
    
    // Nhận response
    char response[BUFFER_SIZE] = {0};
    int bytes = recv(sock, response, sizeof(response) - 1, 0);
    if (bytes > 0) {
        response[bytes] = '\0';
        char *crlf = strstr(response, "\r\n");
        if (crlf) *crlf = '\0';
        
        int status_code;
        if (sscanf(response, "%d", &status_code) == 1 && status_code == 200) {
            return 1;  // Token hợp lệ
        }
    }
    
    // Token không hợp lệ hoặc hết hạn -> clear token
    memset(current_token, 0, sizeof(current_token));
    return 0;
}

// Hàm nhập password mà không hiển thị
void get_password(char *password, int size) {
    struct termios old_term, new_term;
    
    // Lưu cấu hình terminal hiện tại
    tcgetattr(STDIN_FILENO, &old_term);
    new_term = old_term;
    
    // Tắt echo
    new_term.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_term);
    
    // Nhập password
    if (fgets(password, size, stdin)) {
        // Xóa newline
        password[strcspn(password, "\n")] = 0;
    }
    
    // Khôi phục cấu hình terminal
    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
    printf("\n");
}

void print_menu() {
    int logged_in = is_token_valid();

    printf("\n========== FILE SHARING CLIENT ==========\n");
    if (logged_in) {
        printf("Trạng thái: ✓ Đã đăng nhập\n");
        printf("=========================================\n");
        printf("1. Create Group (Tạo nhóm)\n");
        printf("2. View My Groups (Xem nhóm)\n");
        printf("3. Upload File (Tải lên)\n");
        printf("4. Download File (Tải xuống)\n");
        printf("5. Logout (Đăng xuất)\n");
        printf("6. Exit (Thoát)\n");
    } else {
        printf("Trạng thái: ✗ Chưa đăng nhập\n");
        printf("=========================================\n");
        printf("1. Register (Đăng ký)\n");
        printf("2. Login (Đăng nhập)\n");
        printf("3. Exit (Thoát)\n");
    }
    printf("=========================================\n");
    printf("Chọn chức năng: ");
}

int connect_to_server() {
    // Nếu đã có kết nối, sử dụng lại
    if (global_sock > 0) {
        return global_sock;
    }
    
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        perror("Invalid address");
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        close(sock);
        return -1;
    }

    global_sock = sock;
    return sock;
}

void handle_register() {
    char username[100], password[100];
    
    printf("\n--- ĐĂNG KÝ ---\n");
    printf("Username: ");
    scanf("%s", username);
    
    // Clear input buffer
    while (getchar() != '\n');
    
    printf("Password: ");
    get_password(password, sizeof(password));

    int sock = connect_to_server();
    if (sock < 0) {
        printf("Không thể kết nối đến server!\n");
        return;
    }

    // Gửi lệnh REGISTER
    char command[BUFFER_SIZE];
    snprintf(command, sizeof(command), "REGISTER %s %s\r\n", username, password);
    send(sock, command, strlen(command), 0);

    // Nhận response
    char response[BUFFER_SIZE] = {0};
    int bytes = recv(sock, response, sizeof(response) - 1, 0);
    if (bytes > 0) {
        response[bytes] = '\0';
        // Remove trailing CRLF
        char *crlf = strstr(response, "\r\n");
        if (crlf) *crlf = '\0';
        
        printf("\nServer response: %s\n", response);
        
        // Parse response: "200 <token>" or "409" or "500"
        int status_code;
        char token[TOKEN_LENGTH + 1];
        if (sscanf(response, "%d %s", &status_code, token) == 2 && status_code == 200) {
            strncpy(current_token, token, TOKEN_LENGTH);
            current_token[TOKEN_LENGTH] = '\0';
            printf("✓ Đăng ký thành công!\n");
            printf("✓ Đã tự động đăng nhập!\n");
        } else if (status_code == 409) {
            printf("✗ Username đã tồn tại!\n");
        } else if (status_code == 500) {
            printf("✗ Lỗi server!\n");
        } else {
            printf("✗ Đăng ký thất bại!\n");
        }
    }
    
    // Không đóng socket để giữ kết nối
}

void handle_login() {
    char username[100], password[100];
    
    printf("\n--- ĐĂNG NHẬP ---\n");
    printf("Username: ");
    scanf("%s", username);
    
    // Clear input buffer
    while (getchar() != '\n');
    
    printf("Password: ");
    get_password(password, sizeof(password));

    int sock = connect_to_server();
    if (sock < 0) {
        printf("Không thể kết nối đến server!\n");
        return;
    }

    // Gửi lệnh LOGIN
    char command[BUFFER_SIZE];
    snprintf(command, sizeof(command), "LOGIN %s %s\r\n", username, password);
    send(sock, command, strlen(command), 0);

    // Nhận response
    char response[BUFFER_SIZE] = {0};
    int bytes = recv(sock, response, sizeof(response) - 1, 0);
    if (bytes > 0) {
        response[bytes] = '\0';
        // Remove trailing CRLF
        char *crlf = strstr(response, "\r\n");
        if (crlf) *crlf = '\0';
        
        // Parse response: "200 <token>" or "404" or "500"
        int status_code;
        char token[TOKEN_LENGTH + 1];
        if (sscanf(response, "%d %s", &status_code, token) == 2 && status_code == 200) {
            strncpy(current_token, token, TOKEN_LENGTH);
            current_token[TOKEN_LENGTH] = '\0';
            printf("✓ Đăng nhập thành công!\n");
        } else if (status_code == 404) {
            printf("✗ Username không tồn tại hoặc sai password!\n");
        } else if (status_code == 500) {
            printf("✗ Lỗi server!\n");
        } else {
            printf("✗ Đăng nhập thất bại!\n");
        }
    }
    
    // Không đóng socket để giữ kết nối
}

void handle_logout() {
    if (!is_token_valid()) {
        printf("Bạn chưa đăng nhập!\n");
        return;
    }
    
    printf("\n--- ĐĂNG XUẤT ---\n");
    
    int sock = connect_to_server();
    if (sock < 0) {
        printf("Không thể kết nối đến server!\n");
        return;
    }

    // Gửi lệnh LOGOUT với token
    char command[BUFFER_SIZE];
    snprintf(command, sizeof(command), "LOGOUT %s\r\n", current_token);
    send(sock, command, strlen(command), 0);

    // Nhận response
    char response[BUFFER_SIZE] = {0};
    int bytes = recv(sock, response, sizeof(response) - 1, 0);
    if (bytes > 0) {
        response[bytes] = '\0';
        char *crlf = strstr(response, "\r\n");
        if (crlf) *crlf = '\0';
        
        int status_code;
        if (sscanf(response, "%d", &status_code) == 1 && status_code == 200) {
            // Clear token
            memset(current_token, 0, sizeof(current_token));
            printf("✓ Đăng xuất thành công!\n");
        } else {
            printf("✗ Đăng xuất thất bại!\n");
        }
    }
}

void sanitize_pipe(char *str) {
    if (!str) return;
    for (size_t i = 0; str[i]; ++i) {
        if (str[i] == '|') {
            str[i] = '/';
        }
    }
}

static const char base64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_encode(const unsigned char *data, size_t len,
                         char *out, size_t out_size) {
    if (!out || out_size == 0) return -1;

    size_t encoded_len = ((len + 2) / 3) * 4;
    if (encoded_len + 1 > out_size) return -1;

    size_t i = 0;
    size_t j = 0;
    while (i + 2 < len) {
        out[j++] = base64_table[(data[i] >> 2) & 0x3F];
        out[j++] = base64_table[((data[i] & 0x3) << 4) |
                                ((data[i + 1] >> 4) & 0xF)];
        out[j++] = base64_table[((data[i + 1] & 0xF) << 2) |
                                ((data[i + 2] >> 6) & 0x3)];
        out[j++] = base64_table[data[i + 2] & 0x3F];
        i += 3;
    }

    if (i < len) {
        out[j++] = base64_table[(data[i] >> 2) & 0x3F];
        if (i + 1 == len) {
            out[j++] = base64_table[(data[i] & 0x3) << 4];
            out[j++] = '=';
            out[j++] = '=';
        } else {
            out[j++] = base64_table[((data[i] & 0x3) << 4) |
                                    ((data[i + 1] >> 4) & 0xF)];
            out[j++] = base64_table[((data[i + 1] & 0xF) << 2) & 0x3F];
            out[j++] = '=';
        }
    }

    out[j] = '\0';
    return (int)encoded_len;
}

static int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return -2; // padding
    return -1;
}

// static int base64_decode(const char *input,
//                          unsigned char *output,
//                          size_t out_size,
//                          size_t *out_len) {
//     if (!input || !output || !out_len) return -1;

//     size_t len = strlen(input);
//     if (len == 0) {
//         *out_len = 0;
//         return 0;
//     }
//     if (len % 4 != 0) {
//         return -1;
//     }

//     size_t decoded_max = (len / 4) * 3;
//     if (decoded_max > out_size) {
//         return -1;
//     }

//     size_t out_index = 0;
//     for (size_t i = 0; i < len; i += 4) {
//         int vals[4];
//         for (int j = 0; j < 4; ++j) {
//             int v = base64_value(input[i + j]);
//             if (v == -1) {
//                 return -1;
//             }
//             vals[j] = v;
//         }

//         if (input[i] == '=' || input[i + 1] == '=') {
//             return -1;
//         }

//         int pad2 = (input[i + 2] == '=');
//         int pad3 = (input[i + 3] == '=');
//         int v2 = (pad2 || vals[2] < 0) ? 0 : vals[2];
//         int v3 = (pad3 || vals[3] < 0) ? 0 : vals[3];

//         int triple = ((vals[0] & 0x3F) << 18) |
//                      ((vals[1] & 0x3F) << 12) |
//                      ((v2 & 0x3F) << 6) |
//                      (v3 & 0x3F);

//         output[out_index++] = (unsigned char)((triple >> 16) & 0xFF);

//         if (input[i + 2] != '=') {
//             if (out_index >= out_size) return -1;
//             output[out_index++] = (unsigned char)((triple >> 8) & 0xFF);
//         }
//         if (input[i + 3] != '=') {
//             if (out_index >= out_size) return -1;
//             output[out_index++] = (unsigned char)(triple & 0xFF);
//         }
//     }

//     *out_len = out_index;
//     return 0;
// }

static void extract_filename(const char *path, char *out, size_t size) {
    if (!out || size == 0) return;
    out[0] = '\0';

    if (!path) return;

    const char *last_slash = strrchr(path, '/');
#ifdef _WIN32
    const char *last_backslash = strrchr(path, '\\');
    if (!last_slash || (last_backslash && last_backslash > last_slash)) {
        last_slash = last_backslash;
    }
#endif

    const char *name = last_slash ? last_slash + 1 : path;
    if (!name || *name == '\0') {
        name = "upload.bin";
    }

    strncpy(out, name, size - 1);
    out[size - 1] = '\0';
}

static void sanitize_filename_for_command(char *filename) {
    if (!filename) return;
    sanitize_pipe(filename);
    for (size_t i = 0; filename[i]; ++i) {
        if (filename[i] == ' ' || filename[i] == '\t') {
            filename[i] = '_';
        }
    }

    if (strlen(filename) == 0) {
        strcpy(filename, "upload.bin");
    }
}

void handle_create_group() {
    if (!is_token_valid()) {
        printf("Bạn cần đăng nhập trước khi tạo nhóm!\n");
        return;
    }

    while (getchar() != '\n');

    char group_name[256];
    char description[1024];

    printf("\n--- TẠO NHÓM MỚI ---\n");
    printf("Tên nhóm: ");
    if (!fgets(group_name, sizeof(group_name), stdin)) {
        printf("Không đọc được tên nhóm.\n");
        return;
    }
    group_name[strcspn(group_name, "\n")] = 0;

    printf("Mô tả: ");
    if (!fgets(description, sizeof(description), stdin)) {
        printf("Không đọc được mô tả.\n");
        return;
    }
    description[strcspn(description, "\n")] = 0;

    if (strlen(group_name) == 0) {
        printf("Tên nhóm không được để trống.\n");
        return;
    }

    sanitize_pipe(group_name);
    sanitize_pipe(description);

    int sock = connect_to_server();
    if (sock < 0) {
        printf("Không thể kết nối đến server!\n");
        return;
    }

    char command[BUFFER_SIZE];
    snprintf(command, sizeof(command), "CREATE_GROUP %s|%s|%s\r\n",
             current_token, group_name, description);
    send(sock, command, strlen(command), 0);

    char response[BUFFER_SIZE] = {0};
    int bytes = recv(sock, response, sizeof(response) - 1, 0);
    if (bytes <= 0) {
        printf("Không nhận được phản hồi từ server.\n");
        return;
    }
    response[bytes] = '\0';

    int status_code = 0;
    int group_id = 0;
    if (sscanf(response, "%d %d", &status_code, &group_id) < 1) {
        printf("Phản hồi không hợp lệ: %s\n", response);
        return;
    }

    if(status_code == 200) {
        printf("✓ Tạo nhóm thành công! group_id = %d\n", group_id);
    }else{
        printf("✗ Lỗi server (%d).\n", status_code);
    }
}

void handle_list_groups() {
    if (!is_token_valid()) {
        printf("Bạn cần đăng nhập để xem nhóm của mình!\n");
        return;
    }

    int sock = connect_to_server();
    if (sock < 0) {
        printf("Không thể kết nối đến server!\n");
        return;
    }

    char command[BUFFER_SIZE];
    snprintf(command, sizeof(command), "LIST_GROUPS_JOINED %s\r\n", current_token);
    send(sock, command, strlen(command), 0);

    char response[BUFFER_SIZE] = {0};
    int bytes = recv(sock, response, sizeof(response) - 1, 0);
    if (bytes <= 0) {
        printf("Không nhận được phản hồi từ server.\n");
        return;
    }
    response[bytes] = '\0';

    int status_code = 0;
    int group_count = 0;
    sscanf(response, "%d %d", &status_code, &group_count);

    if(status_code == 200) {
        printf("✓ Xem nhóm thành công! group_count = %d\n", group_count);
    }else{
        printf("✗ Lỗi server (%d).\n", status_code);
        return;
    }

    printf("\nBạn đang ở trong %d nhóm:\n", group_count);
    if (group_count == 0) {
        return;
    }

    const char *table_border =
        "+------+----------------------------+--------------+---------------------+--------------------------------+\n";
    printf("%s", table_border);
    printf("| %-4s | %-28s | %-13s | %-22s | %-33s |\n",
        "ID", "Tên nhóm", "Vai trò", "Tạo ngày", "Mô tả"); 
    printf("%s", table_border);

    char *list_start = strstr(response, "\r\n");
    if (!list_start) {
        return;
    }
    list_start += 2;

    while (*list_start) {
        char *next_line = strstr(list_start, "\r\n");
        if (next_line) {
            *next_line = '\0';
        }

        if (strlen(list_start) == 0) {
            if (!next_line) break;
            list_start = next_line + 2;
            continue;
        }

        char line_copy[BUFFER_SIZE];
        strncpy(line_copy, list_start, sizeof(line_copy) - 1);
        line_copy[sizeof(line_copy) - 1] = '\0';

        char *group_id = strtok(line_copy, "|");
        char *group_name = strtok(NULL, "|");
        char *role = strtok(NULL, "|");
        char *created_at = strtok(NULL, "|");
        char *description = strtok(NULL, "|");

        const char *safe_id = group_id ? group_id : "?";
        const char *safe_name = group_name && strlen(group_name) > 0 ? group_name : "(không tên)";
        const char *safe_role = role && strlen(role) > 0 ? role : "member";
        const char *safe_created = created_at && strlen(created_at) > 0 ? created_at : "-";
        const char *safe_desc = (description && strlen(description) > 0) ? description : "(không mô tả)";

        printf("| %-4.4s | %-26.26s | %-12.12s | %-19.19s | %-30s |\n",
               safe_id, safe_name, safe_role, safe_created, safe_desc);

        if (!next_line) break;
        list_start = next_line + 2;
    }

    printf("%s", table_border);
}

void handle_upload_file() {
    if (!is_token_valid()) {
        printf("Bạn cần đăng nhập để upload file!\n");
        return;
    }

    printf("\n--- UPLOAD FILE ---\n");

    int group_id = 0;
    int dir_id = 0;

    printf("Nhập ID nhóm: ");
    if (scanf("%d", &group_id) != 1) {
        printf("ID nhóm không hợp lệ.\n");
        while (getchar() != '\n');
        return;
    }

    printf("Nhập ID thư mục: ");
    if (scanf("%d", &dir_id) != 1) {
        printf("ID thư mục không hợp lệ.\n");
        while (getchar() != '\n');
        return;
    }

    while (getchar() != '\n');

    char file_path[PATH_MAX];
    printf("Đường dẫn file: ");
    if (!fgets(file_path, sizeof(file_path), stdin)) {
        printf("Không đọc được đường dẫn.\n");
        return;
    }
    file_path[strcspn(file_path, "\n")] = '\0';

    if (strlen(file_path) == 0) {
        printf("Đường dẫn không được trống.\n");
        return;
    }

    FILE *fp = fopen(file_path, "rb");
    if (!fp) {
        printf("Không mở được file: %s\n", strerror(errno));
        return;
    }

    struct stat st;
    if (stat(file_path, &st) != 0) {
        printf("Không đọc được thông tin file: %s\n", strerror(errno));
        fclose(fp);
        return;
    }

    long long file_size = st.st_size;
    if (file_size < 0) {
        printf("Kích thước file không hợp lệ.\n");
        fclose(fp);
        return;
    }

    int total_chunks = (file_size > 0)
                           ? (int)((file_size + FILE_CHUNK_SIZE - 1) / FILE_CHUNK_SIZE)
                           : 1;

    unsigned char buffer[FILE_CHUNK_SIZE];
    char base64_buf[BASE64_ENCODED_SIZE];
    char command[UPLOAD_COMMAND_BUFFER];

    char filename[PATH_MAX];
    extract_filename(file_path, filename, sizeof(filename));
    sanitize_filename_for_command(filename);

    int sock = connect_to_server();
    if (sock < 0) {
        fclose(fp);
        return;
    }

    int success = 1;
    for (int chunk_idx = 1; chunk_idx <= total_chunks; ++chunk_idx) {
        size_t bytes_read = 0;
        if (file_size > 0) {
            bytes_read = fread(buffer, 1, FILE_CHUNK_SIZE, fp);
            if (bytes_read == 0 && ferror(fp)) {
                printf("Lỗi đọc file ở chunk %d.\n", chunk_idx);
                success = 0;
                break;
            }
        }

        int enc_len = base64_encode(buffer, bytes_read, base64_buf, sizeof(base64_buf));
        if (enc_len < 0) {
            printf("Lỗi mã hoá chunk %d.\n", chunk_idx);
            success = 0;
            break;
        }

        int cmd_len = snprintf(command, sizeof(command),
                               "UPLOAD_FILE %s %d %d %s %d %d %s\r\n",
                               current_token, group_id, dir_id, filename,
                               chunk_idx, total_chunks, base64_buf);
        if (cmd_len < 0 || cmd_len >= (int)sizeof(command)) {
            printf("Chunk %d quá lớn để gửi.\n", chunk_idx);
            success = 0;
            break;
        }

        if (send(sock, command, cmd_len, 0) < 0) {
            printf("Không gửi được chunk %d: %s\n", chunk_idx, strerror(errno));
            success = 0;
            break;
        }

        char response[256];
        int bytes = recv(sock, response, sizeof(response) - 1, 0);
        if (bytes <= 0) {
            printf("Không nhận được phản hồi cho chunk %d.\n", chunk_idx);
            success = 0;
            break;
        }
        response[bytes] = '\0';

        if (strncmp(response, "500", 3) == 0) {
            printf("Server trả lỗi 500 ở chunk %d.\n", chunk_idx);
            success = 0;
            break;
        }

        int status = 0;
        int resp_chunk = 0;
        int resp_total = 0;
        int parsed = sscanf(response, "%d %d/%d", &status, &resp_chunk, &resp_total);
        if (parsed < 1) {
            printf("Phản hồi không hợp lệ: %s\n", response);
            success = 0;
            break;
        }

        if (status == 202) {
            printf("Đã gửi chunk %d/%d.\n", resp_chunk, resp_total);
        } else if (status == 200) {
            printf("✓ Upload hoàn tất (%d/%d).\n", resp_chunk, resp_total);
        } else {
            printf("Server trả mã %d cho chunk %d.\n", status, chunk_idx);
            success = 0;
            break;
        }
    }

    fclose(fp);

    if (!success) {
        printf("✗ Upload thất bại.\n");
    }
}

static int base64_decode(const char *input, unsigned char *output, size_t out_size, size_t *out_len) {
    if (!input || !output || !out_len) return -1;
    
    size_t len = strlen(input);
    if (len == 0) {
        *out_len = 0;
        return 0;
    }
    
    size_t decoded_len = 0;
    for (size_t i = 0; i < len; i += 4) {
        if (i + 3 >= len) break;
        
        int v1 = base64_value(input[i]);
        int v2 = base64_value(input[i + 1]);
        if (v1 < 0 || v2 < 0) return -1;
        
        if (decoded_len + 1 > out_size) return -1;
        output[decoded_len++] = (unsigned char)((v1 << 2) | (v2 >> 4));
        
        if (input[i + 2] != '=') {
            int v3 = base64_value(input[i + 2]);
            if (v3 < 0) return -1;
            
            if (decoded_len + 1 > out_size) return -1;
            output[decoded_len++] = (unsigned char)(((v2 & 0x0F) << 4) | (v3 >> 2));
            
            if (input[i + 3] != '=') {
                int v4 = base64_value(input[i + 3]);
                if (v4 < 0) return -1;
                
                if (decoded_len + 1 > out_size) return -1;
                output[decoded_len++] = (unsigned char)(((v3 & 0x03) << 6) | v4);
            }
        }
    }
    
    *out_len = decoded_len;
    return 0;
}

void handle_download_file() {
    if( !is_token_valid()) {
        printf("Bạn cần đăng nhập để download file!\n");
        return;
    }

    printf("\n--- DOWNLOAD FILE ---\n");

    int file_id = 0;
    printf("Nhập ID file: ");
    if (scanf("%d", &file_id) != 1) {
        printf("ID file không hợp lệ.\n");
        while (getchar() != '\n');
        return;
    }

    while (getchar() != '\n');

    char file_path[PATH_MAX];
    printf("Đường dẫn file để lưu: ");
    if (!fgets(file_path, sizeof(file_path), stdin)) {
        printf("Không đọc được đường dẫn.\n");
        return;
    }
    file_path[strcspn(file_path, "\n")] = '\0';

    if (strlen(file_path) == 0) {
        printf("Đường dẫn không được trống.\n");
        return;
    }

    // Tạo thư mục nếu cần
    char dir_path[PATH_MAX];
    strncpy(dir_path, file_path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = '\0';
    char *last_slash = strrchr(dir_path, '/');
    if (last_slash) {
        *last_slash = '\0';
        if (strlen(dir_path) > 0) {
            char mkdir_cmd[PATH_MAX + 10];
            snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", dir_path);
            system(mkdir_cmd);
        }
    }

    FILE *fp = fopen(file_path, "wb");
    if (!fp) {
        printf("Không tạo được file: %s\n", strerror(errno));
        return;
    }

    int sock = connect_to_server();
    if (sock < 0) {
        fclose(fp);
        return;
    }

    // Bắt đầu với chunk 1 để lấy thông tin file
    int chunk_idx = 1;
    int total_chunks = 1;
    int success = 1;
    unsigned char buffer[FILE_CHUNK_SIZE];
    char response[DOWNLOAD_RESPONSE_BUFFER];

    while (chunk_idx <= total_chunks) {
        // Gửi yêu cầu chunk
        char command[256];
        int cmd_len = snprintf(command, sizeof(command),
                               "DOWNLOAD_FILE %s %d %d\r\n",
                               current_token, file_id, chunk_idx);
        if (cmd_len < 0 || cmd_len >= (int)sizeof(command)) {
            printf("Lỗi tạo lệnh cho chunk %d.\n", chunk_idx);
            success = 0;
            break;
        }
        
        if (send(sock, command, cmd_len, 0) < 0) {
            printf("Không gửi được yêu cầu chunk %d: %s\n", chunk_idx, strerror(errno));
            success = 0;
            break;
        }

        // Nhận phản hồi
        int bytes = recv(sock, response, sizeof(response) - 1, 0);
        if (bytes <= 0) {
            printf("Không nhận được phản hồi cho chunk %d.\n", chunk_idx);
            success = 0;
            break;
        }
        response[bytes] = '\0';

        // Kiểm tra lỗi
        if (strncmp(response, "500", 3) == 0) {
            printf("Server trả lỗi 500 ở chunk %d.\n", chunk_idx);
            success = 0;
            break;
        }

        // Parse response: "200 chunk_idx/total_chunks base64_data\r\n"
        // hoặc "202 chunk_idx/total_chunks base64_data\r\n"
        int status = 0;
        int resp_chunk = 0;
        int resp_total = 0;
        char *base64_data = NULL;
        
        // Tìm base64 data (sau dấu cách thứ 2)
        char *first_space = strchr(response, ' ');
        if (first_space) {
            char *second_space = strchr(first_space + 1, ' ');
            if (second_space) {
                *second_space = '\0';
                sscanf(response, "%d %d/%d", &status, &resp_chunk, &resp_total);
                base64_data = second_space + 1;
                // Loại bỏ \r\n
                char *crlf = strstr(base64_data, "\r\n");
                if (crlf) *crlf = '\0';
            }
        }

        if (!base64_data || status == 0) {
            printf("Phản hồi không hợp lệ: %s\n", response);
            success = 0;
            break;
        }

        // Cập nhật total_chunks từ response đầu tiên
        if (chunk_idx == 1) {
            total_chunks = resp_total;
        }

        // Decode base64
        size_t decoded_len = 0;
        if (base64_decode(base64_data, buffer, sizeof(buffer), &decoded_len) != 0) {
            printf("Lỗi giải mã chunk %d.\n", chunk_idx);
            success = 0;
            break;
        }

        // Ghi chunk vào file
        if (decoded_len > 0) {
            size_t written = fwrite(buffer, 1, decoded_len, fp);
            if (written != decoded_len) {
                printf("Lỗi ghi chunk %d vào file.\n", chunk_idx);
                success = 0;
                break;
            }
        }

        if (status == 200) {
            printf("✓ Download hoàn tất (%d/%d).\n", resp_chunk, resp_total);
            break;
        } else if (status == 202) {
            printf("Đã nhận chunk %d/%d.\n", resp_chunk, resp_total);
            chunk_idx++;
        } else {
            printf("Server trả mã %d cho chunk %d.\n", status, chunk_idx);
            success = 0;
            break;
        }
    }

    fclose(fp);
    if (!success) {
        printf("✗ Download thất bại.\n");
        unlink(file_path);  // Xóa file nếu download thất bại
    }
}
int main() {
    int choice;

    printf("Kết nối đến server %s:%d...\n", SERVER_IP, SERVER_PORT);

    while (1) {
        print_menu();
        
        if (scanf("%d", &choice) != 1) {
            // Clear input buffer
            while (getchar() != '\n');
            printf("Lựa chọn không hợp lệ!\n");
            continue;
        }

        if (is_token_valid()) {
            // Menu khi đã login
            switch (choice) {
                case 1:
                    handle_create_group();
                    break;
                case 2:
                    handle_list_groups();
                    break;
                case 3:
                    handle_upload_file();
                    break;
                case 4:
                    handle_download_file();
                    break;
                case 5:
                    handle_logout();
                    break;
                case 6:
                    printf("Tạm biệt!\n");
                    if (global_sock > 0) {
                        close(global_sock);
                    }
                    return 0;
                default:
                    printf("Lựa chọn không hợp lệ!\n");
            }
        } else {
            // Menu khi chưa login
            switch (choice) {
                case 1:
                    handle_register();
                    break;
                case 2:
                    handle_login();
                    break;
                case 3:
                    printf("Tạm biệt!\n");
                    if (global_sock > 0) {
                        close(global_sock);
                    }
                    return 0;
                default:
                    printf("Lựa chọn không hợp lệ!\n");
            }
        }
    }

    return 0;
}
