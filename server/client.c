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
void handle_group_access(int group_id, const char *user_role);
void handle_invite_user(int group_id);
void handle_request_join_group();
void handle_view_pending_requests();
void handle_approve_request();
void handle_view_my_invitations();
void handle_upload_file(int group_id);
void handle_download_file(int group_id);
void handle_list_members(int group_id);
void handle_list_folder_content(int group_id, int dir_id, int is_admin);
void handle_create_folder(int group_id, int parent_dir_id);
void handle_delete_item(int group_id);
void handle_rename_item(int group_id);
void handle_move_item(int group_id);
void handle_copy_item(int group_id);

// Lấy đường dẫn file trong thư mục Downloads, xử lý trùng tên kiểu "file(1).ext"
static void build_download_path(const char *filename, char *out_path, size_t out_size) {
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') {
        home = ".";
    }

    char downloads_dir[PATH_MAX];
    snprintf(downloads_dir, sizeof(downloads_dir), "%s/Downloads", home);

    struct stat st;
    if (stat(downloads_dir, &st) == -1) {
        // Tạo thư mục Downloads nếu chưa tồn tại
        mkdir(downloads_dir, 0755);
    }

    // Tách tên file thành phần base và extension
    char base[256];
    char ext[256];
    const char *dot = strrchr(filename, '.');
    if (dot && dot != filename) {
        size_t base_len = (size_t)(dot - filename);
        if (base_len >= sizeof(base)) base_len = sizeof(base) - 1;
        memcpy(base, filename, base_len);
        base[base_len] = '\0';

        strncpy(ext, dot, sizeof(ext) - 1);
        ext[sizeof(ext) - 1] = '\0';
    } else {
        strncpy(base, filename, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
        ext[0] = '\0';
    }

    // Đường dẫn mặc định
    snprintf(out_path, out_size, "%s/%s%s", downloads_dir, base, ext);

    int counter = 1;
    while (access(out_path, F_OK) == 0 && counter < 1000) {
        snprintf(out_path, out_size, "%s/%s(%d)%s", downloads_dir, base, counter, ext);
        counter++;
    }
}

// Kiểm tra token còn hợp lệ hay không
int is_token_valid() {
    // Nếu không có token thì chưa login
    if (strlen(current_token) == 0) {
        return 0;
    }

    // Sử dụng connection hiện tại
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
    // KHÔNG đóng socket vì dùng chung global_sock

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
        printf("1. Tạo nhóm\n");
        printf("2. Xem nhóm của tôi\n");
        printf("3. Xin vào nhóm\n");
        printf("4. Phê duyệt yêu cầu tham gia nhóm (Admin)\n");
        printf("5. Xem lời mời tham gia nhóm\n");
        printf("6. Đăng xuất\n");
        printf("7. Thoát\n");
    } else {
        printf("Trạng thái: ✗ Chưa đăng nhập\n");
        printf("=========================================\n");
        printf("1. Đăng ký\n");
        printf("2. Đăng nhập\n");
        printf("3. Thoát\n");
    }
    printf("=========================================\n");
    printf("Chọn chức năng: ");
}

// Tạo kết nối mới không dùng global_sock
int create_new_connection() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(sock);
        return -1;
    }

    return sock;
}

