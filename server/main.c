#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "io/io_multiplexing.h"
#include "net/client.h"
#include "database/db.h"
#define PORT 3000
#define BACKLOG 10

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        return;
    }
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main() {
    init_mysql();
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        exit(1);
    }
    set_nonblocking(server_sock);

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }
    if (listen(server_sock, BACKLOG) < 0) {
        perror("listen");
        exit(1);
    }

    init_clients();

    printf("Server listening on port %d...\n", PORT);

    run_server_loop(server_sock);

    close_mysql();
    
    return 0;
}
