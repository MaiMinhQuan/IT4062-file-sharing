#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>
#include "database/db.h"

#define PORT 1234
#define BACKLOG 10
#define BUFFER_SIZE 1024
#define MAX_CLIENTS 30

void handle_client_data(int client_sock);

int main() {
    int server_sock, client_sock, max_sd, sd, activity;
    int client_sockets[MAX_CLIENTS];
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len;
    fd_set readfds;

    init_mysql();

    // Khởi tạo mảng client sockets
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_sockets[i] = 0;
    }

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket failed");
        exit(1);
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        exit(1);
    }

    if (listen(server_sock, BACKLOG) < 0) {
        perror("listen failed");
        exit(1);
    }

    printf("Server listening on port %d...\n", PORT);
    printf("Using I/O Multiplexing with select()...\n");

    while (1) {
        // Xóa fd_set và thêm server socket
        FD_ZERO(&readfds);
        FD_SET(server_sock, &readfds);
        max_sd = server_sock;

        // Thêm các client sockets vào fd_set
        for (int i = 0; i < MAX_CLIENTS; i++) {
            sd = client_sockets[i];
            
            if (sd > 0) {
                FD_SET(sd, &readfds);
            }
            
            if (sd > max_sd) {
                max_sd = sd;
            }
        }

        // Chờ activity trên các sockets (blocking nhưng theo dõi nhiều socket)
        activity = select(max_sd + 1, &readfds, NULL, NULL, NULL);

        if (activity < 0) {
            perror("select error");
            continue;
        }

        // Nếu có connection mới trên server socket
        if (FD_ISSET(server_sock, &readfds)) {
            client_addr_len = sizeof(client_addr);
            client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_addr_len);

            if (client_sock < 0) {
                perror("accept failed");
                continue;
            }

            printf("New client connected! Socket fd: %d\n", client_sock);

            // Thêm client mới vào mảng
            int added = 0;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (client_sockets[i] == 0) {
                    client_sockets[i] = client_sock;
                    printf("Added to client list at index %d\n", i);
                    added = 1;
                    break;
                }
            }

            if (!added) {
                printf("Max clients reached. Connection rejected.\n");
                close(client_sock);
            }
        }

        // Kiểm tra I/O operations trên các client sockets
        for (int i = 0; i < MAX_CLIENTS; i++) {
            sd = client_sockets[i];

            if (FD_ISSET(sd, &readfds)) {
                char buffer[BUFFER_SIZE];
                int bytes_read = recv(sd, buffer, sizeof(buffer) - 1, 0);

                if (bytes_read <= 0) {
                    // Client ngắt kết nối
                    if (bytes_read == 0) {
                        printf("Client disconnected. Socket fd: %d\n", sd);
                    } else {
                        perror("recv error");
                    }
                    close(sd);
                    client_sockets[i] = 0;
                } else {
                    // Xử lý dữ liệu từ client
                    buffer[bytes_read] = '\0';
                    printf("Received from socket %d: %s\n", sd, buffer);
                    
                    // Echo back
                    char response[BUFFER_SIZE];
                    int len = snprintf(response, sizeof(response), "Server received: %s", buffer);
                    if (len > 0 && len < sizeof(response)) {
                        send(sd, response, len, 0);
                    } else {
                        send(sd, "Server received (message too long)", 34, 0);
                    }
                }
            }
        }
    }

    close_mysql();
    close(server_sock);
    return 0;
}