int connect_to_server() {
    // Nếu đã có kết nối, trả về luôn
    if (global_sock >= 0) {
        return global_sock;
    }

    // Tạo kết nối mới
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

    global_sock = sock;  // Lưu vào global_sock
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

    // Connection được giữ mở trong global_sock để sử dụng cho các request tiếp theo
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

    // Connection được giữ mở trong global_sock để sử dụng cho các request tiếp theo
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

    // Đóng connection sau khi logout
    if (global_sock >= 0) {
        close(global_sock);
        global_sock = -1;
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

    if(status_code != 200) {
        printf("✗ Lỗi server (%d).\n", status_code);
        return;
    }

    printf("\n DANH SÁCH NHÓM CỦA BẠN (%d nhóm)\n\n", group_count);

    if (group_count == 0) {
        printf("  Bạn chưa tham gia nhóm nào.\n");
        printf("  Sử dụng chức năng 1 để tạo nhóm mới hoặc chức năng 3 để xin tham gia nhóm.\n");
        return;
    }

    const char *table_border =
        "┌──────┬──────────────────────────────┬──────────┬─────────────────────┬──────────────────────────────┐\n";
    const char *table_separator =
        "├──────┼──────────────────────────────┼──────────┼─────────────────────┼──────────────────────────────┤\n";
    const char *table_bottom =
        "└──────┴──────────────────────────────┴──────────┴─────────────────────┴──────────────────────────────┘\n";

    printf("%s", table_border);
    printf("│ %-4s │ %-30s  │ %-9s│ %-22s│ %-31s  │\n",
        "ID", "Tên nhóm", "Vai trò", "Ngày tạo", "Mô tả");
    printf("%s", table_separator);

    // Lưu mapping group_id -> role
    int group_ids[100];
    char roles[100][20];
    int group_index = 0;

    char *list_start = strstr(response, "\r\n");
    if (!list_start) {
        return;
    }
    list_start += 2;

    while (*list_start && group_index < 100) {
        char *next_line = strstr(list_start, "\r\n");

        // Tính độ dài của dòng hiện tại
        size_t line_len;
        if (next_line) {
            line_len = next_line - list_start;
        } else {
            line_len = strlen(list_start);
        }

        if (line_len == 0) {
            if (!next_line) break;
            list_start = next_line + 2;
            continue;
        }

        // Copy dòng để parse (không modify response buffer)
        char line_copy[BUFFER_SIZE];
        if (line_len >= sizeof(line_copy)) {
            line_len = sizeof(line_copy) - 1;
        }
        memcpy(line_copy, list_start, line_len);
        line_copy[line_len] = '\0';

        // Parse tokens
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

        // Hiển thị trong bảng
        if (strcmp(safe_role, "admin") == 0) {
            printf("│ %-4.4s │ %-28.28s │ Admin    │ %-19.19s │ %-28.28s │\n",
                   safe_id, safe_name, safe_created, safe_desc);
        } else {
            printf("│ %-4.4s │ %-28.28s │ Member   │ %-19.19s │ %-28.28s │\n",
                   safe_id, safe_name, safe_created, safe_desc);
        }

        // Lưu mapping group_id -> role
        if (group_id && role) {
            group_ids[group_index] = atoi(group_id);
            strncpy(roles[group_index], role, sizeof(roles[0]) - 1);
            roles[group_index][sizeof(roles[0]) - 1] = '\0';
            group_index++;
        }

        if (!next_line) break;
        list_start = next_line + 2;
    }

    printf("%s", table_bottom);



    // Prompt user để chọn nhóm
    printf("\nNhập ID nhóm để truy cập (hoặc 0 để quay lại): ");
    int selected_group_id;
    if (scanf("%d", &selected_group_id) != 1) {
        while (getchar() != '\n');
        printf("ID không hợp lệ!\n");
        return;
    }
    while (getchar() != '\n');

    if (selected_group_id == 0) {
        return;
    }

    // Tìm role của user trong group này
    char user_role[20] = "member";
    int role_found = 0;
    for (int i = 0; i < group_index; i++) {
        if (group_ids[i] == selected_group_id) {
            strncpy(user_role, roles[i], sizeof(user_role) - 1);
            user_role[sizeof(user_role) - 1] = '\0';
            role_found = 1;
            break;
        }
    }


    // Gọi hàm truy cập nhóm với role
    handle_group_access(selected_group_id, user_role);
}

void handle_group_access(int group_id, const char *user_role) {
    int is_admin = (strcmp(user_role, "admin") == 0);

    printf("\n┌────────────────────────────────────────────┐\n");
    printf("│   Nhóm #%-3d - Vai trò: %-12s        │\n",
           group_id,
           user_role);
    printf("└────────────────────────────────────────────┘\n");

    while (1) {
        printf("\n┌─────────────────────────────────────────┐\n");
        printf("│         QUẢN LÝ NHÓM - MENU             │\n");
        printf("├─────────────────────────────────────────┤\n");
        printf("│ 1. Quản lý File/Folder                  │\n");
        printf("│ 2. Xem thành viên nhóm                  │\n");

        if (is_admin) {
            printf("│ 3. Mời user vào nhóm (Admin)            │\n");
        }

        printf("│ 0. Quay lại                             │\n");
        printf("└─────────────────────────────────────────┘\n");
        printf("Chọn chức năng: ");

        int choice;
        if (scanf("%d", &choice) != 1) {
            while (getchar() != '\n');
            printf("Lựa chọn không hợp lệ!\n");
            continue;
        }
        while (getchar() != '\n');

        switch (choice) {
            case 1:
                handle_list_folder_content(group_id, 0, is_admin);
                break;
            case 2:
                handle_list_members(group_id);
                break;
            case 3:
                if (is_admin) {
                    handle_invite_user(group_id);
                } else {
                    printf("Lựa chọn không hợp lệ!\n");
                }
                break;
            case 0:
                printf("Quay lại menu chính...\n");
                return;
            default:
                if (is_admin) {
                    printf("Lựa chọn không hợp lệ! Vui lòng chọn từ 0-3.\n");
                } else {
                    printf("Lựa chọn không hợp lệ! Vui lòng chọn từ 0-3.\n");
                }
        }
    }
}

void handle_invite_user(int group_id) {
    printf("\n┌────────────────────────────────────────────┐\n");
    printf("│        MỜI USER VÀO NHÓM                   │\n");
    printf("└────────────────────────────────────────────┘\n");

    printf("\nNhập username của người bạn muốn mời (hoặc '0' để quay lại): ");
    char username[256];
    if (scanf("%255s", username) != 1) {
        while (getchar() != '\n');
        printf("Username không hợp lệ!\n");
        return;
    }
    while (getchar() != '\n');

    // Kiểm tra nếu nhập 0 để quay lại
    if (strcmp(username, "0") == 0) {
        printf("Quay lại menu nhóm...\n");
        return;
    }

    int sock = connect_to_server();
    if (sock < 0) {
        printf("Không thể kết nối đến server!\n");
        return;
    }

    // Bước 1: Lấy user_id từ username
    char command[BUFFER_SIZE];
    snprintf(command, sizeof(command), "GET_USER_ID_BY_USERNAME %s\r\n", username);
    send(sock, command, strlen(command), 0);

    // Nhận response
    char response[BUFFER_SIZE] = {0};
    int bytes = recv(sock, response, sizeof(response) - 1, 0);
    if (bytes <= 0) {
        printf("Không nhận được phản hồi từ server.\n");
    // Connection kept open (using global_sock)
        return;
    }
    response[bytes] = '\0';

    char *crlf = strstr(response, "\r\n");
    if (crlf) *crlf = '\0';

    int status_code;
    int invited_user_id = -1;
    if (sscanf(response, "%d %d", &status_code, &invited_user_id) < 1) {
        printf("Phản hồi không hợp lệ: %s\n", response);
    // Connection kept open (using global_sock)
        return;
    }

    if (status_code != 200) {
        if (status_code == 404) {
            printf("Username '%s' không tồn tại!\n", username);
        } else if (status_code == 500) {
            printf("Lỗi server khi tìm kiếm user!\n");
        } else {
            printf("Lỗi không xác định (code: %d)\n", status_code);
        }
    // Connection kept open (using global_sock)
        return;
    }

    if (invited_user_id <= 0) {
        printf("Không lấy được User ID!\n");
    // Connection kept open (using global_sock)
        return;
    }

    // Bước 2: Gửi lệnh INVITE_USER_TO_GROUP với user_id
    snprintf(command, sizeof(command), "INVITE_USER_TO_GROUP %s %d %d\r\n",
             current_token, group_id, invited_user_id);
    send(sock, command, strlen(command), 0);

    // Nhận response
    memset(response, 0, sizeof(response));
    bytes = recv(sock, response, sizeof(response) - 1, 0);
    if (bytes <= 0) {
        printf("Không nhận được phản hồi từ server.\n");
    // Connection kept open (using global_sock)
        return;
    }
    response[bytes] = '\0';

    crlf = strstr(response, "\r\n");
    if (crlf) *crlf = '\0';

    if (sscanf(response, "%d", &status_code) != 1) {
        printf("Phản hồi không hợp lệ: %s\n", response);
    // Connection kept open (using global_sock)
        return;
    }

    switch (status_code) {
        case 200:
            printf("Gửi lời mời thành công!\n");
            printf("User '%s' (ID: %d) sẽ nhận được lời mời tham gia nhóm.\n", username, invited_user_id);
            break;
        case 400:
            printf("Yêu cầu không hợp lệ!\n");
            break;
        case 401:
            printf("Token không hợp lệ hoặc đã hết hạn!\n");
            break;
        case 403:
            printf("Bạn không có quyền mời user (chỉ admin mới được mời)!\n");
            break;
        case 404:
            printf("User '%s' (ID: %d) không tồn tại hoặc nhóm không tồn tại!\n", username, invited_user_id);
            break;
        case 409:
            printf("User '%s' (ID: %d) đã là thành viên của nhóm!\n", username, invited_user_id);
            break;
        case 423:
            printf("Đã gửi lời mời cho user '%s' (ID: %d) trước đó!\n", username, invited_user_id);
            break;
        case 500:
            printf("Lỗi server!\n");
            break;
        default:
            printf("Lỗi không xác định (code: %d)\n", status_code);
    }

    // Connection kept open (using global_sock)
}

void handle_delete_item(int group_id) {
    printf("\n┌────────────────────────────────────────────┐\n");
    printf("│          XÓA FILE/THƯ MỤC                  │\n");
    printf("└────────────────────────────────────────────┘\n");

    printf("\n Loại (F=File, D=Directory): ");
    char type[10];
    if (scanf("%9s", type) != 1) {
        while (getchar() != '\n');
        printf("Loại không hợp lệ!\n");
        return;
    }
    while (getchar() != '\n');

    if (strcasecmp(type, "F") != 0 && strcasecmp(type, "D") != 0) {
        printf("Loại phải là 'F' hoặc 'D'!\n");
        return;
    }

    printf(" Nhập ID của %s cần xóa (hoặc 0 để quay lại): ",
           strcasecmp(type, "F") == 0 ? "file" : "thư mục");
    int item_id;
    if (scanf("%d", &item_id) != 1) {
        while (getchar() != '\n');
        printf("ID không hợp lệ!\n");
        return;
    }
    while (getchar() != '\n');

    if (item_id == 0) {
        printf(" Quay lại menu nhóm...\n");
        return;
    }

    int sock = connect_to_server();
    if (sock < 0) {
        printf("Không thể kết nối đến server!\n");
        return;
    }

    char command[BUFFER_SIZE];
    snprintf(command, sizeof(command), "DELETE_ITEM %s %d %s\r\n",
             current_token, item_id, type);
    send(sock, command, strlen(command), 0);

    char response[BUFFER_SIZE] = {0};
    int bytes = recv(sock, response, sizeof(response) - 1, 0);
    if (bytes <= 0) {
        printf("Không nhận được phản hồi từ server.\n");
    // Connection kept open (using global_sock)
        global_sock = -1;
        return;
    }
    response[bytes] = '\0';

    char *crlf = strstr(response, "\r\n");
    if (crlf) *crlf = '\0';

    int status_code;
    if (sscanf(response, "%d", &status_code) != 1) {
        printf("Phản hồi không hợp lệ: %s\n", response);
    // Connection kept open (using global_sock)
        global_sock = -1;
        return;
    }

    switch (status_code) {
        case 200:
            printf("Xóa %s (ID: %d) thành công!\n",
                   strcasecmp(type, "F") == 0 ? "file" : "thư mục", item_id);
            break;
        case 401:
            printf("Token không hợp lệ hoặc đã hết hạn!\n");
            break;
        case 403:
            printf("Bạn không có quyền xóa (chỉ admin mới được xóa)!\n");
            break;
        case 404:
            printf("Không tìm thấy %s với ID: %d!\n",
                   strcasecmp(type, "F") == 0 ? "file" : "thư mục", item_id);
            break;
        case 500:
            printf("Lỗi server!\n");
            break;
        default:
            printf("Lỗi không xác định (code: %d)\n", status_code);
    }

    // Connection kept open (using global_sock)
    global_sock = -1;
}

void handle_list_members(int group_id) {
    if (!is_token_valid()) {
        printf("Bạn cần đăng nhập để xem thành viên!\n");
        return;
    }

    int sock = connect_to_server();
    if (sock < 0) {
        printf("Không thể kết nối đến server!\n");
        return;
    }

    // Gửi lệnh LIST_GROUP_MEMBERS
    char command[BUFFER_SIZE];
    snprintf(command, sizeof(command), "LIST_GROUP_MEMBERS %s %d\r\n",
             current_token, group_id);
    send(sock, command, strlen(command), 0);

    // Nhận response
    char response[BUFFER_SIZE * 4] = {0};
    int bytes = recv(sock, response, sizeof(response) - 1, 0);
    if (bytes <= 0) {
        printf("Không nhận được phản hồi từ server.\n");
        return;
    }
    response[bytes] = '\0';

    // Parse response
    char *crlf = strstr(response, "\r\n");
    if (crlf) *crlf = '\0';

    int status_code = 0;
    char command_name[64];

    // Response format: "200 LIST_GROUP_MEMBERS user_id||username<SPACE>... group_id\r\n"
    if (sscanf(response, "%d %s", &status_code, command_name) < 1) {
        printf("Phản hồi không hợp lệ: %s\n", response);
        return;
    }

    if (status_code != 200) {
        switch (status_code) {
            case 400:
                printf("Yêu cầu không hợp lệ!\n");
                break;
            case 401:
                printf("Token không hợp lệ hoặc đã hết hạn!\n");
                break;
            case 403:
                printf("Bạn không có quyền xem thành viên (không phải thành viên nhóm)!\n");
                break;
            case 404:
                printf("Nhóm không tồn tại!\n");
                break;
            case 500:
                printf("Lỗi server!\n");
                break;
            default:
                printf("Lỗi không xác định (code: %d)\n", status_code);
        }
        return;
    }

    // Parse members data
    // Format: "200 LIST_GROUP_MEMBERS user_id||username user_id||username ... group_id"
    char *members_start = strstr(response, "LIST_GROUP_MEMBERS");
    if (!members_start) {
        printf("Không tìm thấy dữ liệu thành viên.\n");
        return;
    }
    members_start += strlen("LIST_GROUP_MEMBERS") + 1;

    // Đếm số thành viên
    int member_count = 0;
    char *temp = strdup(members_start);
    char *token = strtok(temp, " ");
    while (token) {
        if (strchr(token, '|')) {
            member_count++;
        }
        token = strtok(NULL, " ");
    }
    free(temp);

    printf("\n┌────────────────────────────────────────────┐\n");
    printf("│         DANH SÁCH THÀNH VIÊN NHÓM #%-3d     │\n", group_id);
    printf("└────────────────────────────────────────────┘\n");
    printf("\nTổng số thành viên: %d\n\n", member_count);

    const char *table_border =
        "┌──────────────────────────────┬──────────────┐\n";
    const char *table_separator =
        "├──────────────────────────────┼──────────────┤\n";
    const char *table_bottom =
        "└──────────────────────────────┴──────────────┘\n";

    printf("%s", table_border);
    printf("│ %-28s │ %-12s │\n", "Username", "Role");
    printf("%s", table_separator);

    // Parse và hiển thị từng thành viên
    temp = strdup(members_start);
    token = strtok(temp, " ");
    while (token) {
        if (strchr(token, '|')) {
            // Tạo bản sao để parse username và role
            char token_copy[256];
            strncpy(token_copy, token, sizeof(token_copy) - 1);
            token_copy[sizeof(token_copy) - 1] = '\0';

            char *separator = strchr(token_copy, '|');
            if (separator) {
                *separator = '\0';
                char *username = token_copy;
                char *role = separator + 1;

                // Tìm separator thứ 2 nếu có (cho trường hợp username||role)
                char *second_sep = strchr(role, '|');
                if (second_sep) {
                    *second_sep = '\0';
                    role = second_sep + 1;
                }

                if (username && role && strlen(username) > 0 && strlen(role) > 0) {
                    // Hiển thị với icon cho role
                    if (strcmp(role, "admin") == 0) {
                        printf("│ %-28s │  Admin       │\n", username);
                    } else {
                        printf("│ %-28s │  Member      │\n", username);
                    }
                }
            }
        }
        token = strtok(NULL, " ");
    }
    free(temp);

    printf("%s", table_bottom);
    printf("\nXem thành viên thành công!\n");
}

void handle_create_folder(int group_id, int parent_dir_id) {
    if (!is_token_valid()) {
        printf(" Bạn cần đăng nhập để tạo thư mục!\n");
        return;
    }

    printf("\n┌────────────────────────────────────────────┐\n");
    printf("│               TẠO THƯ MỤC MỚI              │\n");
    printf("└────────────────────────────────────────────┘\n");

    printf("\nNhập tên thư mục mới (hoặc '0' để hủy): ");
    char folder_name[256];
    if (fgets(folder_name, sizeof(folder_name), stdin) == NULL) {
        printf("Lỗi đọc dữ liệu!\n");
        return;
    }

    // Trim newline
    size_t len = strlen(folder_name);
    while (len > 0 && (folder_name[len - 1] == '\n' || folder_name[len - 1] == '\r')) {
        folder_name[len - 1] = '\0';
        len--;
    }

    if (strlen(folder_name) == 0) {
        printf("Tên thư mục không được để trống!\n");
        return;
    }

    if (strcmp(folder_name, "0") == 0) {
        printf("Hủy tạo thư mục...\n");
        return;
    }

    int sock = connect_to_server();
    if (sock < 0) {
        printf("Không thể kết nối đến server!\n");
        return;
    }

    // Gửi lệnh CREATE_FOLDER
    char command[BUFFER_SIZE];
    snprintf(command, sizeof(command), "CREATE_FOLDER %s %d %d %s\r\n",
             current_token, group_id, parent_dir_id, folder_name);
    send(sock, command, strlen(command), 0);

    // Nhận response
    char response[BUFFER_SIZE] = {0};
    int bytes = recv(sock, response, sizeof(response) - 1, 0);
    if (bytes <= 0) {
        printf("Không nhận được phản hồi từ server.\n");
    // Connection kept open (using global_sock)
        return;
    }
    response[bytes] = '\0';

    char *crlf = strstr(response, "\r\n");
    if (crlf) *crlf = '\0';

    int status_code = 0;
    if (sscanf(response, "%d", &status_code) < 1) {
        printf("Phản hồi không hợp lệ: %s\n", response);
    // Connection kept open (using global_sock)
        return;
    }

    // Connection kept open (using global_sock)

    switch (status_code) {
        case 200:
            printf("Tạo thư mục '%s' thành công!\n", folder_name);
            break;
        case 400:
            printf("Yêu cầu không hợp lệ!\n");
            break;
        case 401:
            printf("Token không hợp lệ hoặc đã hết hạn!\n");
            break;
        case 403:
            printf("Bạn không có quyền tạo thư mục trong nhóm này!\n");
            break;
        case 404:
            printf("Thư mục cha không tồn tại!\n");
            break;
        case 409:
            printf("Thư mục '%s' đã tồn tại trong thư mục này!\n", folder_name);
            break;
        case 500:
            printf("Lỗi server!\n");
            break;
        default:
            printf("Lỗi không xác định (code: %d)\n", status_code);
    }
}

void handle_list_folder_content(int group_id, int dir_id, int is_admin) {
    if (!is_token_valid()) {
        printf("Bạn cần đăng nhập để xem nội dung!\n");
        return;
    }

    while (1) {
        int sock = connect_to_server();
        if (sock < 0) {
            printf("Không thể kết nối đến server!\n");
            return;
        }

        // Gửi lệnh LIST_FOLDER_CONTENT
        char command[BUFFER_SIZE];
        snprintf(command, sizeof(command), "LIST_FOLDER_CONTENT %s %d %d\r\n",
                 current_token, group_id, dir_id);
        send(sock, command, strlen(command), 0);

        // Nhận response
        char response[BUFFER_SIZE * 8] = {0};
        int bytes = recv(sock, response, sizeof(response) - 1, 0);
        if (bytes <= 0) {
            printf("Không nhận được phản hồi từ server.\n");
            return;
        }
        response[bytes] = '\0';

        // Parse response
        char *crlf = strstr(response, "\r\n");
        if (crlf) *crlf = '\0';

        int status_code = 0;
        int current_dir_id = 0;
        int parent_dir_id = 0;
        char *data_start = NULL;

        // Parse: "200 current_dir_id parent_dir_id D|..."
        char *ptr = response;
        if (sscanf(ptr, "%d %d %d", &status_code, &current_dir_id, &parent_dir_id) < 2) {
            printf("Phản hồi không hợp lệ: %s\n", response);
    // Connection kept open (using global_sock)
            return;
        }

        // Tìm vị trí bắt đầu của data (sau 3 số)
        int space_count = 0;
        for (char *p = response; *p; p++) {
            if (*p == ' ') {
                space_count++;
                if (space_count == 3) {
                    data_start = p + 1;
                    break;
                }
            }
        }

        char *content_start = data_start ? data_start : "";

        if (status_code != 200) {
            switch (status_code) {
                case 400:
                    printf("Yêu cầu không hợp lệ!\n");
                    break;
                case 401:
                    printf("Token không hợp lệ hoặc đã hết hạn!\n");
                    break;
                case 403:
                    printf("Bạn không có quyền truy cập!\n");
                    break;
                case 404:
                    printf("Thư mục không tồn tại!\n");
                    break;
                case 500:
                    printf("Lỗi server!\n");
                    break;
                default:
                    printf("Lỗi không xác định (code: %d)\n", status_code);
            }
    // Connection kept open (using global_sock)
            return;
        }

        // Lưu danh sách items để có thể thao tác
        typedef struct {
            char type; // 'D' or 'F'
            int id;
            char name[256];
            long long size;
        } Item;

        Item items[1000];
        int item_count = 0;

        // Đếm folders và files
        int folder_count = 0;
        int file_count = 0;
        char *temp = strdup(content_start);
        char *token = strtok(temp, " ");

        while (token && item_count < 1000) {
            if (strchr(token, '|')) {
                char token_copy[512];
                strncpy(token_copy, token, sizeof(token_copy) - 1);
                token_copy[sizeof(token_copy) - 1] = '\0';

                char *parts[4] = {NULL, NULL, NULL, NULL};
                int part_count = 0;

                char *p = token_copy;
                char *start = p;
                while (*p && part_count < 4) {
                    if (*p == '|') {
                        *p = '\0';
                        parts[part_count++] = start;
                        start = p + 1;
                    }
                    p++;
                }
                if (start && *start) {
                    parts[part_count++] = start;
                }

                if (part_count >= 2) {
                    char *type = parts[0];
                    char *id_str = parts[1];
                    char *name = parts[2] ? parts[2] : "?";
                    char *size = parts[3] ? parts[3] : "0";

                    if (strcmp(type, "D") == 0 && strcmp(name, "..") != 0 && strcmp(name, "ROOT") != 0) {
                        items[item_count].type = 'D';
                        items[item_count].id = atoi(id_str);
                        strncpy(items[item_count].name, name, sizeof(items[item_count].name) - 1);
                        items[item_count].size = 0;
                        item_count++;
                        folder_count++;
                    } else if (strcmp(type, "F") == 0) {
                        items[item_count].type = 'F';
                        items[item_count].id = atoi(id_str);
                        strncpy(items[item_count].name, name, sizeof(items[item_count].name) - 1);
                        items[item_count].size = atoll(size);
                        item_count++;
                        file_count++;
                    }
                }
            }
            token = strtok(NULL, " ");
        }
        free(temp);

        // Connection kept open (using global_sock)

        // Hiển thị danh sách
        printf("\n┌───────────────────────────────────────────────────────────────────┐\n");
        printf("│              FILE EXPLORER - NHÓM #%-3d                            │\n", group_id);
        printf("└───────────────────────────────────────────────────────────────────┘\n");
        printf("\n Tổng: %d thư mục, %d file\n\n", folder_count, file_count);

        const char *table_border =
            "┌──────┬──────────┬───────┬────────────────────────────────┬──────────────┐\n";
        const char *table_separator =
            "├──────┼──────────┼───────┼────────────────────────────────┼──────────────┤\n";
        const char *table_bottom =
            "└──────┴──────────┴───────┴────────────────────────────────┴──────────────┘\n";

        printf("%s", table_border);
        printf("│ %-4s │ %-9s  │ %-5s │ %-30s  │ %-12s   │\n", "STT", "Loại", "ID", "Tên", "Kích thước");
        printf("%s", table_separator);

        for (int i = 0; i < item_count; i++) {
            if (items[i].type == 'D') {
                printf("│ %-4d │ %-8s │ %-5d │ %-30s │ %-12s │\n", i + 1, "Folder", items[i].id, items[i].name, "-");
            } else {
                char size_str[20];
                long long file_size = items[i].size;
                if (file_size < 1024) {
                    snprintf(size_str, sizeof(size_str), "%lld B", file_size);
                } else if (file_size < 1024 * 1024) {
                    snprintf(size_str, sizeof(size_str), "%.2f KB", file_size / 1024.0);
                } else if (file_size < 1024 * 1024 * 1024) {
                    snprintf(size_str, sizeof(size_str), "%.2f MB", file_size / (1024.0 * 1024.0));
                } else {
                    snprintf(size_str, sizeof(size_str), "%.2f GB", file_size / (1024.0 * 1024.0 * 1024.0));
                }
                printf("│ %-4d │ %-8s │ %-5d │ %-30s │ %-12s │\n", i + 1, "File", items[i].id, items[i].name, size_str);
            }
        }

        printf("%s", table_bottom);

        // Menu thao tác
        printf("\n┌──────────────── MENU THAO TÁC ────────────────┐\n");
        printf("│ [Số STT]       Xem nội dung thư mục con       │\n");
        printf("│ ────────────────────────────────────────────  │\n");
        printf("│ U               Upload file                   │\n");
        printf("│ D               Download file                 │\n");
        printf("│ N              Tạo thư mục mới                │\n");
        if (is_admin) {
            printf("│ X               Xóa item (Admin)              │\n");
            printf("│ R               Đổi tên (Admin)               │\n");
            printf("│ M              Di chuyển (Admin)              │\n");
            printf("│ C              Sao chép (Admin)               │\n");
        }
        printf("│ 0               Quay lại                      │\n");
        printf("└───────────────────────────────────────────────┘\n");
        printf("\n Nhập STT của thư mục để xem nội dung bên trong\n\n");
        printf("Chọn: ");

        char action[10];
        if (scanf("%9s", action) != 1) {
            while (getchar() != '\n');
            printf(" Lựa chọn không hợp lệ!\n");
            continue;
        }
        while (getchar() != '\n');

        // Xử lý số STT
        if (isdigit(action[0])) {
            int stt = atoi(action);
            if (stt == 0) {
                // Nếu đang ở root (parent_dir_id == 0 hoặc NULL), quay lại menu
                if (parent_dir_id == 0 || parent_dir_id < 0) {
                    return;
                }
                // Ngược lại, lùi về thư mục cha
                dir_id = parent_dir_id;
                continue;
            } else if (stt > 0 && stt <= item_count) {
                Item *selected = &items[stt - 1];
                if (selected->type == 'D') {
                    // Mở thư mục
                    dir_id = selected->id;
                    continue;
                } else {
                    // Download file
                    handle_download_file(group_id);
                }
            } else {
                printf(" STT không hợp lệ!\n");
            }
        }
        // Xử lý các lệnh ký tự
        else if (strcasecmp(action, "U") == 0) {
            handle_upload_file(group_id);
        }
        else if (strcasecmp(action, "D") == 0) {
            handle_download_file(group_id);
        }
        else if (strcasecmp(action, "N") == 0) {
            handle_create_folder(group_id, dir_id);
        }
        else if (strcasecmp(action, "X") == 0 && is_admin) {
            handle_delete_item(group_id);
        }
        else if (strcasecmp(action, "R") == 0 && is_admin) {
            handle_rename_item(group_id);
        }
        else if (strcasecmp(action, "M") == 0 && is_admin) {
            handle_move_item(group_id);
        }
        else if (strcasecmp(action, "C") == 0 && is_admin) {
            handle_copy_item(group_id);
        }
        else {
            printf(" Lệnh không hợp lệ!\n");
        }
    }
}

void handle_rename_item(int group_id) {
    printf("\n┌────────────────────────────────────────────┐\n");
    printf("│          ĐỔI TÊN FILE/THƯ MỤC               │\n");
    printf("└────────────────────────────────────────────┘\n");

    printf("\n Loại (F=File, D=Directory): ");
    char type[10];
    if (scanf("%9s", type) != 1) {
        while (getchar() != '\n');
        printf("Loại không hợp lệ!\n");
        return;
    }
    while (getchar() != '\n');

    if (strcasecmp(type, "F") != 0 && strcasecmp(type, "D") != 0) {
        printf("Loại phải là 'F' hoặc 'D'!\n");
        return;
    }

    printf(" Nhập ID của %s cần đổi tên (hoặc 0 để quay lại): ",
           strcasecmp(type, "F") == 0 ? "file" : "thư mục");
    int item_id;
    if (scanf("%d", &item_id) != 1) {
        while (getchar() != '\n');
        printf("ID không hợp lệ!\n");
        return;
    }
    while (getchar() != '\n');

    if (item_id == 0) {
        printf("Quay lại menu nhóm...\n");
        return;
    }

    printf(" Tên mới: ");
    char new_name[256];
    if (scanf("%255s", new_name) != 1) {
        while (getchar() != '\n');
        printf("Tên không hợp lệ!\n");
        return;
    }
    while (getchar() != '\n');

    int sock = connect_to_server();
    if (sock < 0) {
        printf("Không thể kết nối đến server!\n");
        return;
    }

    char command[BUFFER_SIZE];
    snprintf(command, sizeof(command), "RENAME_ITEM %s %d %s %s\r\n",
             current_token, item_id, new_name, type);
    send(sock, command, strlen(command), 0);

    char response[BUFFER_SIZE] = {0};
    int bytes = recv(sock, response, sizeof(response) - 1, 0);
    if (bytes <= 0) {
        printf("Không nhận được phản hồi từ server.\n");
    // Connection kept open (using global_sock)
        global_sock = -1;
        return;
    }
    response[bytes] = '\0';

    char *crlf = strstr(response, "\r\n");
    if (crlf) *crlf = '\0';

    int status_code;
    if (sscanf(response, "%d", &status_code) != 1) {
        printf("Phản hồi không hợp lệ: %s\n", response);
    // Connection kept open (using global_sock)
        global_sock = -1;
        return;
    }

    switch (status_code) {
        case 200:
            printf("Đổi tên %s (ID: %d) thành '%s' thành công!\n",
                   strcasecmp(type, "F") == 0 ? "file" : "thư mục", item_id, new_name);
            break;
        case 401:
            printf("Token không hợp lệ hoặc đã hết hạn!\n");
            break;
        case 403:
            printf("Bạn không có quyền đổi tên (chỉ admin mới được đổi tên)!\n");
            break;
        case 404:
            printf("Không tìm thấy %s với ID: %d!\n",
                   strcasecmp(type, "F") == 0 ? "file" : "thư mục", item_id);
            break;
        case 500:
            printf("Lỗi server!\n");
            break;
        default:
            printf("Lỗi không xác định (code: %d)\n", status_code);
    }

    // Connection kept open (using global_sock)
    global_sock = -1;
}

void handle_move_item(int group_id) {
    printf("\n┌────────────────────────────────────────────┐\n");
    printf("│       DI CHUYỂN FILE/THƯ MỤC           │\n");
    printf("└────────────────────────────────────────────┘\n");

    printf("\n Loại (F=File, D=Directory): ");
    char type[10];
    if (scanf("%9s", type) != 1) {
        while (getchar() != '\n');
        printf("Loại không hợp lệ!\n");
        return;
    }
    while (getchar() != '\n');

    if (strcasecmp(type, "F") != 0 && strcasecmp(type, "D") != 0) {
        printf("Loại phải là 'F' hoặc 'D'!\n");
        return;
    }

    printf(" Nhập ID của %s cần di chuyển (hoặc 0 để quay lại): ",
           strcasecmp(type, "F") == 0 ? "file" : "thư mục");
    int item_id;
    if (scanf("%d", &item_id) != 1) {
        while (getchar() != '\n');
        printf("ID không hợp lệ!\n");
        return;
    }
    while (getchar() != '\n');

    if (item_id == 0) {
        printf("Quay lại menu nhóm...\n");
        return;
    }

    printf(" ID thư mục đích: ");
    int target_dir_id;
    if (scanf("%d", &target_dir_id) != 1) {
        while (getchar() != '\n');
        printf("ID không hợp lệ!\n");
        return;
    }
    while (getchar() != '\n');

    int sock = connect_to_server();
    if (sock < 0) {
        printf("Không thể kết nối đến server!\n");
        return;
    }

    char command[BUFFER_SIZE];
    snprintf(command, sizeof(command), "MOVE_ITEM %s %d %d %s\r\n",
             current_token, item_id, target_dir_id, type);

    int sent = send(sock, command, strlen(command), 0);
    if (sent <= 0) {
        printf("Không thể gửi lệnh đến server!\n");
    // Connection kept open (using global_sock)
        global_sock = -1;
        return;
    }

    char response[BUFFER_SIZE] = {0};
    int bytes = recv(sock, response, sizeof(response) - 1, 0);

    if (bytes <= 0) {
        printf("Không nhận được phản hồi từ server.\n");
    // Connection kept open (using global_sock)
        global_sock = -1;
        return;
    }
    response[bytes] = '\0';

    char *crlf = strstr(response, "\r\n");
    if (crlf) *crlf = '\0';

    int status_code;
    if (sscanf(response, "%d", &status_code) != 1) {
        printf("Phản hồi không hợp lệ: %s\n", response);
    // Connection kept open (using global_sock)
        global_sock = -1;
        return;
    }

    switch (status_code) {
        case 200:
            printf("Di chuyển %s (ID: %d) đến thư mục (ID: %d) thành công!\n",
                   strcasecmp(type, "F") == 0 ? "file" : "thư mục", item_id, target_dir_id);
            break;
        case 401:
            printf("Token không hợp lệ hoặc đã hết hạn!\n");
            break;
        case 403:
            printf("Bạn không có quyền di chuyển hoặc file/thư mục không cùng nhóm!\n");
            break;
        case 404:
            printf("Không tìm thấy %s hoặc thư mục đích!\n",
                   strcasecmp(type, "F") == 0 ? "file" : "thư mục");
            break;
        case 500:
            printf("Lỗi server!\n");
            break;
        default:
            printf("Lỗi không xác định (code: %d)\n", status_code);
    }

    // Connection kept open (using global_sock)
    global_sock = -1;
}

void handle_copy_item(int group_id) {
    printf("\n┌────────────────────────────────────────────┐\n");
    printf("│            SAO CHÉP FILE/THƯ MỤC           │\n");
    printf("└────────────────────────────────────────────┘\n");

    printf("\n Loại (F=File, D=Directory): ");
    char type[10];
    if (scanf("%9s", type) != 1) {
        while (getchar() != '\n');
        printf("Loại không hợp lệ!\n");
        return;
    }
    while (getchar() != '\n');

    if (strcasecmp(type, "F") != 0 && strcasecmp(type, "D") != 0) {
        printf("Loại phải là 'F' hoặc 'D'!\n");
        return;
    }

    printf("Nhập ID của %s cần sao chép (hoặc 0 để quay lại): ",
           strcasecmp(type, "F") == 0 ? "file" : "thư mục");
    int item_id;
    if (scanf("%d", &item_id) != 1) {
        while (getchar() != '\n');
        printf("ID không hợp lệ!\n");
        return;
    }
    while (getchar() != '\n');

    if (item_id == 0) {
        printf("🔙 Quay lại menu nhóm...\n");
        return;
    }

    printf("ID thư mục đích: ");
    int target_dir_id;
    if (scanf("%d", &target_dir_id) != 1) {
        while (getchar() != '\n');
        printf("ID không hợp lệ!\n");
        return;
    }
    while (getchar() != '\n');

    int sock = connect_to_server();
    if (sock < 0) {
        printf("Không thể kết nối đến server!\n");
        return;
    }

    char command[BUFFER_SIZE];
    snprintf(command, sizeof(command), "COPY_ITEM %s %d %d %s\r\n",
             current_token, item_id, target_dir_id, type);
    send(sock, command, strlen(command), 0);

    char response[BUFFER_SIZE] = {0};
    int bytes = recv(sock, response, sizeof(response) - 1, 0);
    if (bytes <= 0) {
        printf("Không nhận được phản hồi từ server.\n");
    // Connection kept open (using global_sock)
        global_sock = -1;
        return;
    }
    response[bytes] = '\0';

    char *crlf = strstr(response, "\r\n");
    if (crlf) *crlf = '\0';

    int status_code;
    if (sscanf(response, "%d", &status_code) != 1) {
        printf("Phản hồi không hợp lệ: %s\n", response);
    // Connection kept open (using global_sock)
        global_sock = -1;
        return;
    }

    switch (status_code) {
        case 200:
            printf("Sao chép %s (ID: %d) đến thư mục (ID: %d) thành công!\n",
                   strcasecmp(type, "F") == 0 ? "file" : "thư mục", item_id, target_dir_id);
            break;
        case 401:
            printf("Token không hợp lệ hoặc đã hết hạn!\n");
            break;
        case 403:
            printf("Bạn không có quyền sao chép hoặc file/thư mục không cùng nhóm!\n");
            break;
        case 404:
            printf("Không tìm thấy %s hoặc thư mục đích!\n",
                   strcasecmp(type, "F") == 0 ? "file" : "thư mục");
            break;
        case 500:
            printf("Lỗi server!\n");
            break;
        default:
            printf("Lỗi không xác định (code: %d)\n", status_code);
    }

    // Connection kept open (using global_sock)
    global_sock = -1;
}

void handle_request_join_group() {
    if (!is_token_valid()) {
        printf("Bạn cần đăng nhập để gửi yêu cầu tham gia nhóm!\n");
        return;
    }

    printf("\n--- XIN THAM GIA NHÓM ---\n");
    printf("Đang tải danh sách các nhóm chưa tham gia...\n\n");

    int sock = connect_to_server();
    if (sock < 0) {
        printf("✗ Không thể kết nối đến server!\n");
        return;
    }

    // Gửi lệnh LIST_GROUPS_NOT_JOINED
    char command[BUFFER_SIZE];
    snprintf(command, sizeof(command), "LIST_GROUPS_NOT_JOINED %s\r\n", current_token);
    send(sock, command, strlen(command), 0);

    // Nhận response
    char response[BUFFER_SIZE] = {0};
    int bytes = recv(sock, response, sizeof(response) - 1, 0);
    if (bytes <= 0) {
        printf("✗ Không nhận được phản hồi từ server.\n");
        return;
    }
    response[bytes] = '\0';

    int status_code = 0;
    int group_count = 0;
    sscanf(response, "%d %d", &status_code, &group_count);

    if (status_code != 200) {
        printf("✗ Lỗi khi tải danh sách nhóm (Mã: %d).\n", status_code);
        return;
    }

    printf("DANH SÁCH CÁC NHÓM CÓ THỂ THAM GIA (%d nhóm)\n", group_count);

    if (group_count == 0) {
        printf("\n  Không có nhóm nào để tham gia.\n");
        printf("  Bạn đã là thành viên của tất cả các nhóm hoặc chưa có nhóm nào được tạo.\n");
        return;
    }

    const char *table_border =
        "+------+----------------------------+----------------------------+------------------+\n";
    printf("%s", table_border);
    printf("| %-4s | %-26s | %-26s | %-16s |\n",
           "ID", "Tên nhóm", "Mô tả", "Admin");
    printf("%s", table_border);

    // Parse danh sách nhóm từ response
    char *list_start = strstr(response, "\r\n");
    if (!list_start) {
        printf("✗ Không thể parse danh sách nhóm.\n");
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

        // Format: group_id|group_name|description|admin_name|created_at
        char *group_id = strtok(line_copy, "|");
        char *group_name = strtok(NULL, "|");
        char *description = strtok(NULL, "|");
        char *admin_name = strtok(NULL, "|");
        char *created_at = strtok(NULL, "|");

        const char *safe_id = group_id ? group_id : "?";
        const char *safe_name = group_name && strlen(group_name) > 0 ? group_name : "(không tên)";
        const char *safe_desc = (description && strlen(description) > 0) ? description : "(không mô tả)";
        const char *safe_admin = admin_name && strlen(admin_name) > 0 ? admin_name : "(unknown)";

        printf("| %-4.4s | %-26.26s | %-26.26s | %-16.16s |\n",
               safe_id, safe_name, safe_desc, safe_admin);

        if (!next_line) break;
        list_start = next_line + 2;
    }

    printf("%s", table_border);

    printf("\n");
    printf("┌──────────────────────────────────────┐\n");
    printf("│ Nhập Group ID để gửi yêu cầu         │\n");
    printf("│ Hoặc nhập 0 để quay lại menu chính  │\n");
    printf("└──────────────────────────────────────┘\n");
    printf("Lựa chọn: ");

    int group_id;
    if (scanf("%d", &group_id) != 1) {
        printf("✗ Lựa chọn không hợp lệ!\n");
        while (getchar() != '\n');
        return;
    }

    // Quay lại menu chính
    if (group_id == 0) {
        printf("Quay lại menu chính...\n");
        return;
    }

    if (group_id < 0) {
        printf("✗ Group ID không hợp lệ!\n");
        return;
    }

    // Gửi lệnh REQUEST_JOIN_GROUP
    snprintf(command, sizeof(command), "REQUEST_JOIN_GROUP %s %d\r\n",
             current_token, group_id);
    send(sock, command, strlen(command), 0);

    // Nhận response
    memset(response, 0, sizeof(response));
    bytes = recv(sock, response, sizeof(response) - 1, 0);
    if (bytes <= 0) {
        printf("✗ Không nhận được phản hồi từ server.\n");
        return;
    }
    response[bytes] = '\0';

    // Remove trailing CRLF
    char *crlf = strstr(response, "\r\n");
    if (crlf) *crlf = '\0';

    // Parse response: "200 REQUEST_JOIN_GROUP <group_id>"
    status_code = 0;
    char cmd_name[50];
    int resp_group_id = 0;

    if (sscanf(response, "%d %s %d", &status_code, cmd_name, &resp_group_id) >= 1) {
        printf("\n");
        switch (status_code) {
            case 200:
                printf(" Gửi yêu cầu tham gia nhóm #%d thành công!\n", group_id);
                printf("  Yêu cầu của bạn đang chờ admin phê duyệt.\n");
                break;
            case 409:
                printf("✗ Bạn đã là thành viên của nhóm #%d rồi!\n", group_id);
                break;
            case 423:
                printf("  Bạn đã gửi yêu cầu tham gia nhóm #%d trước đó.\n", group_id);
                printf("ℹ  Vui lòng chờ admin phê duyệt.\n");
                break;
            case 404:
                printf("Nhóm với ID %d không tồn tại!\n", group_id);
                break;
            case 401:
                printf("✗ Token không hợp lệ hoặc đã hết hạn. Vui lòng đăng nhập lại!\n");
                memset(current_token, 0, sizeof(current_token));
                break;
            case 500:
                printf("✗ Lỗi server! Vui lòng thử lại sau.\n");
                break;
            default:
                printf("✗ Lỗi không xác định (Mã: %d)\n", status_code);
                break;
        }
    } else {
        printf("✗ Phản hồi không hợp lệ từ server: %s\n", response);
    }
}


void handle_view_pending_requests() {
    if (!is_token_valid()) {
        printf("Bạn cần đăng nhập để xem yêu cầu!\n");
        return;
    }

    while (1) {  // Loop để xét duyệt nhiều request
        printf("\n--- XEM & PHÊ DUYỆT YÊU CẦU ---\n");
        printf("Đang tải danh sách yêu cầu...\n\n");

        int sock = connect_to_server();
        if (sock < 0) {
            printf("✗ Không thể kết nối đến server!\n");
            return;
        }

        // Gửi lệnh GET_PENDING_REQUESTS
        char command[BUFFER_SIZE];
        snprintf(command, sizeof(command), "GET_PENDING_REQUESTS %s\r\n", current_token);
        send(sock, command, strlen(command), 0);

        // Nhận response
        char response[BUFFER_SIZE] = {0};
        int bytes = recv(sock, response, sizeof(response) - 1, 0);
        if (bytes <= 0) {
            printf("✗ Không nhận được phản hồi từ server.\n");
            return;
        }
        response[bytes] = '\0';

        int status_code = 0;
        int request_count = 0;
        sscanf(response, "%d %d", &status_code, &request_count);

        if (status_code != 200) {
            printf("✗ Lỗi khi tải danh sách yêu cầu (Mã: %d).\n", status_code);
            return;
        }

        if (request_count == 0) {
            printf("\nKhông có yêu cầu nào đang chờ duyệt.\n");
            printf("Bạn chưa có yêu cầu nào cần xét duyệt trong các nhóm bạn quản lý.\n");
            return;
        }

        printf("DANH SÁCH CÁC YÊU CẦU CHỜ DUYỆT (%d yêu cầu)\n\n", request_count);

        // Parse danh sách requests từ response
        char *list_start = strstr(response, "\r\n");
        if (!list_start) {
            printf("✗ Không thể parse danh sách yêu cầu.\n");
            return;
        }
        list_start += 2;

        int current_group_id = -1;
        char current_group_name[256] = "";

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

            // Format: request_id|user_id|username|group_id|group_name|created_at
            char *request_id = strtok(line_copy, "|");
            char *user_id = strtok(NULL, "|");
            char *username = strtok(NULL, "|");
            char *group_id = strtok(NULL, "|");
            char *group_name = strtok(NULL, "|");
            char *created_at = strtok(NULL, "|");

            int gid = group_id ? atoi(group_id) : -1;

            // Nếu là nhóm mới, in header
            if (gid != current_group_id) {
                current_group_id = gid;
                strncpy(current_group_name, group_name ? group_name : "(unknown)", sizeof(current_group_name) - 1);

                printf("\n┌────────────────────────────────────────────────────────────────┐\n");
                printf("│ Nhóm #%-4d: %-48s │\n", current_group_id, current_group_name);
                printf("├────────────┬─────────┬──────────────────┬─────────────────────┤\n");
                printf("│ Request ID │ User ID │ Username         │ Ngày gửi            │\n");
                printf("├────────────┼─────────┼──────────────────┼─────────────────────┤\n");
            }

            const char *safe_request_id = request_id ? request_id : "?";
            const char *safe_user_id = user_id ? user_id : "?";
            const char *safe_username = username ? username : "(unknown)";
            const char *safe_created_at = created_at ? created_at : "";

            printf("│ %-10s │ %-7s │ %-16.16s │ %-19.19s │\n",
                   safe_request_id, safe_user_id, safe_username, safe_created_at);

            if (!next_line) break;
            list_start = next_line + 2;
        }

        if (current_group_id != -1) {
            printf("└────────────┴─────────┴──────────────────┴─────────────────────┘\n");
        }

        // Phần xét duyệt
        printf("\n┌─────────────────────────────────────────────────┐\n");
        printf("│ Nhập Request ID để xét duyệt                    │\n");
        printf("│ Hoặc nhập 0 để quay lại menu chính             │\n");
        printf("└─────────────────────────────────────────────────┘\n");
        printf("Request ID: ");

        int request_id;
        if (scanf("%d", &request_id) != 1) {
            printf("✗ Request ID không hợp lệ!\n");
            while (getchar() != '\n');
            continue;
        }

        // Quay lại menu chính
        if (request_id == 0) {
            printf("Quay lại menu chính...\n");
            return;
        }

        if (request_id < 0) {
            printf("✗ Request ID phải lớn hơn 0!\n");
            continue;
        }

        while (getchar() != '\n'); // Clear buffer

        // Chọn hành động
        printf("\n┌─────────────────────────────────────────────────┐\n");
        printf("│ Chọn hành động:                                 │\n");
        printf("│   1. Accept (Chấp nhận - Thêm user vào nhóm)   │\n");
        printf("│   2. Reject (Từ chối yêu cầu)                  │\n");
        printf("└─────────────────────────────────────────────────┘\n");
        printf("Lựa chọn (1/2): ");

        char option[20];
        if (!fgets(option, sizeof(option), stdin)) {
            printf("✗ Không đọc được lựa chọn!\n");
            continue;
        }
        option[strcspn(option, "\n")] = 0;

        const char *action = NULL;
        if (strcmp(option, "1") == 0) {
            action = "accepted";
        } else if (strcmp(option, "2") == 0) {
            action = "rejected";
        } else {
            printf("✗ Lựa chọn không hợp lệ!\n");
            continue;
        }

        // Gửi lệnh HANDLE_JOIN_REQUEST
        sock = connect_to_server();
        if (sock < 0) {
            printf("✗ Không thể kết nối đến server!\n");
            continue;
        }

        snprintf(command, sizeof(command), "HANDLE_JOIN_REQUEST %s %d %s\r\n",
                 current_token, request_id, action);
        send(sock, command, strlen(command), 0);

        // Nhận response
        memset(response, 0, sizeof(response));
        bytes = recv(sock, response, sizeof(response) - 1, 0);
        if (bytes <= 0) {
            printf("✗ Không nhận được phản hồi từ server.\n");
            continue;
        }
        response[bytes] = '\0';

        // Remove trailing CRLF
        char *crlf = strstr(response, "\r\n");
        if (crlf) *crlf = '\0';

        // Parse response
        status_code = 0;
        char cmd_name[50];
        int resp_request_id = 0;

        if (sscanf(response, "%d %s %d", &status_code, cmd_name, &resp_request_id) >= 1) {
            printf("\n");
            switch (status_code) {
                case 200:
                    if (strcmp(action, "accepted") == 0) {
                        printf("Đã chấp nhận yêu cầu #%d thành công!\n", request_id);
                        printf("User đã được thêm vào nhóm.\n");
                    } else {
                        printf("Đã từ chối yêu cầu #%d thành công!\n", request_id);
                    }
                    break;
                case 403:
                    printf("✗ Bạn không có quyền xét duyệt yêu cầu này!\n");
                    printf("Chỉ admin của nhóm mới có thể xét duyệt.\n");
                    break;
                case 404:
                    printf("✗ Không tìm thấy yêu cầu với ID %d!\n", request_id);
                    break;
                case 409:
                    printf("✗ Yêu cầu này đã được xét duyệt trước đó!\n");
                    break;
                case 401:
                    printf("✗ Token không hợp lệ hoặc đã hết hạn. Vui lòng đăng nhập lại!\n");
                    memset(current_token, 0, sizeof(current_token));
                    return;
                case 500:
                    printf("✗ Lỗi server! Vui lòng thử lại sau.\n");
                    break;
                default:
                    printf("✗ Lỗi không xác định (Mã: %d)\n", status_code);
                    break;
            }
        } else {
            printf("✗ Phản hồi không hợp lệ từ server: %s\n", response);
        }

        printf("\nĐang tải lại danh sách...\n");
        // Loop sẽ tự động tải lại danh sách
    }
}

