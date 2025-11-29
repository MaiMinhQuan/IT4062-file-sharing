#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <termios.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 1234
#define BUFFER_SIZE 4096
#define TOKEN_LENGTH 32

// Global token storage
char current_token[TOKEN_LENGTH + 1] = {0};

// Global socket - persistent connection
int global_sock = -1;

// Forward declarations
int connect_to_server();
void handle_create_group();
void handle_list_groups();
void handle_request_join_group();
void handle_view_pending_requests();
void handle_approve_request();

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
        printf("2. View My Groups (Xem nhóm của tôi)\n");
        printf("3. Request Join Group (Xin vào nhóm)\n");
        printf("4. [Admin] View & Approve Requests (Xem & phê duyệt yêu cầu)\n");
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

    printf("\n📂 DANH SÁCH NHÓM CỦA BẠN (%d nhóm)\n\n", group_count);

    if (group_count == 0) {
        printf("⚠️  Bạn chưa tham gia nhóm nào.\n");
        printf("ℹ️  Sử dụng chức năng 1 để tạo nhóm mới hoặc chức năng 3 để xin tham gia nhóm.\n");
        return;
    }

    const char *table_border =
        "┌──────┬──────────────────────────────┬──────────┬─────────────────────┬──────────────────────────────┐\n";
    const char *table_separator =
        "├──────┼──────────────────────────────┼──────────┼─────────────────────┼──────────────────────────────┤\n";
    const char *table_bottom =
        "└──────┴──────────────────────────────┴──────────┴─────────────────────┴──────────────────────────────┘\n";

    printf("%s", table_border);
    printf("│ %-4s │ %-29s │ %-10s │ %-24s │ %-28s │\n",
        "ID", "Tên nhóm", "Vai trò", "Ngày tạo", "Mô tả");
    printf("%s", table_separator);

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

        // Định dạng vai trò với icon
        if (strcmp(safe_role, "admin") == 0) {
            printf("│ %-4.4s │ %-28.28s │ 👑 Admin │ %-19.19s │ %-28.28s │\n",
                   safe_id, safe_name, safe_created, safe_desc);
        } else {
            printf("│ %-4.4s │ %-28.28s │ 👤 Member│ %-19.19s │ %-28.28s │\n",
                   safe_id, safe_name, safe_created, safe_desc);
        }

        if (!next_line) break;
        list_start = next_line + 2;
    }

    printf("%s", table_bottom);
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

    printf("📋 DANH SÁCH CÁC NHÓM CÓ THỂ THAM GIA (%d nhóm)\n", group_count);

    if (group_count == 0) {
        printf("\n⚠️  Không có nhóm nào để tham gia.\n");
        printf("ℹ️  Bạn đã là thành viên của tất cả các nhóm hoặc chưa có nhóm nào được tạo.\n");
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
        printf("↩️  Quay lại menu chính...\n");
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
                printf("✅ Gửi yêu cầu tham gia nhóm #%d thành công!\n", group_id);
                printf("ℹ️  Yêu cầu của bạn đang chờ admin phê duyệt.\n");
                break;
            case 409:
                printf("✗ Bạn đã là thành viên của nhóm #%d rồi!\n", group_id);
                break;
            case 423:
                printf("⚠️  Bạn đã gửi yêu cầu tham gia nhóm #%d trước đó.\n", group_id);
                printf("ℹ️  Vui lòng chờ admin phê duyệt.\n");
                break;
            case 404:
                printf("✗ Nhóm với ID %d không tồn tại!\n", group_id);
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
            printf("\n⚠️  Không có yêu cầu nào đang chờ duyệt.\n");
            printf("ℹ️  Bạn chưa có yêu cầu nào cần xét duyệt trong các nhóm bạn quản lý.\n");
            return;
        }

        printf("📋 DANH SÁCH CÁC YÊU CẦU CHỜ DUYỆT (%d yêu cầu)\n\n", request_count);

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
            printf("↩️  Quay lại menu chính...\n");
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
                        printf("✅ Đã chấp nhận yêu cầu #%d thành công!\n", request_id);
                        printf("ℹ️  User đã được thêm vào nhóm.\n");
                    } else {
                        printf("✅ Đã từ chối yêu cầu #%d thành công!\n", request_id);
                    }
                    break;
                case 403:
                    printf("✗ Bạn không có quyền xét duyệt yêu cầu này!\n");
                    printf("ℹ️  Chỉ admin của nhóm mới có thể xét duyệt.\n");
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

        printf("\n⏳ Đang tải lại danh sách...\n");
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
        printf("↩️  Đã hủy thao tác.\n");
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
                    printf("ℹ️  User đã được thêm vào nhóm.\n");
                } else {
                    printf("✓ Đã từ chối yêu cầu thành công!\n");
                }
                break;
            case 409:
                printf("✗ Yêu cầu này đã được xử lý trước đó rồi!\n");
                break;
            case 403:
                printf("✗ Bạn không có quyền xử lý yêu cầu này!\n");
                printf("ℹ️  Chỉ admin của nhóm mới có thể phê duyệt.\n");
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
