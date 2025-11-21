#include "io_multiplexing.h"
#include "../net/client.h"
#include "../net/stream.h"
#include "../protocol/command.h"

#include <sys/select.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

#define BUFFER_SIZE 4096

void run_server_loop(int server_sock) {
    fd_set readfds, writefds;
    int max_fd;

    printf("Using I/O Multiplexing with select()...\n");

    while (1) {
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        FD_SET(server_sock, &readfds);
        max_fd = server_sock;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            int sd = clients[i].sock;
            if (sd > 0) {
                FD_SET(sd, &readfds);
                if (clients[i].send_len > clients[i].send_offset)
                    FD_SET(sd, &writefds);

                if (sd > max_fd) max_fd = sd;
            }
        }

        int activity =
            select(max_fd + 1, &readfds, &writefds, NULL, NULL);

        if (activity < 0 && errno != EINTR) {
            perror("select");
            continue;
        }

        // ACCEPT
        if (FD_ISSET(server_sock, &readfds)) {
            struct sockaddr_in client_addr;
            socklen_t len = sizeof(client_addr);
            int client_sock =
                accept(server_sock, (struct sockaddr *)&client_addr, &len);

            if (client_sock >= 0) {
                int idx = add_client(client_sock);
                printf("New client idx=%d fd=%d\n", idx, client_sock);
            }
        }

        // WRITE
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].sock > 0 && FD_ISSET(clients[i].sock, &writefds)) {
                if (flush_send(i) < 0) {
                    remove_client_index(i);
                }
            }
        }

        // READ
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int sd = clients[i].sock;

            if (sd > 0 && FD_ISSET(sd, &readfds)) {
                char tmpbuf[2048];
                ssize_t bytes = recv(sd, tmpbuf, sizeof(tmpbuf), 0);

                if (bytes > 0) {
                    if (clients[i].recv_len + bytes > BUFFER_SIZE) {
                        remove_client_index(i);
                        continue;
                    }

                    memcpy(clients[i].recv_buf + clients[i].recv_len,
                           tmpbuf, bytes);
                    clients[i].recv_len += bytes;

                    int pos;
                    while ((pos = find_crlf(
                                clients[i].recv_buf,
                                clients[i].recv_len)) >= 0)
                    {
                        process_command(i, clients[i].recv_buf, pos);

                        int tail = clients[i].recv_len - (pos + 2);
                        if (tail > 0) {
                            memmove(clients[i].recv_buf,
                                    clients[i].recv_buf + pos + 2,
                                    tail);
                        }
                        clients[i].recv_len = tail;
                    }
                }
                else {
                    remove_client_index(i);
                }
            }
        }
    }
}