void handle_approve_request() {
    if (!is_token_valid()) {
        printf("Bạn cần đăng nhập để phê duyệt yêu cầu!\n");
        return;
    }

    int request_id;
    char option[20];

    printf("\n--- PHÊ DUYỆT YÊU CẦU ---\n");
    printf("Nhập Request ID cần xử lý (hoặc 0 để hủy): ");

    if (scanf("%d", &request_id) != 1) {
        printf("✗ Request ID không hợp lệ!\n");
        while (getchar() != '\n');
        return;
    }
    if (request_id == 0) {
        printf("Đã hủy thao tác.\n");
        return;
    }

    if (request_id < 0) {
        printf("✗ Request ID phải lớn hơn 0!\n");
        return;
    }

    while (getchar() != '\n'); // Clear buffer

    printf("\nChọn hành động:\n");
    printf("  1. accepted (Chấp nhận - Thêm user vào nhóm)\n");
    printf("  2. rejected (Từ chối - Không thêm vào nhóm)\n");
    printf("Lựa chọn (1/2): ");

    if (!fgets(option, sizeof(option), stdin)) {
        printf("✗ Không đọc được lựa chọn!\n");
        return;
    }
    option[strcspn(option, "\n")] = 0;

    const char *action = NULL;
    if (strcmp(option, "1") == 0) {
        action = "accepted";
    } else if (strcmp(option, "2") == 0) {
        action = "rejected";
    } else {
        printf("✗ Lựa chọn không hợp lệ!\n");
        return;
    }

    int sock = connect_to_server();
    if (sock < 0) {
        printf("✗ Không thể kết nối đến server!\n");
        return;
    }

    // Gửi lệnh HANDLE_JOIN_REQUEST
    char command[BUFFER_SIZE];
    snprintf(command, sizeof(command), "HANDLE_JOIN_REQUEST %s %d %s\r\n",
             current_token, request_id, action);
    send(sock, command, strlen(command), 0);

    // Nhận response
    char response[BUFFER_SIZE] = {0};
    int bytes = recv(sock, response, sizeof(response) - 1, 0);
    if (bytes <= 0) {
        printf("✗ Không nhận được phản hồi từ server.\n");
        return;
    }
    response[bytes] = '\0';

    // Remove trailing CRLF
    char *crlf = strstr(response, "\r\n");
    if (crlf) *crlf = '\0';

    // Parse response: "200 HANDLE_JOIN_REQUEST <request_id>"
    int status_code = 0;
    char cmd_name[50];
    int resp_request_id = 0;

    if (sscanf(response, "%d %s %d", &status_code, cmd_name, &resp_request_id) >= 1) {
        switch (status_code) {
            case 200:
                if (strcmp(action, "accepted") == 0) {
                    printf("✓ Đã chấp nhận yêu cầu thành công!\n");
                    printf("User đã được thêm vào nhóm.\n");
                } else {
                    printf("✓ Đã từ chối yêu cầu thành công!\n");
                }
                break;
            case 409:
                printf("✗ Yêu cầu này đã được xử lý trước đó rồi!\n");
                break;
            case 403:
                printf("✗ Bạn không có quyền xử lý yêu cầu này!\n");
                printf("Chỉ admin của nhóm mới có thể phê duyệt.\n");
                break;
            case 404:
                printf("✗ Yêu cầu với ID %d không tồn tại!\n", request_id);
                break;
            case 401:
                printf("✗ Token không hợp lệ hoặc đã hết hạn. Vui lòng đăng nhập lại!\n");
                memset(current_token, 0, sizeof(current_token));
                break;
            case 400:
                printf("✗ Dữ liệu không hợp lệ!\n");
                break;
            case 500:
                printf("✗ Lỗi server! Vui lòng thử lại sau.\n");
                break;
            default:
                printf("✗ Lỗi không xác định (Mã: %d)\n", status_code);
                break;
        }
    } else {
        printf("✗ Phản hồi không hợp lệ từ server: %s\n", response);
    }
}

