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
        printf("3. Logout (Đăng xuất)\n");
        printf("4. Exit (Thoát)\n");
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
                    handle_logout();
                    break;
                case 4:
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
