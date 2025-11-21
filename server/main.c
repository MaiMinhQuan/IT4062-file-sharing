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
#define PORT 1234
#define BACKLOG 10

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main() {
    init_mysql();
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);

    set_nonblocking(server_sock);

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(server_sock, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_sock, BACKLOG);

    init_clients();

    printf("Server listening on port %d...\n", PORT);

    run_server_loop(server_sock);

    close_mysql();
    
    return 0;
}