void handle_view_my_invitations() {
    if (!is_token_valid()) {
        printf("Bạn cần đăng nhập để xem lời mời!\n");
        return;
    }

    while (1) {  // Loop để có thể xử lý nhiều invitation
        printf("\n┌────────────────────────────────────────────┐\n");
        printf("│         LỜI MỜI THAM GIA NHÓM CỦA TÔI      │\n");
        printf("└────────────────────────────────────────────┘\n");

        int sock = connect_to_server();
        if (sock < 0) {
            printf("Không thể kết nối đến server!\n");
            return;
        }

        // Gửi lệnh GET_MY_INVITATIONS
        char command[BUFFER_SIZE];
        snprintf(command, sizeof(command), "GET_MY_INVITATIONS %s\r\n", current_token);
        send(sock, command, strlen(command), 0);

        // Nhận response
        char response[BUFFER_SIZE] = {0};
        int bytes = recv(sock, response, sizeof(response) - 1, 0);
        if (bytes <= 0) {
            printf("Không nhận được phản hồi từ server.\n");
            return;
        }
        response[bytes] = '\0';

        // Remove trailing CRLF at the end (not the first one)
        size_t len = strlen(response);
        if (len >= 2 && response[len-2] == '\r' && response[len-1] == '\n') {
            response[len-2] = '\0';
        }

        // Parse response: "200 [invitation_1] [invitation_2] ..."
        int status_code = 0;
        char cmd_name[50];
        char invitations_data[BUFFER_SIZE] = {0};

        // Parse ít nhất status_code và cmd_name
        int parsed = sscanf(response, "%d %[^\n]", &status_code, invitations_data);
        if (parsed < 1) {
            printf("Phản hồi không hợp lệ từ server.\n");
            printf("Debug: response = \n", response);
            return;
        }

        if (status_code != 200) {
            if (status_code == 401) {
                printf("Token không hợp lệ hoặc hết hạn!\n");
                memset(current_token, 0, sizeof(current_token));
            } else {
                printf("Lỗi khi tải danh sách lời mời (Mã: %d).\n", status_code);
            }
            return;
        }

        if (strlen(invitations_data) == 0 || strstr(invitations_data, "[invitation_") == NULL) {
            printf("\nBạn không có lời mời nào đang chờ xử lý.\n");
            return;
        }

        printf("\nDANH SÁCH LỜI MỜI:\n\n");
        printf("┌────────────┬──────────┬───────────────────────────────┬──────────────┐\n");
        printf("│ Request ID │ Group ID │ Tên nhóm                      │ Trạng thái   │\n");
        printf("├────────────┼──────────┼───────────────────────────────┼──────────────┤\n");

        // Parse invitations: [invitation_n]: group_id group_name request_id request_status
        char *ptr = invitations_data;
        int invitation_count = 0;

        while (*ptr) {
            // Find next invitation marker
            char *inv_start = strstr(ptr, "[invitation_");
            if (!inv_start) break;

            // Find the colon after invitation marker
            char *colon = strchr(inv_start, ':');
            if (!colon) break;

            // Parse: group_id group_name request_id request_status
            // Copy data để parse (vì sẽ modify chuỗi)
            char line_buf[512];
            char *data = colon + 1;
            while (*data == ' ') data++; // Skip leading spaces

            strncpy(line_buf, data, sizeof(line_buf) - 1);
            line_buf[sizeof(line_buf) - 1] = '\0';

            // Loại bỏ khoảng trắng/newline ở cuối
            size_t len = strlen(line_buf);
            while (len > 0 && (line_buf[len-1] == ' ' || line_buf[len-1] == '\n' ||
                              line_buf[len-1] == '\r' || line_buf[len-1] == '\t')) {
                line_buf[--len] = '\0';
            }

            if (len == 0) {
                ptr = colon + 1;
                while (*ptr && *ptr != '[') ptr++;
                continue;
            }

            // Bây giờ parse: "6 code java 7 pending"
            // Tìm status (từ cuối cùng)
            char *last_space = strrchr(line_buf, ' ');
            if (!last_space) {
                ptr = colon + 1;
                while (*ptr && *ptr != '[') ptr++;
                continue;
            }

            char status[32];
            strncpy(status, last_space + 1, sizeof(status) - 1);
            status[sizeof(status) - 1] = '\0';
            *last_space = '\0'; // Cắt chuỗi: "6 code java 7"

            // Tìm request_id (từ cuối cùng của chuỗi còn lại)
            char *second_last_space = strrchr(line_buf, ' ');
            if (!second_last_space) {
                ptr = colon + 1;
                while (*ptr && *ptr != '[') ptr++;
                continue;
            }

            int request_id = atoi(second_last_space + 1);
            *second_last_space = '\0'; // Cắt chuỗi: "6 code java"

            // Parse group_id (số đầu tiên)
            int group_id = atoi(line_buf);

            // Group name là phần còn lại sau group_id
            char *group_name_start = line_buf;
            while (*group_name_start && isdigit(*group_name_start)) {
                group_name_start++;
            }
            while (*group_name_start == ' ') group_name_start++;

            printf("│ %-10d │ %-8d │ %-29.29s │ %-12s │\n",
                   request_id, group_id, group_name_start, status);
            invitation_count++;

            // Move to next invitation
            ptr = colon + 1;
            while (*ptr && *ptr != '[') ptr++;
        }

        printf("└────────────┴──────────┴───────────────────────────────┴──────────────┘\n");

        if (invitation_count == 0) {
            printf("\nBạn không có lời mời nào đang chờ xử lý.\n");
            return;
        }

        // Hỏi user có muốn xử lý không
        printf("\n┌─────────────────────────────────────────────────┐\n");
        printf("│ Nhập Request ID để chấp nhận/từ chối           │\n");
        printf("│ Hoặc nhập 0 để quay lại menu chính             │\n");
        printf("└─────────────────────────────────────────────────┘\n");
        printf("Request ID: ");

        int request_id;
        if (scanf("%d", &request_id) != 1) {
            printf("Request ID không hợp lệ!\n");
            while (getchar() != '\n');
            continue;
        }
        while (getchar() != '\n');

        // Quay lại menu chính
        if (request_id == 0) {
            printf("Quay lại menu chính...\n");
            return;
        }

        if (request_id < 0) {
            printf("Request ID phải lớn hơn 0!\n");
            continue;
        }

        // Hỏi action
        printf("\n┌─────────────────────────────────────────┐\n");
        printf("│ 1. Chấp nhận (Tham gia nhóm)           │\n");
        printf("│ 2. Từ chối                             │\n");
        printf("└─────────────────────────────────────────┘\n");
        printf("Lựa chọn (1/2): ");

        int action_choice;
        if (scanf("%d", &action_choice) != 1) {
            printf("Lựa chọn không hợp lệ!\n");
            while (getchar() != '\n');
            continue;
        }
        while (getchar() != '\n');

        const char *action = NULL;
        if (action_choice == 1) {
            action = "accept";
        } else if (action_choice == 2) {
            action = "reject";
        } else {
            printf("Lựa chọn không hợp lệ!\n");
            continue;
        }

        // Gửi lệnh RESPOND_TO_INVITATION
        snprintf(command, sizeof(command), "RESPOND_TO_INVITATION %s %d %s\r\n",
                 current_token, request_id, action);
        send(sock, command, strlen(command), 0);

        // Nhận response
        memset(response, 0, sizeof(response));
        bytes = recv(sock, response, sizeof(response) - 1, 0);
        if (bytes <= 0) {
            printf("Không nhận được phản hồi từ server.\n");
            return;
        }
        response[bytes] = '\0';

        char *crlf2 = strstr(response, "\r\n");
        if (crlf2) *crlf2 = '\0';

        if (sscanf(response, "%d", &status_code) != 1) {
            printf("Phản hồi không hợp lệ: %s\n", response);
            return;
        }

        switch (status_code) {
            case 200:
                printf("Đã chấp nhận lời mời! Bạn đã tham gia nhóm.\n");
                break;
            case 201:
                printf("Đã từ chối lời mời.\n");
                break;
            case 400:
                printf("Yêu cầu không hợp lệ!\n");
                break;
            case 401:
                printf("Token không hợp lệ hoặc hết hạn!\n");
                memset(current_token, 0, sizeof(current_token));
                return;
            case 403:
                printf("Request này không phải là lời mời!\n");
                break;
            case 404:
                printf("Request ID không tồn tại!\n");
                break;
            case 409:
                printf("Lời mời này đã được xử lý trước đó!\n");
                break;
            case 500:
                printf("Lỗi server!\n");
                break;
            default:
                printf("Lỗi không xác định (Mã: %d)\n", status_code);
        }

        printf("\n");
    }
}

