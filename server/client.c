#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 1234
#define BUFFER_SIZE 1024

int main() {
    int sock;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];
    char message[BUFFER_SIZE];

    // Tạo socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket failed");
        exit(1);
    }

    // Thiết lập địa chỉ server
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        perror("Invalid address");
        exit(1);
    }

    // Kết nối tới server
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect failed");
        exit(1);
    }

    printf("Connected to server %s:%d\n", SERVER_IP, SERVER_PORT);
    printf("Type messages to send to server (type 'quit' to exit):\n");

    while (1) {
        printf("> ");
        fflush(stdout);

        // Đọc input từ người dùng
        if (fgets(message, sizeof(message), stdin) == NULL) {
            break;
        }

        // Xóa newline
        message[strcspn(message, "\n")] = 0;

        // Kiểm tra lệnh quit
        if (strcmp(message, "quit") == 0) {
            printf("Disconnecting...\n");
            break;
        }

        // Gửi message tới server
        if (send(sock, message, strlen(message), 0) < 0) {
            perror("send failed");
            break;
        }

        // Nhận response từ server
        int bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (bytes_received > 0) {
            buffer[bytes_received] = '\0';
            printf("Server: %s\n", buffer);
        } else if (bytes_received == 0) {
            printf("Server disconnected\n");
            break;
        } else {
            perror("recv failed");
            break;
        }
    }

    close(sock);
    printf("Connection closed.\n");
    return 0;
}