void handle_upload_file(int group_id) {
    if (!is_token_valid()) {
        printf("Bạn cần đăng nhập để upload file!\n");
        return;
    }

    printf("\n--- UPLOAD FILE ---\n");

    int dir_id = 0;

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

void handle_download_file(int group_id) {
    if( !is_token_valid()) {
        printf("Bạn cần đăng nhập để download file!\n");
        return;
    }

    printf("\n--- DOWNLOAD FILE ---\n");
    printf("Group ID: %d\n", group_id);

    int file_id = 0;
    printf("Nhập ID file: ");
    if (scanf("%d", &file_id) != 1) {
        printf("ID file không hợp lệ.\n");
        while (getchar() != '\n');
        return;
    }

    while (getchar() != '\n');

    int sock = connect_to_server();
    if (sock < 0) {
        return;
    }

    // Bắt đầu với chunk 1 để lấy thông tin file
    int chunk_idx = 1;
    int total_chunks = 1;
    int success = 1;
    unsigned char buffer[FILE_CHUNK_SIZE];
    char response[DOWNLOAD_RESPONSE_BUFFER];
    char file_path[PATH_MAX];
    FILE *fp = NULL;
    int file_opened = 0;

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

        // Response format:
        //   "200 chunk_idx/total_chunks file_name base64_data\r\n"
        //   hoặc "202 chunk_idx/total_chunks file_name base64_data\r\n"
        // Tách các token bằng strtok
        char *line = response;
        char *crlf = strstr(line, "\r\n");
        if (crlf) *crlf = '\0';

        char *token_status = strtok(line, " ");
        char *token_chunk = strtok(NULL, " ");
        char *token_filename = strtok(NULL, " ");
        char *token_base64 = strtok(NULL, "");

        if (!token_status || !token_chunk || !token_filename || !token_base64) {
            printf("Phản hồi không hợp lệ: %s\n", response);
            success = 0;
            break;
        }

        status = atoi(token_status);
        if (sscanf(token_chunk, "%d/%d", &resp_chunk, &resp_total) != 2) {
            printf("Phản hồi chunk không hợp lệ: %s\n", token_chunk);
            success = 0;
            break;
        }

        // Mở file lần đầu với tên từ server (ở chunk đầu tiên)
        if (!file_opened) {
            char server_filename[MAX_FILENAME_LEN + 1];
            strncpy(server_filename, token_filename, sizeof(server_filename) - 1);
            server_filename[sizeof(server_filename) - 1] = '\0';

            build_download_path(server_filename, file_path, sizeof(file_path));

            fp = fopen(file_path, "wb");
            if (!fp) {
                printf("Không tạo được file trong thư mục Downloads: %s\n", strerror(errno));
                success = 0;
                break;
            }
            file_opened = 1;
        }

        base64_data = token_base64;

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

    if (fp) {
        fclose(fp);
    }
    if (!success && file_opened) {
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
                    handle_request_join_group();
                    break;
                case 4:
                    handle_view_pending_requests();
                    break;
                case 5:
                    handle_view_my_invitations();
                    break;
                case 6:
                    handle_logout();
                    break;
                case 7:
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
